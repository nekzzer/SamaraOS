#include "core/smp.h"
#include "core/vmm.h"
#include "core/string.h"
#include "core/task.h"
#include "core/heap.h"
#include "core/swap.h"
#include "boot/paging.h"

/* ---------------- physical frame pool ---------------- */

/* refcnt per frame, indexed by pfn. 0xFF = not ours (kernel, holes, mmio) */
static uint8_t* refcnt;
static uint64_t max_pfn, pool_frames, pool_free, hint;
static spin_t pl;
/* page tables of one address space, hashed by pd. never nested */
static spin_t mml[16];
#define MML(pd) (&mml[((pd) >> 12) & 15])
/* where the last search ended, per space (hashed like mml, same lock). freeing below it pulls it back */
static struct { uint64_t pd, lo, cache; } fc[16];

void pmm_init(uint64_t top) {
    max_pfn = top >> 12;
    refcnt = (uint8_t*)kmalloc(max_pfn);
    memset(refcnt, 0xFF, max_pfn);
    pool_frames = pool_free = hint = 0;
}

void pmm_add(uint64_t start, uint64_t end) {
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    for (uint64_t a = start; a < end && (a >> 12) < max_pfn; a += PAGE_SIZE) {
        refcnt[a >> 12] = 0;
        pool_frames++;
        pool_free++;
    }
}

uint64_t pmm_free_frames(void)  { return pool_free; }
uint64_t pmm_total_frames(void) { return pool_frames; }

uint64_t pmm_alloc(void) {
    uint64_t f = spin_lock(&pl);
    uint64_t frame = 0;
    for (uint64_t n = 0; n < max_pfn; n++) {
        uint64_t i = (hint + n) % max_pfn;
        if (refcnt[i]) continue;
        refcnt[i] = 1;
        pool_free--;
        hint = i + 1;
        frame = i << 12;
        break;
    }
    spin_unlock(&pl, f);
    if (frame) memset(P2V(frame), 0, PAGE_SIZE);
    return frame;
}

/* n frames in a row (io_uring rings want one flat kernel view of them) */
uint64_t pmm_alloc_run(uint64_t n) {
    uint64_t f = spin_lock(&pl);
    uint64_t base = 0;
    for (uint64_t i = 0, run = 0; i < max_pfn; i++) {
        run = refcnt[i] ? 0 : run + 1;
        if (run < n) continue;
        uint64_t s = i + 1 - n;
        for (uint64_t k = 0; k < n; k++) refcnt[s + k] = 1;
        pool_free -= n;
        base = s << 12;
        break;
    }
    spin_unlock(&pl, f);
    if (base) memset(P2V(base), 0, n * PAGE_SIZE);
    return base;
}

static bool ours(uint64_t frame) {
    uint64_t i = frame >> 12;
    return i < max_pfn && refcnt[i] != 0xFF;
}

int pmm_refcnt(uint64_t frame) { return ours(frame) ? refcnt[frame >> 12] : 0; }

/* 254 is sticky: stays, never freed (too many forks of one page) */
void pmm_ref(uint64_t frame) {
    uint64_t f = spin_lock(&pl);
    if (ours(frame) && refcnt[frame >> 12] < 254) refcnt[frame >> 12]++;
    spin_unlock(&pl, f);
}

void pmm_unref(uint64_t frame) {
    uint64_t f = spin_lock(&pl);
    if (ours(frame) && refcnt[frame >> 12] && refcnt[frame >> 12] != 254 && --refcnt[frame >> 12] == 0) pool_free++;
    spin_unlock(&pl, f);
}

/* ---------------- page tables ---------------- */

#define IDX(va, lvl) (((va) >> (12 + 9 * (lvl))) & 0x1FF)

void vmm_flush(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    if (ncpu > 1) tlb_shootdown();
}

static uint64_t* pte_slot(uint64_t pd, uint64_t va, bool create);
static uint64_t hole_at(uint64_t pd, uint64_t va);
#define SLOT(e) (((e) & PTE_ADDR) >> 12)

/* every change of a live leaf pte goes through here. other cpus may hold the old pte
   (threads of one process), so they get a shootdown too. range loops batch it: sd_defer */
static int sd_defer[MAX_CPUS], sd_need[MAX_CPUS];
static uint64_t sd_pd[MAX_CPUS];       /* space the locked section works on */
#define SD_DEFER sd_defer[this_cpu()->id]
#define SD_NEED sd_need[this_cpu()->id]
#define SD_PD sd_pd[this_cpu()->id]

static void tlb_inval(uint64_t va) {
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
    if (ncpu < 2) return;
    if (SD_DEFER) SD_NEED = 1;
    else tlb_shootdown_pd(SD_PD);
}

static uint64_t MMLOCK(uint64_t pd) {
    uint64_t f = spin_lock(MML(pd));
    SD_PD = pd;
    return f;
}

static void sd_end(void) {
    if (--SD_DEFER == 0 && SD_NEED) { SD_NEED = 0; tlb_shootdown_pd(SD_PD); }
}

