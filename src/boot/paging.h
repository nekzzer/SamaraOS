#ifndef SAMARA_PAGING_H
#define SAMARA_PAGING_H
#include "core/types.h"

/* Boot tables (boot.S) already map the first 4 GiB; this drops the identity
   map, extends the direct map over RAM above 4 GiB and installs the fault
   handlers. Needs the IDT. */
void paging_init(uint64_t ram_top);

uint64_t paging_cr0(void);
uint64_t paging_cr3(void);
uint64_t paging_cr4(void);

#endif
