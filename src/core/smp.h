#ifndef SAMARA_SMP_H
#define SAMARA_SMP_H
#include "core/types.h"
#include "boot/gdt.h"

/* big kernel lock: whoever runs in ring 0 and is not parked in hlt holds it.
   c->bkl says if this cpu has it. irqs must be off around take/drop */
void bkl_take(struct cpu* c);
void bkl_drop(struct cpu* c);

void smp_init(void);                    /* wake the other cpus */
void tlb_service(void);                 /* no lock needed, called from ipi and from lock spins */
void tlb_shootdown(void);               /* other cpus on our address space flush, waits */
void kick_idle(void);                   /* poke a parked cpu so it looks for work */

#endif