/* not present before: nobody can have it cached */
static void pte_put(uint64_t* p, uint64_t va, uint64_t v) {
    uint64_t old = *p;
    if (old == v) return;
    *p = v;
    if (old & PTE_P) tlb_inval(va);
    else __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

/* writable now: a frame that somebody else holds too waits for the write fault */
static void make_writable(uint64_t* p, uint64_t va) {
    uint64_t e = *p;
    if ((e & PTE_P) && !(e & PTE_SHARED) && ours(e & PTE_ADDR) && refcnt[(e & PTE_ADDR) >> 12] > 1)
        pte_put(p, va, (e & ~PTE_RW) | PTE_COW);
    else
        pte_put(p, va, (e & ~PTE_COW) | PTE_RW);
}

static bool vmm_cow_nl(uint64_t pd, uint64_t va) {
    va &= ~(PAGE_SIZE - 1);
    uint64_t* p = pte_slot(pd, va, false);
    if (!p || !(*p & PTE_P) || !(*p & PTE_COW)) return false;
    uint64_t e = *p, fr = e & PTE_ADDR;
    if (refcnt[fr >> 12] != 1) {
        uint64_t nf = pmm_alloc();
        if (!nf) return false;
        memcpy(P2V(nf), P2V(fr), PAGE_SIZE);
        pte_put(p, va, nf | (e & ~PTE_ADDR & ~PTE_COW) | PTE_RW);
        pmm_unref(fr);
        return true;
    }
    pte_put(p, va, (e & ~PTE_COW) | PTE_RW);
    return true;
}

bool vmm_cow(uint64_t pd, uint64_t va) {
    uint64_t f = MMLOCK(pd);
    bool r = vmm_cow_nl(pd, va);
    spin_unlock(MML(pd), f);
    return r;
}

uint64_t vmm_new_space(void) {
    uint64_t pd = pmm_alloc();
    if (!pd) return 0;
    const uint64_t* k = (const uint64_t*)P2V(task_kernel_cr3());
    uint64_t* d = (uint64_t*)P2V(pd);
    for (int i = 256; i < 512; i++) d[i] = k[i];
    return pd;
}

/* walk down to the pte for va. next level tables are made on the way when `create` */
static uint64_t* pte_slot(uint64_t pd, uint64_t va, bool create) {
    uint64_t* t = (uint64_t*)P2V(pd);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t* e = &t[IDX(va, lvl)];
        if (!(*e & PTE_P)) {
            if (lvl == 1 && (*e & PTE_LAZY)) {         /* whole 2M reserved PROT_NONE, make the real table now */
                uint64_t n = pmm_alloc();
                if (!n) return NULL;
                uint64_t* pt = (uint64_t*)P2V(n);
                for (int i = 0; i < 512; i++) pt[i] = PTE_LAZY;
                *e = n | PTE_P | PTE_RW | PTE_US;
                t = pt;
                continue;
            }
            if (!create) return NULL;
            uint64_t n = pmm_alloc();
            if (!n) return NULL;
            *e = n | PTE_P | PTE_RW | PTE_US;
        }
        t = (uint64_t*)P2V(*e & PTE_ADDR);
    }
    return &t[IDX(va, 0)];
}

static bool uaddr(uint64_t a) { return a >= USER_BASE && a < USER_TOP; }

/* the pde for va (the 2M slot), tables above it made when `create` */
static uint64_t* pde_slot(uint64_t pd, uint64_t va, bool create) {
    uint64_t* t = (uint64_t*)P2V(pd);
    for (int lvl = 3; lvl > 1; lvl--) {
        uint64_t* e = &t[IDX(va, lvl)];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            uint64_t n = pmm_alloc();
            if (!n) return NULL;
            *e = n | PTE_P | PTE_RW | PTE_US;
        }
        t = (uint64_t*)P2V(*e & PTE_ADDR);
    }
    return &t[IDX(va, 1)];
}

/* chromium reserves 64G of PROT_NONE for partition_alloc in every process, a pte per page
   was 128M of tables each and seconds under the bkl. an untouched 2M slot is one marker now */
static bool pde_res(uint64_t pd, uint64_t va) {
    uint64_t* e = pde_slot(pd, va, false);
    return e && !(*e & PTE_P) && (*e & PTE_LAZY);
}

uint64_t vmm_pte(uint64_t pd, uint64_t va) {
    if (!uaddr(va)) return 0;
    uint64_t* p = pte_slot(pd, va, false);
    return p ? *p : 0;
}

// non present pte going away: swap slot or lazy shm page ref
static void pte_rel(uint64_t e) {
    if (e & PTE_SWAP) swap_put(SLOT(e));
    else if (e & PTE_SHL) shm_lz_put(e);
}

