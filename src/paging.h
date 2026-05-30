#ifndef SAMARA_PAGING_H
#define SAMARA_PAGING_H
#include "types.h"

/* Identity-maps the whole 4 GiB address space with 4 MiB PSE pages and
   turns paging on. Must be called after IDT is in place so #PF can fire. */
void paging_init(void);

/* Diagnostics. */
uint32_t paging_cr0(void);
uint32_t paging_cr3(void);
uint32_t paging_cr4(void);

#endif
