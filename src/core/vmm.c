#include "core/vmm.h"
#include "core/string.h"
#include "core/task.h"
#include "core/heap.h"
#include "boot/paging.h"

/* ---------------- physical frame pool ---------------- */

/* refcnt per frame, indexed by pfn. 0xFF = not ours (kernel, holes, mmio) */
static uint8_t* refcnt;
static uint64_t max_pfn, pool_frames, pool_free, hint;

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
    uint64_t f = irq_save();
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
    irq_restore(f);
    if (frame) memset(P2V(frame), 0, PAGE_SIZE);
    return frame;
}

/* n frames in a row (io_uring rings want one flat kernel view of them) */
uint64_t pmm_alloc_run(uint64_t n) {
    uint64_t f = irq_save();
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
    irq_restore(f);
    if (base) memset(P2V(base), 0, n * PAGE_SIZE);
    return base;
}

static bool ours(uint64_t frame) {
    uint64_t i = frame >> 12;
    return i < max_pfn && refcnt[i] != 0xFF;
}

void pmm_ref(uint64_t frame) {
    if (ours(frame) && refcnt[frame >> 12] < 254) refcnt[frame >> 12]++;
}

void pmm_unref(uint64_t frame) {
    if (!ours(frame) || !refcnt[frame >> 12]) return;
    uint64_t f = irq_save();
    if (--refcnt[frame >> 12] == 0) pool_free++;
    irq_restore(f);
}

/* ---------------- page tables ---------------- */

#define IDX(va, lvl) (((va) >> (12 + 9 * (lvl))) & 0x1FF)

void vmm_flush(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
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

int vmm_alloc_range(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) return -1;
        uint64_t* p = pte_slot(pd, a, true);
        if (!p) return -1;
        if (*p & PTE_P) {
            if (writable) *p |= PTE_RW;
            continue;
        }
        uint64_t fr = pmm_alloc();
        if (!fr) return -1;
        *p = fr | PTE_P | PTE_US | (writable ? PTE_RW : 0);
    }
    return 0;
}

void vmm_set_writable(uint64_t pd, uint64_t va, uint64_t len, bool writable) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & (PTE_P | PTE_LAZY))) continue;
        if (writable) *p |= PTE_RW; else *p &= ~PTE_RW;
    }
}

int vmm_map_frame(uint64_t pd, uint64_t va, uint64_t fr, bool rw) {
    uint64_t* p = pte_slot(pd, va, true);
    if (!p) return -1;
    pmm_ref(fr);
    if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
    *p = fr | PTE_P | PTE_US | (rw ? PTE_RW : 0) | PTE_SHARED;   /* every caller maps something shared */
    return 0;
}

/* java reserves hundreds of MB (heap, metaspace, thread stacks, code cache)
   and touches a fraction. every byte of it used to be a real frame up
   front: minecraft ran the box out of memory while making the world */
int vmm_lazy_range(uint64_t pd, uint64_t va, uint64_t len, bool rw, bool user) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) return -1;
        uint64_t* p = pte_slot(pd, a, true);
        if (!p) return -1;
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        *p = PTE_LAZY | (rw ? PTE_RW : 0) | (user ? PTE_US : 0);
    }
    return 0;
}

bool vmm_fault_in(uint64_t pd, uint64_t va) {
    uint64_t* p = pte_slot(pd, va & ~(PAGE_SIZE - 1), false);
    if (!p || (*p & PTE_P) || !(*p & PTE_LAZY)) return false;
    uint64_t fr = pmm_alloc();                     /* zeroed */
    if (!fr) return false;
    *p = fr | PTE_P | (*p & (PTE_RW | PTE_US));
    return true;
}

// PROT_NONE = present but supervisor only, so user access faults
void vmm_set_user(uint64_t pd, uint64_t va, uint64_t len, bool user) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & (PTE_P | PTE_LAZY))) continue;
        if (user) *p |= PTE_US; else *p &= ~PTE_US;
    }
}

void vmm_free_range(uint64_t pd, uint64_t va, uint64_t len) {
    uint64_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (!uaddr(a)) continue;
        uint64_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & (PTE_P | PTE_LAZY))) continue;
        if (*p & PTE_P) pmm_unref(*p & PTE_ADDR);
        *p = 0;
    }
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
        /* device pages (the lfb mapped by xorg) are shared, not copied */
        if ((e & PTE_RW) && ours(fr) && !(e & PTE_SHARED)) {   /* shm/memfd: shared, not copied */
            uint64_t nf = pmm_alloc();
            if (!nf) return false;
            memcpy(P2V(nf), P2V(fr), PAGE_SIZE);
            d[i] = nf | (e & ~PTE_ADDR);
        } else {
            pmm_ref(fr);                 /* read-only text: share */
            d[i] = e;
        }
    }
    return true;
}

uint64_t vmm_clone_space(uint64_t pd) {
    uint64_t npd = vmm_new_space();
    if (!npd) return 0;
    uint64_t* s = (uint64_t*)P2V(pd);
    uint64_t* d = (uint64_t*)P2V(npd);
    for (int i = 0; i < 256; i++) {
        if (!(s[i] & PTE_P)) continue;
        uint64_t n = pmm_alloc();
        if (!n) goto fail;
        d[i] = n | (s[i] & 0xFFF);
        if (!clone_level(s[i] & PTE_ADDR, n, 2, (uint64_t)i << 39)) goto fail;
    }
    return npd;
fail:
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