static int vmm_alloc_range_nl(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int ret = 0;
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) { ret = -1; break; }
        uint64_t* p = pte_slot(pd, a, true);
        if (!p) { ret = -1; break; }
        if (*p & PTE_P) {
            if (writable) make_writable(p, a);
            continue;
        }
        if (*p & PTE_SWAP) { if (writable) *p |= PTE_RW; continue; }
        uint64_t fr = pmm_alloc();
        if (!fr) { ret = -1; break; }
        if (*p & PTE_SHL) shm_lz_put(*p);
        pte_put(p, a, fr | PTE_P | PTE_US | (writable ? PTE_RW : 0));
    }
    sd_end();
    return ret;
}

// brk grow: 1 if something is mapped there already, -1 when frames ran out
int vmm_alloc_free(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t f = MMLOCK(pd);
    int r = vmm_range_unmapped(pd, va, len) ? vmm_alloc_range_nl(pd, va, len, writable) : 1;
    spin_unlock(MML(pd), f);
    return r;
}

int vmm_alloc_range(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t f = MMLOCK(pd);
    int r = vmm_alloc_range_nl(pd, va, len, writable);
    spin_unlock(MML(pd), f);
    return r;
}

static void vmm_set_writable_nl(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t h = hole_at(pd, a);
        if (h) { a = (a & ~(h - 1)) + h - PAGE_SIZE; continue; }
        if (!writable && !(a & 0x1FFFFF) && a + 0x200000 <= end && pde_res(pd, a)) { a += 0x200000 - PAGE_SIZE; continue; }
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & PTE_USED)) continue;
        if (!writable) pte_put(p, a, *p & ~(PTE_RW | PTE_COW));
        else if (*p & PTE_P) make_writable(p, a);
        else pte_put(p, a, *p | PTE_RW);
    }
    sd_end();
}

void vmm_set_writable(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t f = MMLOCK(pd);
    vmm_set_writable_nl(pd, va, len, writable);
    spin_unlock(MML(pd), f);
}

static int vmm_map_frame_nl(uint64_t pd, uint64_t va, uint64_t fr, bool rw) {
    uint64_t* p = pte_slot(pd, va, true);
    if (!p) return -1;
    pmm_ref(fr);
    if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
    else pte_rel(*p);
    pte_put(p, va, fr | PTE_P | PTE_US | (rw ? PTE_RW : 0) | PTE_SHARED);   /* every caller maps something shared */
    return 0;
}

int vmm_map_frame(uint64_t pd, uint64_t va, uint64_t fr, bool rw) {
    uint64_t f = MMLOCK(pd);
    int r = vmm_map_frame_nl(pd, va, fr, rw);
    spin_unlock(MML(pd), f);
    return r;
}

int vmm_map_cache(uint64_t pd, uint64_t va, uint64_t fr, bool w, bool shared) {
    uint64_t f = MMLOCK(pd);
    int r = -1;
    uint64_t* p = pte_slot(pd, va, true);
    if (p) {
        pmm_ref(fr);
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        else pte_rel(*p);
        uint64_t fl = shared ? PTE_SHARED | (w ? PTE_RW : 0) : (w ? PTE_COW : 0);
        pte_put(p, va, fr | PTE_P | PTE_US | fl);
        r = 0;
    }
    spin_unlock(MML(pd), f);
    return r;
}

/* java reserves hundreds of MB (heap, metaspace, thread stacks, code cache)
   and touches a fraction. every byte of it used to be a real frame up
   front: minecraft ran the box out of memory while making the world */
static int vmm_lazy_range_nl(uint64_t pd, uint64_t va, uint64_t len, bool rw, bool user) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int ret = 0;
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) { ret = -1; break; }
        if (!rw && !user && !(a & 0x1FFFFF) && a + 0x200000 <= end) {
            uint64_t* pde = pde_slot(pd, a, true);
            if (pde && (!*pde || *pde == PTE_LAZY)) { *pde = PTE_LAZY; a += 0x200000 - PAGE_SIZE; continue; }
        }
        uint64_t* p = pte_slot(pd, a, true);
        if (!p) { ret = -1; break; }
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        else pte_rel(*p);
        pte_put(p, a, PTE_LAZY | (rw ? PTE_RW : 0) | (user ? PTE_US : 0));
    }
    sd_end();
    return ret;
}

int vmm_lazy_shm(uint64_t pd, uint64_t va, uint64_t len, bool rw, bool user, int slot, uint64_t first) {
    uint64_t f = MMLOCK(pd);
    int ret = 0;
    SD_DEFER++;
    for (uint64_t k = 0; k < len / PAGE_SIZE; k++) {
        uint64_t a = va + k * PAGE_SIZE;
        uint64_t* p = pte_slot(pd, a, true);
        if (!p) { ret = -1; break; }
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        else pte_rel(*p);
        uint64_t e = PTE_LAZY | PTE_SHL | (rw ? PTE_RW : 0) | (user ? PTE_US : 0) | ((uint64_t)slot << 40) | ((first + k) << 12);
        shm_lz_dup(e);
        pte_put(p, a, e);
    }
    sd_end();
    spin_unlock(MML(pd), f);
    return ret;
}

