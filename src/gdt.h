#ifndef SAMARA_GDT_H
#define SAMARA_GDT_H
#include "types.h"

void gdt_init(void);

/* Update the ring0 stack pointer that the CPU will load on a ring3->ring0
   transition. Call when the active kernel stack changes (e.g. task switch). */
void tss_set_esp0(uint32_t esp0);

#endif
