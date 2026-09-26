#include "core/vmm.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/paging.h"

/* ---------------- physical frame pool ---------------- */

static uint32_t pool_base, pool_frames, pool_free, hint;
static uint8_t  refcnt[(DMAP_SIZE >> 12)];     /* frames live in the direct map */

void pmm_init(uint32_t start, uint32_t end) {
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (end > DMAP_SIZE) end = DMAP_SIZE;
    if (end <= start) { pool_frames = 0; return; }
    pool_base = start;
    pool_frames = (end - start) >> 12;
    pool_free = pool_frames;
    hint = 0;
    memset(refcnt, 0, sizeof(refcnt));
}

uint32_t pmm_free_frames(void)  { return pool_free; }
uint32_t pmm_total_frames(void) { return pool_frames; }

uint32_t pmm_alloc(void) {
    uint32_t f = irq_save();
    uint32_t frame = 0;
    for (uint32_t n = 0; n < pool_frames; n++) {
        uint32_t i = (hint + n) % pool_frames;
        if (refcnt[i]) continue;
        refcnt[i] = 1;
        pool_free--;
        hint = i + 1;
        frame = pool_base + (i << 12);
        break;
    }
    irq_restore(f);
    if (frame) memset(P2V(frame), 0, PAGE_SIZE);
    return frame;
}

static int frame_idx(uint32_t frame) {
    if (frame < pool_base) return -1;
    uint32_t i = (frame - pool_base) >> 12;
    return i < pool_frames ? (int)i : -1;
}

void pmm_ref(uint32_t frame) {
    int i = frame_idx(frame);
    if (i >= 0 && refcnt[i] < 255) refcnt[i]++;
}

void pmm_unref(uint32_t frame) {
    int i = frame_idx(frame);
    if (i < 0 || !refcnt[i]) return;
    uint32_t f = irq_save();
    if (--refcnt[i] == 0) pool_free++;
    irq_restore(f);
}

/* ---------------- page directories ---------------- */

#define PDI(va) ((va) >> 22)
#define PTI(va) (((va) >> 12) & 0x3FF)

void vmm_flush(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

uint32_t vmm_new_space(void) {
    uint32_t pd = pmm_alloc();
    if (!pd) return 0;
    const uint32_t* kpd = (const uint32_t*)P2V(task_kernel_cr3());
    uint32_t* d = (uint32_t*)P2V(pd);
    for (int i = 0; i < 1024; i++)
        d[i] = (i >= (int)PDI(USER_BASE) && i < (int)PDI(USER_TOP)) ? 0 : kpd[i];
    return pd;
}

static uint32_t* pte_slot(uint32_t pd, uint32_t va, bool create) {
    uint32_t* d = (uint32_t*)P2V(pd);
    uint32_t pde = d[PDI(va)];
    if (!(pde & PTE_P)) {
        if (!create) return NULL;
        uint32_t pt = pmm_alloc();
        if (!pt) return NULL;
        d[PDI(va)] = pt | PTE_P | PTE_RW | PTE_US;
        pde = d[PDI(va)];
    }
    return (uint32_t*)P2V(pde & ~0xFFFu) + PTI(va);
}

uint32_t vmm_pte(uint32_t pd, uint32_t va) {
    if (va < USER_BASE || va >= USER_TOP) return 0;
    uint32_t* p = pte_slot(pd, va, false);
    return p ? *p : 0;
}

int vmm_alloc_range(uint32_t pd, uint32_t va, uint32_t len, bool writable) {
    uint32_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (a < USER_BASE || a >= USER_TOP) return -1;
        uint32_t* p = pte_slot(pd, a, true);
        if (!p) return -1;
        if (*p & PTE_P) {
            if (writable) *p |= PTE_RW;
            continue;
        }
        uint32_t fr = pmm_alloc();
        if (!fr) return -1;
        *p = fr | PTE_P | PTE_US | (writable ? PTE_RW : 0);
    }
    return 0;
}

void vmm_set_writable(uint32_t pd, uint32_t va, uint32_t len, bool writable) {
    uint32_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint32_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & PTE_P)) continue;
        if (writable) *p |= PTE_RW; else *p &= ~PTE_RW;
    }
}