int vmm_lazy_range(uint64_t pd, uint64_t va, uint64_t len, bool rw, bool user) {
    uint64_t f = MMLOCK(pd);
    int r = vmm_lazy_range_nl(pd, va, len, rw, user);
    spin_unlock(MML(pd), f);
    return r;
}

bool vmm_fault_in(uint64_t pd, uint64_t va) {
    va &= ~(PAGE_SIZE - 1);
    uint64_t fr = pmm_alloc();                     // zeroing a page under mml was most of the hold time
    uint64_t f = MMLOCK(pd);
    uint64_t* p = pte_slot(pd, va, false);
    uint64_t old = p ? *p : 0;
    bool r = false, sw = (old & (PTE_P | PTE_SWAP)) == PTE_SWAP;
    if (p && (old & PTE_P)) r = true;              /* other thread was faster */
    else if (p && (old & PTE_SHL) && fr) {
        // lazy shm page: the frame comes from the segment, ours is only a spare
        uint64_t t = shm_lz_frame(old, fr);
        pmm_ref(t);
        if (__sync_bool_compare_and_swap(p, old, t | PTE_P | PTE_US | PTE_SHARED | (old & PTE_RW))) shm_lz_put(old);
        else pmm_unref(t);
        if (t == fr) fr = 0;
        r = true;
    }
    else if (p && (old & PTE_LAZY) && fr) {
        /* two threads on one fresh page: the loser must not overwrite what the winner already wrote */
        if (__sync_bool_compare_and_swap(p, old, fr | PTE_P | (old & (PTE_RW | PTE_US)))) fr = 0;
        r = true;
    }
    spin_unlock(MML(pd), f);
    if (fr) pmm_unref(fr);
    return sw ? vmm_swap_in(pd, va) : r;
}

/* the disk read happens with no lock held. the pte is looked at again after it:
   a thread of the same process may have brought the page back (or unmapped it) meanwhile */
bool vmm_swap_in(uint64_t pd, uint64_t va) {
    va &= ~(PAGE_SIZE - 1);
    for (int tries = 0; tries < 4; tries++) {
        uint64_t f = MMLOCK(pd);
        uint64_t* p = pte_slot(pd, va, false);
        uint64_t e = p ? *p : 0;
        spin_unlock(MML(pd), f);
        if (e & PTE_P) return true;
        if (!(e & PTE_SWAP)) return false;
        uint64_t fr = pmm_alloc();
        if (!fr) return false;
        if (swap_read(SLOT(e), P2V(fr)) < 0) { pmm_unref(fr); return false; }
        f = MMLOCK(pd);
        p = pte_slot(pd, va, false);
        if (p && *p == e) {
            pte_put(p, va, fr | PTE_P | (e & (PTE_RW | PTE_US)));
            spin_unlock(MML(pd), f);
            swap_put(SLOT(e));
            return true;
        }
        spin_unlock(MML(pd), f);
        pmm_unref(fr);
    }
    return false;
}

// PROT_NONE = present but supervisor only, so user access faults
static void vmm_set_user_nl(uint64_t pd, uint64_t va, uint64_t len, bool user) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t h = hole_at(pd, a);
        if (h) { a = (a & ~(h - 1)) + h - PAGE_SIZE; continue; }
        if (!user && !(a & 0x1FFFFF) && a + 0x200000 <= end && pde_res(pd, a)) { a += 0x200000 - PAGE_SIZE; continue; }
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & PTE_USED)) continue;
        pte_put(p, a, user ? *p | PTE_US : *p & ~PTE_US);
    }
    sd_end();
}

void vmm_set_user(uint64_t pd, uint64_t va, uint64_t len, bool user) {
    uint64_t f = MMLOCK(pd);
    vmm_set_user_nl(pd, va, len, user);
    spin_unlock(MML(pd), f);
}

/* defer: frames wait in dfr until the other cpus have flushed, the caller does that after
   dropping the lock (a shootdown under mml stalled every fault of the process behind an ipi round trip) */
static uint64_t dfr[MAX_CPUS][256];
static int ndf[MAX_CPUS];

static void dfr_flush(void) {
    int c = this_cpu()->id;
    if (SD_NEED) { SD_NEED = 0; tlb_shootdown_pd(SD_PD); }
    for (int i = 0; i < ndf[c]; i++) pmm_unref(dfr[c][i]);
    ndf[c] = 0;
}

