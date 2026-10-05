#ifndef SAMARA_VMM_H
#define SAMARA_VMM_H
#include "core/types.h"

/* Physical frames for user processes + per-process 4-level page tables.

   Kernel half (PML4 256..511) is shared by every address space: the direct
   map of all RAM at DMAP_BASE (plus the first 4 GiB for mmio) and the kernel
   image at KERNEL_BASE. The user half is made of 4 KiB pages. */

#define PAGE_SIZE        4096ul
#define USER_BASE        0x10000ul
#define USER_TOP         0x800000000000ul
#define USER_MMAP_BASE   0x7f0000000000ul
#define USER_STACK_TOP   0x7ffffffff000ul
#define USER_STACK_MAX   (8ul * 1024 * 1024)    /* grows on demand */

#define DMAP_BASE        0xffff800000000000ul
#define KERNEL_BASE      0xffffffff80000000ul
#define P2V(p)           ((void*)((uint64_t)(p) + DMAP_BASE))
/* Physical address of a kernel buffer, for DMA: direct map or kernel image
   (heap included). User addresses have no single physical address, bounce those. */
static inline uint64_t V2P(const void* v) {
    uint64_t a = (uint64_t)v;
    return a >= KERNEL_BASE ? a - KERNEL_BASE : a - DMAP_BASE;
}
static inline bool dma_ok(const void* v) {
    uint64_t a = (uint64_t)v;
    return a >= KERNEL_BASE || (a >= DMAP_BASE && a - DMAP_BASE < 0x100000000ul);
}

#define PTE_P  0x001ul
#define PTE_RW 0x002ul
#define PTE_US 0x004ul
#define PTE_SHARED 0x200ul         /* avl bit: MAP_SHARED / shmat page, fork shares it */
#define PTE_LAZY   0x400ul         /* avl bit, P=0: anon mmap page that gets a frame on first touch */
#define PTE_SWAP   0x100ul         /* P=0: page is in swap, slot in the address bits, RW/US kept */
#define PTE_COW    0x800ul         /* avl bit, P=1, RW=0: writable page whose frame is shared after fork, copied on the write fault */
#define PTE_A  0x020ul
#define PTE_USED   (PTE_P | PTE_LAZY | PTE_SWAP)
#define PTE_WR(e)  ((e) & (PTE_RW | PTE_COW))
#define PTE_ADDR   0x000FFFFFFFFFF000ul

/* frames: pmm_init(top of ram), then pmm_add() for each usable range */
void     pmm_init(uint64_t top);
void     pmm_add(uint64_t start, uint64_t end);
uint64_t pmm_alloc(void);             /* zeroed frame, 0 when exhausted */
uint64_t pmm_alloc_run(uint64_t n);   /* contiguous, zeroed, 0 when there is no such run */
void     pmm_ref(uint64_t frame);
void     pmm_unref(uint64_t frame);
int      pmm_refcnt(uint64_t frame);
uint64_t pmm_free_frames(void);
uint64_t pmm_total_frames(void);

uint64_t vmm_new_space(void);                          /* 0 on OOM */
void     vmm_destroy_space(uint64_t pd);
uint64_t vmm_clone_space(uint64_t pd);                 /* fork: copy RW, share RO */

/* Map fresh zeroed pages over [va, va+len). Already-mapped pages are kept
   (and made writable if `writable`). Returns 0 or -1 on OOM. */
int      vmm_alloc_range(uint64_t pd, uint64_t va, uint64_t len, bool writable);
void     vmm_free_range(uint64_t pd, uint64_t va, uint64_t len);
bool     vmm_range_unmapped(uint64_t pd, uint64_t va, uint64_t len);
uint64_t vmm_map_anon(uint64_t pd, uint64_t addr, bool fixed, uint64_t lo, uint64_t hi, uint64_t len, bool rw, bool user);
uint64_t vmm_find_free(uint64_t pd, uint64_t from, uint64_t limit, uint64_t len);
uint64_t vmm_reserve(uint64_t pd, uint64_t addr, uint64_t lo, uint64_t hi, uint64_t len);
uint64_t vmm_pte(uint64_t pd, uint64_t va);            /* 0 = not mapped */
void     vmm_set_writable(uint64_t pd, uint64_t va, uint64_t len, bool writable);
void     vmm_set_user(uint64_t pd, uint64_t va, uint64_t len, bool user);
int      vmm_map_frame(uint64_t pd, uint64_t va, uint64_t fr, bool rw);    /* takes a ref on fr */
/* page cache frame into a user page, takes a ref. w: private writable (cow) or, with `shared`, plain rw */
int      vmm_map_cache(uint64_t pd, uint64_t va, uint64_t fr, bool w, bool shared);
/* anon memory without frames yet: they come on first touch (vmm_fault_in) */
int      vmm_lazy_range(uint64_t pd, uint64_t va, uint64_t len, bool rw, bool user);
bool     vmm_fault_in(uint64_t pd, uint64_t va);   /* a lazy page gets its frame, a swapped one comes back; false = not lazy / oom */
bool     vmm_swap_in(uint64_t pd, uint64_t va);    /* may sleep on the disk, no locks held */

/* reclaim, see mem.c. one swap-out batch: pages picked, write protected, not yet written */
typedef struct { uint64_t pd, va, fr, fl; int slot; } swb_t;
#define SWB_MAX 16
int      vmm_swap_scan(uint64_t pd, uint64_t* hand, int budget, swb_t* b, int* n);   /* 1 = reached the end of the space */
uint64_t vmm_swap_find(uint64_t pd, uint64_t from, int area);
int      vmm_swap_commit(swb_t* b, int n);                                    /* after the write, returns pages freed */


/* Copy into / zero another space through the direct map (no CR3 switch). */
int      vmm_copy_to(uint64_t pd, uint64_t va, const void* src, uint64_t len);

uint64_t vmm_count_pages(uint64_t pd);                  /* mapped user pages */

/* device memory (pci bars), anywhere in the physical space */
void*    mmio_map(uint64_t pa, size_t len);

/* write fault on a PTE_COW page: own copy (or just RW when we are the last user). false = not a cow page / oom */
bool     vmm_cow(uint64_t pd, uint64_t va);

/* After editing the live tables. Single pages go through tlb_inval in vmm.c (SMP shootdown goes there) */
void     vmm_flush(void);

#endif
