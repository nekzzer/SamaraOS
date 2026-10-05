#include "boot/apic.h"
#include "boot/acpi.h"
#include "boot/gdt.h"
#include "boot/pic.h"
#include "core/io.h"
#include "core/vmm.h"
#include "drivers/vga.h"

int apic_on;
uint32_t tsc_khz;
static uint64_t tsc0;
static uint32_t lapic_khz;              /* lapic timer ticks per ms, divide 16 */
static volatile uint32_t* lapic;
static volatile uint32_t* ioa[MAX_IOAPICS];
static int ioa_n[MAX_IOAPICS];
static int bsp_apic;
extern uint64_t boot_pd[];

static inline uint64_t rdtsc(void) {
    uint32_t a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}
static uint64_t rdmsr(uint32_t m) {
    uint32_t a, d;
    __asm__ volatile ("rdmsr" : "=a"(a), "=d"(d) : "c"(m));
    return ((uint64_t)d << 32) | a;
}
static void wrmsr(uint32_t m, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint32_t lrd(int r) { return lapic[r >> 2]; }
static void lwr(int r, uint32_t v) { lapic[r >> 2] = v; }

void lapic_eoi(void) { lwr(0xB0, 0); }
int  lapic_id(void) { return lrd(0x20) >> 24; }

void lapic_ipi_raw(int apic_id, uint32_t icr) {
    while (lrd(0x300) & 0x1000) __asm__ volatile ("pause");
    lwr(0x310, (uint32_t)apic_id << 24);
    lwr(0x300, icr);
}
void lapic_ipi(int apic_id, int vec) { lapic_ipi_raw(apic_id, vec | 0x4000); }

static uint32_t ird(int i, int reg) { ioa[i][0] = reg; return ioa[i][4]; }
static void iwr(int i, int reg, uint32_t v) { ioa[i][0] = reg; ioa[i][4] = v; }

void ioapic_irq(int irq, bool mask) {
    if (!apic_on || irq == 2) return;        // irq2 is the pit's gsi here, cascade means nothing
    uint16_t fl;
    uint32_t gsi = acpi_irq_gsi(irq, &fl);
    for (int i = 0; i < acpi.nioapic; i++) {
        if (gsi < acpi.ioapic[i].gsi_base || gsi >= acpi.ioapic[i].gsi_base + ioa_n[i]) continue;
        int pin = gsi - acpi.ioapic[i].gsi_base;
        uint32_t lo = 0x20 + irq;
        if ((fl & 3) == 3) lo |= 1 << 13;
        if (((fl >> 2) & 3) == 3) lo |= 1 << 15;
        if (mask) lo |= 1 << 16;
        iwr(i, 0x11 + pin * 2, (uint32_t)bsp_apic << 24);
        iwr(i, 0x10 + pin * 2, lo);
        return;
    }
}

uint64_t tsc_us(void) {
    if (!tsc_khz) return 0;
    return (rdtsc() - tsc0) * 1000 / tsc_khz;
}

uint64_t tsc_ms(void) {
    if (!tsc_khz) return 0;
    return (rdtsc() - tsc0) / tsc_khz;
}

/* pit ch2 as a stopwatch, 30ms. lapic timer and tsc counted meanwhile */
static void calibrate(void) {
    uint32_t cnt = 35795;
    outb(0x61, (inb(0x61) & 0xFC) | 1);
    outb(0x43, 0xB0);
    outb(0x42, cnt & 0xFF);
    outb(0x42, cnt >> 8);
    lwr(0x3E0, 3);                            /* divide by 16 */
    lwr(0x320, 0x10000);
    lwr(0x380, 0xFFFFFFFF);
    uint64_t t0 = rdtsc();
    while (!(inb(0x61) & 0x20)) {}
    uint64_t t1 = rdtsc();
    uint32_t left = lrd(0x390);
    lwr(0x380, 0);
    uint64_t lt = 0xFFFFFFFFu - left;
    tsc_khz = (uint32_t)((t1 - t0) * 1193182ull / cnt / 1000);
    lapic_khz = (uint32_t)(lt * 1193182ull / cnt / 1000);
    tsc0 = t0;
}

void apic_init(void) {
    if (!acpi.ok) return;
    uint64_t base = rdmsr(0x1B);
    if (base & 0x400) return;                 // x2apic already on, not supported
    wrmsr(0x1B, (base & ~0xFFFull) | 0x800 | (acpi.lapic_pa & ~0xFFFull));
    // lapic/ioapic live in 2M pages of the direct map: make them uncached
    boot_pd[acpi.lapic_pa >> 21] |= 0x18;
    for (int i = 0; i < acpi.nioapic; i++) boot_pd[acpi.ioapic[i].pa >> 21] |= 0x18;
    vmm_flush();
    lapic = (volatile uint32_t*)P2V(acpi.lapic_pa);
    bsp_apic = lapic_id();
    for (int i = 0; i < acpi.nioapic; i++) {
        ioa[i] = (volatile uint32_t*)P2V(acpi.ioapic[i].pa);
        ioa_n[i] = ((ird(i, 1) >> 16) & 0xFF) + 1;
        for (int p = 0; p < ioa_n[i]; p++) iwr(i, 0x10 + p * 2, 1 << 16);
    }
    // legacy pic out of the way
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    lwr(0xF0, 0x100 | VEC_SPUR);
    apic_on = 1;
    calibrate();
    vga_printf("apic: %d cpu, %d ioapic, tsc %u kHz, lapic %u kHz\n", acpi.ncpu, acpi.nioapic, tsc_khz, lapic_khz * 16);
}

void apic_cpu_init(void) {
    wrmsr(0x1B, (rdmsr(0x1B) & ~0xFFFull) | 0x800 | (acpi.lapic_pa & ~0xFFFull));
    lwr(0xF0, 0x100 | VEC_SPUR);
    lwr(0x80, 0);
    lwr(0x350, 0x10000);                      /* lint0, lint1: masked or nmi */
    lwr(0x360, acpi.nmi_lint[1] >= 0 ? 0x400 : 0x10000);
    if (acpi.nmi_lint[0] >= 0) lwr(0x350, 0x400);
    lwr(0x370, 0xFE);
}

void apic_timer_start(void) {
    lwr(0x3E0, 3);
    lwr(0x320, VEC_TIMER | 0x20000);          /* periodic */
    lwr(0x380, lapic_khz);                    /* 1 kHz */
}