static void vmm_free_range_nl(uint64_t pd, uint64_t va, uint64_t len, bool defer) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int me = this_cpu()->id;
    if (fc[(pd >> 12) & 15].pd == pd && va < fc[(pd >> 12) & 15].cache) fc[(pd >> 12) & 15].cache = va & ~(PAGE_SIZE - 1);
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) continue;
        uint64_t h = hole_at(pd, a);
        if (h) { a = (a & ~(h - 1)) + h - PAGE_SIZE; continue; }
        if (!(a & 0x1FFFFF) && a + 0x200000 <= end && pde_res(pd, a)) { *pde_slot(pd, a, false) = 0; a += 0x200000 - PAGE_SIZE; continue; }
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & PTE_USED)) continue;
        if ((*p & PTE_P) && defer && ncpu > 1) {
            if (ndf[me] == 256) dfr_flush();
            dfr[me][ndf[me]++] = *p & PTE_ADDR;
        }
        else if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        else pte_rel(*p);
        pte_put(p, a, 0);
    }
    if (defer) SD_DEFER--;
    else sd_end();
}

void vmm_free_range(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t fi = irq_save();           // stays off until the frames are gone, per cpu state in there
    uint64_t f = MMLOCK(pd);
    vmm_free_range_nl(pd, va, len, true);
    spin_unlock(MML(pd), f);
    dfr_flush();
    irq_restore(fi);
}

// madvise(DONTNEED): private writable pages go back to lazy zero. partitionalloc in chromium
// takes decommitted memory for zeroed and skips the memset, no-op here gave it garbage
void vmm_discard(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int me;
    uint64_t fi = irq_save();
    uint64_t f = MMLOCK(pd);
    me = this_cpu()->id;
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) continue;
        uint64_t h = hole_at(pd, a);
        if (h) { a = (a & ~(h - 1)) + h - PAGE_SIZE; continue; }
        if (!(a & 0x1FFFFF) && a + 0x200000 <= end && pde_res(pd, a)) { a += 0x200000 - PAGE_SIZE; continue; }
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || (*p & (PTE_P | PTE_RW | PTE_SHARED)) != (PTE_P | PTE_RW)) continue;
        uint64_t e = *p;
        if (ncpu > 1) {
            if (ndf[me] == 256) dfr_flush();
            dfr[me][ndf[me]++] = e & PTE_ADDR;
        }
        else pmm_unref(e & PTE_ADDR);
        pte_put(p, a, PTE_LAZY | PTE_RW | (e & PTE_US));
    }
    SD_DEFER--;
    spin_unlock(MML(pd), f);
    dfr_flush();
    irq_restore(fi);
}

bool vmm_range_unmapped(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t h = hole_at(pd, a);
        if (h) { a = (a & ~(h - 1)) + h - PAGE_SIZE; continue; }
        if (pde_res(pd, a)) return false;
        uint64_t* p = pte_slot(pd, a, false);
        if (p && (*p & PTE_USED)) return false;
    }
    return true;
}

/* how much address space from va up is certainly empty because an upper
   level entry is missing, 0 if the walk reaches a pte table */
static uint64_t hole_at(uint64_t pd, uint64_t va) {
    uint64_t* t = (uint64_t*)P2V(pd);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t e = t[IDX(va, lvl)];
        if (!(e & PTE_P)) return lvl == 1 && (e & PTE_LAZY) ? 0 : 1ul << (12 + 9 * lvl);
        t = (uint64_t*)P2V(e & PTE_ADDR);
    }
    return 0;
}

// first run of len free bytes in [from, limit), 0 if none. walks the pd pages directly
static uint64_t scan_free(uint64_t pd, uint64_t from, uint64_t limit, uint64_t len) {
    uint64_t run = 0, start = from, a = from;
    uint64_t* t3 = (uint64_t*)P2V(pd);
    while (a < limit) {
        uint64_t e3 = t3[IDX(a, 3)];
        if (!(e3 & PTE_P)) {
            uint64_t nx = (a | 0x7FFFFFFFFF) + 1;
            if (!run) start = a;
            run += nx - a; a = nx;
            if (run >= len) return start;
            continue;
        }
        uint64_t* t2 = (uint64_t*)P2V(e3 & PTE_ADDR);
        uint64_t e2 = t2[IDX(a, 2)];
        if (!(e2 & PTE_P)) {
            uint64_t nx = (a | 0x3FFFFFFF) + 1;
            if (!run) start = a;
            run += nx - a; a = nx;
            if (run >= len) return start;
            continue;
        }
        uint64_t* t1 = (uint64_t*)P2V(e2 & PTE_ADDR);
        // a whole pd page: walk its 512 slots straight
        uint64_t gb = (a | 0x3FFFFFFF) + 1;
        for (int i = IDX(a, 1); i < 512 && a < limit; i++) {
            uint64_t e = t1[i];
            if (!(e & PTE_P)) {
                if (e & PTE_LAZY) { run = 0; a = (a | 0x1FFFFF) + 1; continue; }
                if (!run) start = a;
                run += ((a | 0x1FFFFF) + 1) - a;
                a = (a | 0x1FFFFF) + 1;
                if (run >= len) return start;
                continue;
            }
            uint64_t* pt = (uint64_t*)P2V(e & PTE_ADDR);
            for (int j = IDX(a, 0); j < 512 && a < limit; j++, a += PAGE_SIZE) {
                if (pt[j] & PTE_USED) { run = 0; continue; }
                if (!run) start = a;
                run += PAGE_SIZE;
                if (run >= len) return start;
            }
        }
        a = gb;
    }
    return 0;
}

