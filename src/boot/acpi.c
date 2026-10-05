#include "boot/acpi.h"
#include "core/vmm.h"
#include "core/string.h"
#include "drivers/vga.h"

struct acpi_info acpi;

struct sdt {
    char sig[4];
    uint32_t len;
    uint8_t rev, sum;
    char oem[6], oemt[8];
    uint32_t oemrev, cid, crev;
} __attribute__((packed));

static uint8_t sum(const void* p, int n) {
    uint8_t s = 0;
    for (int i = 0; i < n; i++) s += ((const uint8_t*)p)[i];
    return s;
}

static struct sdt* map_sdt(uint64_t pa) {
    if (pa >= 0x100000000ull) return NULL;      // TODO tables above 4G
    return (struct sdt*)P2V(pa);
}

static uint8_t* find_rsdp(void) {
    extern uint8_t mb2_rsdp[]; extern int mb2_have_rsdp;
    if (mb2_have_rsdp) return mb2_rsdp;
    uint8_t* r[2];
    int n = 0;
    uint32_t ebda = *(uint16_t*)P2V(0x40E) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) r[n++] = (uint8_t*)P2V(ebda);
    r[n++] = (uint8_t*)P2V(0xE0000);
    for (int k = 0; k < n; k++) {
        int len = k == 0 && n == 2 ? 1024 : 0x20000;
        for (int i = 0; i < len; i += 16)
            if (!memcmp(r[k] + i, "RSD PTR ", 8) && !sum(r[k] + i, 20)) return r[k] + i;
    }
    return NULL;
}

static void parse_madt(struct sdt* t) {
    uint8_t* p = (uint8_t*)t + 44, *e = (uint8_t*)t + t->len;
    acpi.lapic_pa = *(uint32_t*)((uint8_t*)t + 36);
    acpi.nmi_lint[0] = acpi.nmi_lint[1] = -1;
    while (p + 2 <= e && p[1]) {
        switch (p[0]) {
        case 0:
            if ((p[4] & 3) && acpi.ncpu < MAX_CPUS) acpi.cpu_apic[acpi.ncpu++] = p[3];
            break;
        case 1:
            if (acpi.nioapic < MAX_IOAPICS) {
                int i = acpi.nioapic++;
                acpi.ioapic[i].id = p[2];
                acpi.ioapic[i].pa = *(uint32_t*)(p + 4);
                acpi.ioapic[i].gsi_base = *(uint32_t*)(p + 8);
            }
            break;
        case 2:
            if (acpi.niso < MAX_ISO) {
                int i = acpi.niso++;
                acpi.iso[i].irq = p[3];
                acpi.iso[i].gsi = *(uint32_t*)(p + 4);
                acpi.iso[i].flags = *(uint16_t*)(p + 8);
            }
            break;
        case 4:                                 /* nmi: proc 0xff = all cpus */
            if (p[2] == 0xFF && p[5] < 2) acpi.nmi_lint[p[5]] = p[5];
            break;
        case 5:
            acpi.lapic_pa = *(uint64_t*)(p + 4);
            break;
        }
        p += p[1];
    }
}

void acpi_init(void) {
    uint8_t* rsdp = find_rsdp();
    if (!rsdp) return;
    int rev = rsdp[15];
    uint64_t root = rev >= 2 ? *(uint64_t*)(rsdp + 24) : *(uint32_t*)(rsdp + 16);
    struct sdt* r = map_sdt(root);
    if (!r) return;
    int w = rev >= 2 ? 8 : 4;
    int n = (r->len - sizeof(*r)) / w;
    for (int i = 0; i < n; i++) {
        uint8_t* ent = (uint8_t*)r + sizeof(*r) + i * w;
        uint64_t pa = w == 8 ? *(uint64_t*)ent : *(uint32_t*)ent;
        struct sdt* t = map_sdt(pa);
        if (t && !memcmp(t->sig, "APIC", 4)) { parse_madt(t); acpi.ok = acpi.ncpu > 0; }
    }
}

uint32_t acpi_irq_gsi(int irq, uint16_t* flags) {
    for (int i = 0; i < acpi.niso; i++)
        if (acpi.iso[i].irq == irq) { if (flags) *flags = acpi.iso[i].flags; return acpi.iso[i].gsi; }
    if (flags) *flags = 0;
    return irq;
}
