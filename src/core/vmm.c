#include "core/smp.h"
#include "core/vmm.h"
#include "core/string.h"
#include "core/task.h"
#include "core/heap.h"
#include "boot/paging.h"

/* ---------------- physical frame pool ---------------- */

/* refcnt per frame, indexed by pfn. 0xFF = not ours (kernel, holes, mmio) */
static uint8_t* refcnt;
static uint64_t max_pfn, pool_frames, pool_free, hint;
static spin_t pl;
/* page tables of one address space, hashed by pd. never nested */
static spin_t mml[16];
#define MML(pd) (&mml[((pd) >> 12) & 15])

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

uint64_t vmm_pte(uint64_t pd, uint64_t va) {
    if (!uaddr(va)) return 0;
    uint64_t* p = pte_slot(pd, va, false);
    return p ? *p : 0;
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
        uint64_t fr = pmm_alloc();
        if (!fr) { ret = -1; break; }
        pte_put(p, a, fr | PTE_P | PTE_US | (writable ? PTE_RW : 0));
    }
    sd_end();
    return ret;
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
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & (PTE_P | PTE_LAZY))) continue;
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
    pte_put(p, va, fr | PTE_P | PTE_US | (rw ? PTE_RW : 0) | PTE_SHARED);   /* every caller maps something shared */
    return 0;
}

int vmm_map_frame(uint64_t pd, uint64_t va, uint64_t fr, bool rw) {
    uint64_t f = MMLOCK(pd);
    int r = vmm_map_frame_nl(pd, va, fr, rw);
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
        uint64_t* p = pte_slot(pd, a, true);
        if (!p) { ret = -1; break; }
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        pte_put(p, a, PTE_LAZY | (rw ? PTE_RW : 0) | (user ? PTE_US : 0));
    }
    sd_end();
    return ret;
}

int vmm_lazy_range(uint64_t pd, uint64_t va, uint64_t len, bool rw, bool user) {
    uint64_t f = MMLOCK(pd);
    int r = vmm_lazy_range_nl(pd, va, len, rw, user);
    spin_unlock(MML(pd), f);
    return r;
}

static bool vmm_fault_in_nl(uint64_t pd, uint64_t va) {
    uint64_t* p = pte_slot(pd, va & ~(PAGE_SIZE - 1), false);
    if (!p) return false;
    uint64_t old = *p;
    if (old & PTE_P) return true;                  /* other thread was faster */
    if (!(old & PTE_LAZY)) return false;
    uint64_t fr = pmm_alloc();                     /* zeroed */
    if (!fr) return false;
    /* two threads on one fresh page: the loser must not overwrite what the winner already wrote */
    if (!__sync_bool_compare_and_swap(p, old, fr | PTE_P | (old & (PTE_RW | PTE_US)))) { pmm_unref(fr); return true; }
    return true;
}

bool vmm_fault_in(uint64_t pd, uint64_t va) {
    uint64_t f = MMLOCK(pd);
    bool r = vmm_fault_in_nl(pd, va);
    spin_unlock(MML(pd), f);
    return r;
}

// PROT_NONE = present but supervisor only, so user access faults
static void vmm_set_user_nl(uint64_t pd, uint64_t va, uint64_t len, bool user) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & (PTE_P | PTE_LAZY))) continue;
        pte_put(p, a, user ? *p | PTE_US : *p & ~PTE_US);
    }
    sd_end();
}

void vmm_set_user(uint64_t pd, uint64_t va, uint64_t len, bool user) {
    uint64_t f = MMLOCK(pd);
    vmm_set_user_nl(pd, va, len, user);
    spin_unlock(MML(pd), f);
}

static void vmm_free_range_nl(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    SD_DEFER++;
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) continue;
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & (PTE_P | PTE_LAZY))) continue;
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        pte_put(p, a, 0);
    }
    sd_end();
}

void vmm_free_range(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t f = MMLOCK(pd);
    vmm_free_range_nl(pd, va, len);
    spin_unlock(MML(pd), f);
}

bool vmm_range_unmapped(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t* p = pte_slot(pd, a, false);
        if (p && (*p & (PTE_P | PTE_LAZY))) return false;
    }
    return true;
}

/* how much address space from va up is certainly empty because an upper
   level entry is missing, 0 if the walk reaches a pte table */