// caller holds the mm lock
static uint64_t find_free_nl(uint64_t pd, uint64_t from, uint64_t limit, uint64_t len) {
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int h = (pd >> 12) & 15;
    if (fc[h].pd != pd || fc[h].lo != from) { fc[h].pd = pd; fc[h].lo = from; fc[h].cache = from; }
    uint64_t c = fc[h].cache;
    uint64_t r = 0;
    if (c > from && c < limit) r = scan_free(pd, c, limit, len);
    if (!r) r = scan_free(pd, from, limit, len);
    if (r) fc[h].cache = r + len;
    return r;
}

uint64_t vmm_find_free(uint64_t pd, uint64_t from, uint64_t limit, uint64_t len) {
    uint64_t f = MMLOCK(pd);
    uint64_t r = find_free_nl(pd, from, limit, len);
    spin_unlock(MML(pd), f);
    return r;
}

/* anon mmap for the threads that mmap at once: find the hole and claim it under one lock */
uint64_t vmm_map_anon(uint64_t pd, uint64_t addr, bool fixed, uint64_t lo, uint64_t hi, uint64_t len, bool rw, bool user) {
    uint64_t f = MMLOCK(pd);
    if (fixed) vmm_free_range_nl(pd, addr, len, false);
    else if (!(addr && !(addr & 0xFFF) && addr >= lo && addr + len <= hi && vmm_range_unmapped(pd, addr, len)))
        addr = find_free_nl(pd, lo, hi, len);
    if (addr && vmm_lazy_range_nl(pd, addr, len, rw, user) < 0) { vmm_free_range_nl(pd, addr, len, false); addr = 0; }
    spin_unlock(MML(pd), f);
    return addr;
}

/* same hole search as vmm_map_anon but leaves a PROT_NONE lazy claim, the real mapping overwrites it.
   find + mmap in two steps raced with the lock-free anon mmap of another thread (es2gears, malloc vs xkb files) */
uint64_t vmm_reserve(uint64_t pd, uint64_t addr, uint64_t lo, uint64_t hi, uint64_t len) {
    uint64_t f = MMLOCK(pd);
    if (!(addr && !(addr & 0xFFF) && addr >= lo && addr + len <= hi && vmm_range_unmapped(pd, addr, len)))
        addr = find_free_nl(pd, lo, hi, len);
    if (addr && vmm_lazy_range_nl(pd, addr, len, false, false) < 0) { vmm_free_range_nl(pd, addr, len, false); addr = 0; }
    spin_unlock(MML(pd), f);
    return addr;
}

static void free_tables(uint64_t tbl, int lvl) {
    uint64_t* t = (uint64_t*)P2V(tbl);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) {
            if (lvl == 0) pte_rel(t[i]);
            continue;
        }
        if (lvl == 0) pmm_unref(t[i] & PTE_ADDR);
        else free_tables(t[i] & PTE_ADDR, lvl - 1);
    }
    pmm_unref(tbl);
}

void vmm_destroy_space(uint64_t pd) {
    if (ncpu > 1) tlb_unload(pd);
    uint64_t f = MMLOCK(pd);
    if (fc[(pd >> 12) & 15].pd == pd) fc[(pd >> 12) & 15].pd = 0;
    spin_unlock(MML(pd), f);
    uint64_t* d = (uint64_t*)P2V(pd);
    for (int i = 0; i < 256; i++)
        if (d[i] & PTE_P) { free_tables(d[i] & PTE_ADDR, 2); d[i] = 0; }
    pmm_unref(pd);
}

static bool clone_level(uint64_t src, uint64_t dst, int lvl, uint64_t base) {
    uint64_t* s = (uint64_t*)P2V(src);
    uint64_t* d = (uint64_t*)P2V(dst);
    for (int i = 0; i < 512; i++) {
        uint64_t e = s[i];
        if (lvl > 0) {
            if (!(e & PTE_P)) { if (lvl == 1 && (e & PTE_LAZY)) d[i] = e; continue; }
            uint64_t n = pmm_alloc();
            if (!n) return false;
            d[i] = n | (e & 0xFFF);
            if (!clone_level(e & PTE_ADDR, n, lvl - 1, base | ((uint64_t)i << (12 + 9 * lvl)))) return false;
            continue;
        }
        if (!(e & PTE_P)) {
            if (e & PTE_LAZY) { if (e & PTE_SHL) shm_lz_dup(e); d[i] = e; }   /* untouched: stays lazy */
            else if (e & PTE_SWAP) { swap_dup(SLOT(e)); d[i] = e; }
            continue;
        }
        uint64_t fr = e & PTE_ADDR;
        /* device pages (the lfb mapped by xorg) are shared as they are, shm/memfd too.
           private writable ones are cow now: both sides lose RW, the first write copies */
        if (PTE_WR(e) && ours(fr) && !(e & PTE_SHARED)) {
            e = (e & ~PTE_RW) | PTE_COW;
            s[i] = e;
            tlb_inval(base | ((uint64_t)i << 12));
        }
        pmm_ref(fr);
        d[i] = e;
    }
    return true;
}

