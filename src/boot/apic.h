#ifndef SAMARA_APIC_H
#define SAMARA_APIC_H
#include "core/types.h"

#define VEC_TIMER  0x30
#define VEC_TLB    0xF1
#define VEC_KICK   0xF2
#define VEC_SPUR   0xFF

extern int apic_on;
extern uint32_t tsc_khz;

void apic_init(void);                  /* bsp: lapic, ioapic, pic off */
void apic_cpu_init(void);              /* every cpu, after its gdt/idt */
void apic_timer_start(void);           /* this cpu, 1 kHz */
void lapic_eoi(void);
int  lapic_id(void);
void lapic_ipi(int apic_id, int vec);
void lapic_ipi_raw(int apic_id, uint32_t icr);
void ioapic_irq(int irq, bool mask);   /* isa irq -> its gsi, vector 0x20+irq, to the bsp */
uint64_t tsc_ms(void);                 /* time since boot, tsc based */

#endif
