#ifndef SAMARA_SMP_H
#define SAMARA_SMP_H
#include "core/types.h"
#include "boot/gdt.h"

/* big kernel lock: whoever runs in ring 0 and is not parked in hlt holds it.
   c->bkl says if this cpu has it. irqs must be off around take/drop */
void bkl_take(struct cpu* c);
void bkl_drop(struct cpu* c);
void bkl_yield(struct cpu* c);

/* small spinlocks for the parts that run without the bkl. irq off while held,
   so never touch user memory under one (a fault longjmps out with the lock held) */
typedef struct { volatile uint32_t v; int cpu; void* pc; } spin_t;
uint64_t spin_lock(spin_t* l);          /* returns the irq flags for spin_unlock */
void spin_unlock(spin_t* l, uint64_t f);

void smp_init(void);                    /* wake the other cpus */
void tlb_service(void);                 /* no lock needed, called from ipi and from lock spins */
void tlb_unload(uint64_t pd);           /* before the page tables of pd are freed */
void tlb_shootdown_pd(uint64_t pd);     /* same, only cpus running pd */
void tlb_shootdown(void);               /* other cpus on our address space flush, waits */
void kick_idle(void);                   /* poke a parked cpu so it looks for work */

#endif