uint64_t vmm_clone_space(uint64_t pd) {
    uint64_t npd = vmm_new_space();
    if (!npd) return 0;
    uint64_t* s = (uint64_t*)P2V(pd);
    uint64_t* d = (uint64_t*)P2V(npd);
    uint64_t fl = MMLOCK(pd);
    SD_DEFER++;
    for (int i = 0; i < 256; i++) {
        if (!(s[i] & PTE_P)) continue;
        uint64_t n = pmm_alloc();
        if (!n) goto fail;
        d[i] = n | (s[i] & 0xFFF);
        if (!clone_level(s[i] & PTE_ADDR, n, 2, (uint64_t)i << 39)) goto fail;
    }
    sd_end();      /* siblings of the parent on other cpus must not write through a stale RW */
    spin_unlock(MML(pd), fl);
    return npd;
fail:
    sd_end();
    spin_unlock(MML(pd), fl);
    vmm_destroy_space(npd);
    return 0;
}

int vmm_copy_to(uint64_t pd, uint64_t va, const void* src, uint64_t len) {
    const uint8_t* s = (const uint8_t*)src;
    while (len) {
        uint64_t pte = vmm_pte(pd, va);
        if (!(pte & PTE_P)) {
            if (!vmm_fault_in(pd, va)) return -1;
            pte = vmm_pte(pd, va);
        }
        if (pte & PTE_COW) {
            if (!vmm_cow(pd, va)) return -1;
            pte = vmm_pte(pd, va);
        }
        uint64_t off = va & 0xFFF;
        uint64_t n = PAGE_SIZE - off;
        if (n > len) n = len;
        uint8_t* dst = (uint8_t*)P2V(pte & PTE_ADDR) + off;
        if (s) { memcpy(dst, s, n); s += n; }
        else   memset(dst, 0, n);
        va += n; len -= n;
    }
    return 0;
}

static uint64_t count_level(uint64_t tbl, int lvl) {
    uint64_t* t = (uint64_t*)P2V(tbl);
    uint64_t n = 0;
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) continue;
        n += lvl ? count_level(t[i] & PTE_ADDR, lvl - 1) : 1;
    }
    return n;
}

// like count_level but lazy and swapped pages count too (rlimit AS)
static uint64_t count_virt(uint64_t tbl, int lvl) {
    uint64_t* t = (uint64_t*)P2V(tbl);
    uint64_t n = 0;
    for (int i = 0; i < 512; i++) {
        if (!lvl) { if (t[i] & PTE_USED) n++; continue; }
        if (t[i] & PTE_P) n += count_virt(t[i] & PTE_ADDR, lvl - 1);
    }
    return n;
}

uint64_t vmm_count_virt(uint64_t pd) {
    uint64_t* d = (uint64_t*)P2V(pd);
    uint64_t n = 0;
    for (int i = 0; i < 256; i++)
        if (d[i] & PTE_P) n += count_virt(d[i] & PTE_ADDR, 2);
    return n;
}

uint64_t vmm_count_pages(uint64_t pd) {
    uint64_t* d = (uint64_t*)P2V(pd);
    uint64_t n = 0;
    for (int i = 0; i < 256; i++)
        if (d[i] & PTE_P) n += count_level(d[i] & PTE_ADDR, 2);
    return n;
}

/* below 4G the direct map already covers it. above: hang a 1G slot of 2M
   uncached pages into the direct map (pools of 4, bars up there are rare) */
extern uint64_t boot_pdpt_a[], boot_pml4[];
void* mmio_map(uint64_t pa, size_t len) {
    static uint64_t pd[4][512] __attribute__((aligned(4096)));
    static int used;
    if (pa + len <= 0x100000000ull) return P2V(pa);
    if (pa >= 1ull << 39) {
        // venus hostmem puts the bar at 512G+, its own pml4 slot (dmap + pa lands there)
        static uint64_t hp[512] __attribute__((aligned(4096))), hd[4][512] __attribute__((aligned(4096)));
        static int hu, hs;
        uint64_t sl = pa >> 39;
        if (sl > 200 || sl != ((pa + len - 1) >> 39) || (hs && hs != (int)sl)) return NULL;
        if (!hs) { hs = (int)sl; boot_pml4[256 + sl] = V2P(hp) | 3; }
        for (uint64_t g = (pa >> 30) & 511; g <= (((pa + len - 1) >> 30) & 511); g++) {
            if (hp[g] & PTE_P) continue;
            if (hu == 4) return NULL;
            for (int i = 0; i < 512; i++) hd[hu][i] = ((sl << 39) | (g << 30)) | ((uint64_t)i << 21) | 0x9b;
            hp[g] = V2P(hd[hu++]) | 3;
        }
        vmm_flush();
        return P2V(pa);
    }
    for (uint64_t g = pa >> 30; g <= (pa + len - 1) >> 30; g++) {
        if (g >= 512) return NULL;
        if (boot_pdpt_a[g] & PTE_P) continue;
        if (used == 4) return NULL;
        for (int i = 0; i < 512; i++) pd[used][i] = (g << 30) | ((uint64_t)i << 21) | 0x9b;
        boot_pdpt_a[g] = V2P(pd[used++]) | 3;
    }
    vmm_flush();
    return P2V(pa);
}

