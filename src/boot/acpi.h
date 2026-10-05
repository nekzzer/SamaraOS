#ifndef SAMARA_ACPI_H
#define SAMARA_ACPI_H
#include "core/types.h"

#define MAX_IOAPICS 4
#define MAX_ISO     16

struct acpi_info {
    int      ok;
    uint64_t lapic_pa;
    int      ncpu;
    uint8_t  cpu_apic[MAX_CPUS];        /* lapic ids, bsp is NOT first necessarily */
    int      nioapic;
    struct { uint8_t id; uint32_t pa, gsi_base; } ioapic[MAX_IOAPICS];
    int      niso;
    struct { uint8_t irq; uint32_t gsi; uint16_t flags; } iso[MAX_ISO];
    int      nmi_lint[2];               /* lint pin for nmi, -1 none */
};
extern struct acpi_info acpi;

void acpi_init(void);
/* gsi an isa irq ends up on, and the mps flags of the override (0 = default) */
uint32_t acpi_irq_gsi(int irq, uint16_t* flags);

#endif