void vmm_free_range(uint32_t pd, uint32_t va, uint32_t len) {
    uint32_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        if (a < USER_BASE || a >= USER_TOP) continue;
        uint32_t* p = pte_slot(pd, a, false);
        if (!p || !(*p & PTE_P)) continue;
        pmm_unref(*p & ~0xFFFu);
        *p = 0;
    }
}

bool vmm_range_unmapped(uint32_t pd, uint32_t va, uint32_t len) {
    uint32_t end = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t a = va & ~(PAGE_SIZE - 1); a < end; a += PAGE_SIZE) {
        uint32_t* p = pte_slot(pd, a, false);
        if (p && (*p & PTE_P)) return false;
    }
    return true;
}

uint32_t vmm_find_free(uint32_t pd, uint32_t from, uint32_t limit, uint32_t len) {
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t run = 0, start = from;
    for (uint32_t a = from; a < limit; a += PAGE_SIZE) {
        uint32_t* d = (uint32_t*)P2V(pd);
        if (!(d[PDI(a)] & PTE_P) && (a & 0x3FFFFF) == 0 && run == 0) {
            /* Whole empty 4 MiB table: take it in one step. */
            start = a;
            run = 0x400000;
            if (run >= len) return start;
            a += 0x400000 - PAGE_SIZE;
            continue;
        }
        uint32_t* p = pte_slot(pd, a, false);
        if (p && (*p & PTE_P)) { run = 0; continue; }
        if (run == 0) start = a;
        run += PAGE_SIZE;
        if (run >= len) return start;
    }
    return 0;
}

void vmm_destroy_space(uint32_t pd) {
    uint32_t* d = (uint32_t*)P2V(pd);
    for (uint32_t i = PDI(USER_BASE); i < PDI(USER_TOP); i++) {
        if (!(d[i] & PTE_P)) continue;
        uint32_t ptf = d[i] & ~0xFFFu;
        uint32_t* pt = (uint32_t*)P2V(ptf);
        for (int j = 0; j < 1024; j++)
            if (pt[j] & PTE_P) pmm_unref(pt[j] & ~0xFFFu);
        pmm_unref(ptf);
        d[i] = 0;
    }
    pmm_unref(pd);
}

uint32_t vmm_clone_space(uint32_t pd) {
    uint32_t npd = vmm_new_space();
    if (!npd) return 0;
    uint32_t* d = (uint32_t*)P2V(pd);
    for (uint32_t i = PDI(USER_BASE); i < PDI(USER_TOP); i++) {
        if (!(d[i] & PTE_P)) continue;
        uint32_t* pt = (uint32_t*)P2V(d[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PTE_P)) continue;
            uint32_t va = (i << 22) | ((uint32_t)j << 12);
            uint32_t* np = pte_slot(npd, va, true);
            if (!np) { vmm_destroy_space(npd); return 0; }
            uint32_t fr = pt[j] & ~0xFFFu;
            if (pt[j] & PTE_RW) {
                uint32_t nf = pmm_alloc();
                if (!nf) { vmm_destroy_space(npd); return 0; }
                memcpy(P2V(nf), P2V(fr), PAGE_SIZE);
                *np = nf | (pt[j] & 0xFFFu);
            } else {
                pmm_ref(fr);                 /* read-only text: share */
                *np = pt[j];
            }
        }
    }
    return npd;
}

int vmm_copy_to(uint32_t pd, uint32_t va, const void* src, uint32_t len) {
    const uint8_t* s = (const uint8_t*)src;
    while (len) {
        uint32_t pte = vmm_pte(pd, va);
        if (!(pte & PTE_P)) return -1;
        uint32_t off = va & 0xFFF;
        uint32_t n = PAGE_SIZE - off;
        if (n > len) n = len;
        uint8_t* dst = (uint8_t*)P2V(pte & ~0xFFFu) + off;
        if (s) { memcpy(dst, s, n); s += n; }
        else   memset(dst, 0, n);
        va += n; len -= n;
    }
    return 0;
}

uint32_t vmm_count_pages(uint32_t pd) {
    uint32_t* d = (uint32_t*)P2V(pd);
    uint32_t n = 0;
    for (uint32_t i = PDI(USER_BASE); i < PDI(USER_TOP); i++) {
        if (!(d[i] & PTE_P)) continue;
        uint32_t* pt = (uint32_t*)P2V(d[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) if (pt[j] & PTE_P) n++;
    }
    return n;
}