/* clock scan for swap-out. under the space lock: pages with the accessed bit lose it (second chance),
   the others get a slot, a ref of ours and go read-only+cow so a write meanwhile makes the owner a
   copy and leaves ours alone. commit then checks the pte is still the same */
int vmm_swap_scan(uint64_t pd, uint64_t* hand, int budget, swb_t* b, int* n) {
    uint64_t f = MMLOCK(pd);
    uint64_t va = *hand < USER_BASE ? USER_BASE : *hand;
    bool full = false;
    SD_DEFER++;
    while (va < USER_TOP && budget > 0 && *n < SWB_MAX && !full) {
        uint64_t* t = (uint64_t*)P2V(pd);
        int lvl;
        for (lvl = 3; lvl > 0; lvl--) {
            uint64_t e = t[IDX(va, lvl)];
            if (!(e & PTE_P)) {
                uint64_t sz = 1ul << (12 + 9 * lvl);
                va = (va & ~(sz - 1)) + sz;
                break;
            }
            t = (uint64_t*)P2V(e & PTE_ADDR);
        }
        if (lvl) continue;
        for (int i = IDX(va, 0); i < 512 && budget > 0 && *n < SWB_MAX; i++, va += PAGE_SIZE) {
            uint64_t e = t[i];
            if ((e & (PTE_P | PTE_US | PTE_SHARED)) != (PTE_P | PTE_US) || !(e & (PTE_RW | PTE_COW))) continue;
            uint64_t fr = e & PTE_ADDR;
            if (!ours(fr) || refcnt[fr >> 12] != 1) continue;
            budget--;
            if (e & PTE_A) { t[i] = e & ~PTE_A; tlb_inval(va); continue; }
            int s = swap_alloc();
            if (s < 0) { full = true; break; }
            pmm_ref(fr);
            pte_put(&t[i], va, (e & ~PTE_RW) | PTE_COW);
            b[*n] = (swb_t){ pd, va, fr, PTE_RW | PTE_US, s };
            (*n)++;
        }
    }
    sd_end();
    spin_unlock(MML(pd), f);
    *hand = va;
    return va >= USER_TOP;
}

int vmm_swap_commit(swb_t* b, int n) {
    int freed = 0;
    for (int i = 0; i < n; ) {
        uint64_t pd = b[i].pd;
        int j = i;
        uint64_t f = MMLOCK(pd);
        SD_DEFER++;
        for (; j < n && b[j].pd == pd; j++) {
            uint64_t* p = pte_slot(pd, b[j].va, false);
            uint64_t e = p ? *p : 0;
            if (p && (e & PTE_P) && (e & PTE_ADDR) == b[j].fr && (e & PTE_COW)) {
                pte_put(p, b[j].va, PTE_SWAP | b[j].fl | ((uint64_t)b[j].slot << 12));
                b[j].slot = -1;           // taken
            }
        }
        sd_end();
        spin_unlock(MML(pd), f);
        for (int k = i; k < j; k++) {
            if (b[k].slot < 0) { pmm_unref(b[k].fr); freed++; }    // the pte's ref, ours goes below
            else swap_put(b[k].slot);
            pmm_unref(b[k].fr);
        }
        i = j;
    }
    return freed;
}

/* first page of the space at or above `from` that sits in swap area `area` (-1 = any), 0 if none */
uint64_t vmm_swap_find(uint64_t pd, uint64_t from, int area) {
    uint64_t va = from < USER_BASE ? USER_BASE : from;
    while (va < USER_TOP) {
        uint64_t* t = (uint64_t*)P2V(pd);
        int lvl;
        for (lvl = 3; lvl > 0; lvl--) {
            uint64_t e = t[IDX(va, lvl)];
            if (!(e & PTE_P)) {
                uint64_t sz = 1ul << (12 + 9 * lvl);
                va = (va & ~(sz - 1)) + sz;
                break;
            }
            t = (uint64_t*)P2V(e & PTE_ADDR);
        }
        if (lvl) continue;
        for (int i = IDX(va, 0); i < 512; i++, va += PAGE_SIZE) {
            uint64_t e = t[i];
            if ((e & (PTE_P | PTE_SWAP)) == PTE_SWAP && (area < 0 || (int)(SLOT(e) >> 28) == area)) return va;
        }
    }
    return 0;
}