static uint64_t hole_at(uint64_t pd, uint64_t va) {
    uint64_t* t = (uint64_t*)P2V(pd);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t e = t[IDX(va, lvl)];
        if (!(e & PTE_P)) return 1ul << (12 + 9 * lvl);
        t = (uint64_t*)P2V(e & PTE_ADDR);
    }
    return 0;
}

uint64_t vmm_find_free(uint64_t pd, uint64_t from, uint64_t limit, uint64_t len) {
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t run = 0, start = from;
    for (uint64_t a = from; a < limit; a += PAGE_SIZE) {
        uint64_t h = hole_at(pd, a);
        if (h) {
            /* whole empty table: take it in one step */
            uint64_t nx = (a & ~(h - 1)) + h;
            if (run == 0) start = a;
            run += nx - a;
            if (run >= len) return start;
            a = nx - PAGE_SIZE;
            continue;
        }
        uint64_t* p = pte_slot(pd, a, false);
        if (p && (*p & (PTE_P | PTE_LAZY))) { run = 0; continue; }
        if (run == 0) start = a;
        run += PAGE_SIZE;
        if (run >= len) return start;
    }
    return 0;
}

/* anon mmap for the threads that mmap at once: find the hole and claim it under one lock */
uint64_t vmm_map_anon(uint64_t pd, uint64_t addr, bool fixed, uint64_t lo, uint64_t hi, uint64_t len, bool rw, bool user) {
    uint64_t f = MMLOCK(pd);
    if (fixed) vmm_free_range_nl(pd, addr, len);
    else if (!(addr && !(addr & 0xFFF) && addr >= lo && addr + len <= hi && vmm_range_unmapped(pd, addr, len)))
        addr = vmm_find_free(pd, lo, hi, len);
    if (addr && vmm_lazy_range_nl(pd, addr, len, rw, user) < 0) { vmm_free_range_nl(pd, addr, len); addr = 0; }
    spin_unlock(MML(pd), f);
    return addr;
}

/* same hole search as vmm_map_anon but leaves a PROT_NONE lazy claim, the real mapping overwrites it.
   find + mmap in two steps raced with the lock-free anon mmap of another thread (es2gears, malloc vs xkb files) */
uint64_t vmm_reserve(uint64_t pd, uint64_t addr, uint64_t lo, uint64_t hi, uint64_t len) {
    uint64_t f = MMLOCK(pd);
    if (!(addr && !(addr & 0xFFF) && addr >= lo && addr + len <= hi && vmm_range_unmapped(pd, addr, len)))
        addr = vmm_find_free(pd, lo, hi, len);
    if (addr && vmm_lazy_range_nl(pd, addr, len, false, false) < 0) { vmm_free_range_nl(pd, addr, len); addr = 0; }
    spin_unlock(MML(pd), f);
    return addr;
}

static void free_tables(uint64_t tbl, int lvl) {
    uint64_t* t = (uint64_t*)P2V(tbl);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) continue;
        if (lvl == 0) pmm_unref(t[i] & PTE_ADDR);
        else free_tables(t[i] & PTE_ADDR, lvl - 1);
    }
    pmm_unref(tbl);
}

void vmm_destroy_space(uint64_t pd) {
    if (ncpu > 1) tlb_unload(pd);
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
            if (!(e & PTE_P)) continue;
            uint64_t n = pmm_alloc();
            if (!n) return false;
            d[i] = n | (e & 0xFFF);
            if (!clone_level(e & PTE_ADDR, n, lvl - 1, base | ((uint64_t)i << (12 + 9 * lvl)))) return false;
            continue;
        }
        if (!(e & PTE_P)) {
            if (e & PTE_LAZY) d[i] = e;            /* untouched: stays lazy */
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

uint64_t vmm_count_pages(uint64_t pd) {
    uint64_t* d = (uint64_t*)P2V(pd);
    uint64_t n = 0;
    for (int i = 0; i < 256; i++)
        if (d[i] & PTE_P) n += count_level(d[i] & PTE_ADDR, 2);
    return n;
}

/* below 4G the direct map already covers it. above: hang a 1G slot of 2M
   uncached pages into the direct map (pools of 4, bars up there are rare) */
extern uint64_t boot_pdpt_a[];
void* mmio_map(uint64_t pa, size_t len) {
    static uint64_t pd[4][512] __attribute__((aligned(4096)));
    static int used;
    if (pa + len <= 0x100000000ull) return P2V(pa);
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
