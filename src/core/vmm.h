#ifndef SAMARA_VMM_H
#define SAMARA_VMM_H
#include "core/types.h"

/* Physical frames for user processes + per-process page directories.

   The kernel keeps its 4 MiB PSE identity map for everything. A process
   directory copies those supervisor PDEs and replaces the user window
   [USER_BASE, USER_TOP) with 4 KiB pages backed by frames from a pool that
   sits between the end of the kernel heap and USER_BASE. Because the pool
   lies below USER_BASE, the kernel can always reach any frame through the
   identity map, no matter which directory is loaded. */

#define PAGE_SIZE        4096u
#define USER_BASE        0x08000000u
#define USER_TOP         0x40000000u
#define USER_MMAP_BASE   0x20000000u
#define USER_STACK_TOP   0x40000000u
#define USER_STACK_MAX   (8u * 1024u * 1024u)   /* grows on demand */

#define PTE_P  0x001u
#define PTE_RW 0x002u
#define PTE_US 0x004u

void     pmm_init(uint32_t pool_start, uint32_t pool_end);
uint32_t pmm_alloc(void);             /* zeroed frame, 0 when exhausted */
void     pmm_ref(uint32_t frame);
void     pmm_unref(uint32_t frame);
uint32_t pmm_free_frames(void);
uint32_t pmm_total_frames(void);

uint32_t vmm_new_space(void);                          /* 0 on OOM */
void     vmm_destroy_space(uint32_t pd);
uint32_t vmm_clone_space(uint32_t pd);                 /* fork: copy RW, share RO */

/* Map fresh zeroed pages over [va, va+len). Already-mapped pages are kept
   (and made writable if `writable`). Returns 0 or -1 on OOM. */
int      vmm_alloc_range(uint32_t pd, uint32_t va, uint32_t len, bool writable);
void     vmm_free_range(uint32_t pd, uint32_t va, uint32_t len);
bool     vmm_range_unmapped(uint32_t pd, uint32_t va, uint32_t len);
uint32_t vmm_find_free(uint32_t pd, uint32_t from, uint32_t limit, uint32_t len);
uint32_t vmm_pte(uint32_t pd, uint32_t va);            /* 0 = not mapped */
void     vmm_set_writable(uint32_t pd, uint32_t va, uint32_t len, bool writable);

/* Copy into / zero another space through the identity map (no CR3 switch). */
int      vmm_copy_to(uint32_t pd, uint32_t va, const void* src, uint32_t len);

uint32_t vmm_count_pages(uint32_t pd);                  /* mapped user pages */

/* After editing the live directory. */
void     vmm_flush(void);

#endif
