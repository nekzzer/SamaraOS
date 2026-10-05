#include "boot/acpi.h"
#include "core/vmm.h"
#include "core/string.h"
#include "drivers/vga.h"
#include "core/io.h"
#include "boot/idt.h"
#include "boot/pic.h"
#include "boot/apic.h"
#include "core/task.h"
#include "fs/ext2.h"
#include "fs/fatfs.h"

static void klog(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

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

static struct {
    uint16_t sci;
    uint32_t smi_cmd, pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt;
    uint8_t acpi_en, evt_len;
    uint8_t reset_sp, reset_val;
    uint64_t reset_addr;
    int s5a, s5b;                         /* -1 = no _S5_ found */
    volatile int pwr;
    task_t* wt;
} pm = { .s5a = -1, .s5b = -1 };

static void parse_fadt(struct sdt* t) {
    uint8_t* b = (uint8_t*)t;
    pm.sci = *(uint16_t*)(b + 46);
    pm.smi_cmd = *(uint32_t*)(b + 48);
    pm.acpi_en = b[52];
    pm.pm1a_evt = *(uint32_t*)(b + 56);
    pm.pm1b_evt = *(uint32_t*)(b + 60);
    pm.pm1a_cnt = *(uint32_t*)(b + 64);
    pm.pm1b_cnt = *(uint32_t*)(b + 68);
    pm.evt_len = b[88];
    if (t->len >= 129 && (*(uint32_t*)(b + 112) & (1 << 10)) && b[116] <= 1) {
        pm.reset_sp = b[116];
        pm.reset_addr = *(uint64_t*)(b + 120);
        pm.reset_val = b[128];
    }
    uint64_t dsdt = *(uint32_t*)(b + 40);
    if (t->len >= 148 && *(uint64_t*)(b + 140)) dsdt = *(uint64_t*)(b + 140);
    struct sdt* d = map_sdt(dsdt);
    if (d && !memcmp(d->sig, "DSDT", 4)) acpi.dsdt = d;
}

/* no aml interpreter, just look for Name(_S5_, Package(){a, b, ..}) bytes */
static void find_s5(struct sdt* t) {
    uint8_t* p = (uint8_t*)t + sizeof(*t), *e = (uint8_t*)t + t->len;
    for (; p + 12 < e; p++) {
        if (memcmp(p, "_S5_", 4) || p[4] != 0x12) continue;
        if (p > (uint8_t*)t + sizeof(*t) && p[-1] != 0x08 && !(p[-2] == 0x08 && p[-1] == '\\')) continue;
        uint8_t* q = p + 5;
        q += ((*q >> 6) & 3) + 1;                 /* pkglen */
        q++;                                      /* numelements */
        int v[2];
        for (int i = 0; i < 2; i++) {
            if (*q == 0x0A) { v[i] = q[1]; q += 2; }
            else if (*q == 0x0B) { v[i] = *(uint16_t*)(q + 1); q += 3; }
            else if (*q <= 1) { v[i] = *q; q++; }
            else return;
        }
        pm.s5a = v[0] & 7;
        pm.s5b = v[1] & 7;
        return;
    }
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
        if (!t) continue;
        if (!memcmp(t->sig, "APIC", 4)) { parse_madt(t); acpi.ok = acpi.ncpu > 0; }
        else if (!memcmp(t->sig, "FACP", 4)) parse_fadt(t);
        else if (!memcmp(t->sig, "HPET", 4)) acpi.hpet_pa = *(uint64_t*)((uint8_t*)t + 44);
    }
    if (acpi.dsdt) find_s5(acpi.dsdt);
    // ssdt may come before the fadt, walk again if the dsdt had nothing
    if (pm.s5a < 0) {
        for (int i = 0; i < n; i++) {
            uint8_t* ent = (uint8_t*)r + sizeof(*r) + i * w;
            struct sdt* t = map_sdt(w == 8 ? *(uint64_t*)ent : *(uint32_t*)ent);
            if (t && !memcmp(t->sig, "SSDT", 4)) find_s5(t);
        }
    }
    if (pm.pm1a_cnt) { vga_printf("acpi: s5 %d/%d pm1a %x sci %d\n", pm.s5a, pm.s5b, pm.pm1a_cnt, pm.sci); { char b[16]; klog("[acpi] s5 "); itoa(pm.s5a, b, 10); klog(b); klog(" sci "); itoa(pm.sci, b, 10); klog(b); klog("\r\n"); } }
}

uint32_t acpi_irq_gsi(int irq, uint16_t* flags) {
    for (int i = 0; i < acpi.niso; i++)
        if (acpi.iso[i].irq == irq) { if (flags) *flags = acpi.iso[i].flags; return acpi.iso[i].gsi; }
    if (flags) *flags = 0;
    return irq;
}

static void pwr_task(void) {
    for (;;) {
        uint32_t f = irq_save();
        if (!pm.pwr) { pm.wt->state = T_BLOCKED; while (pm.wt->state == T_BLOCKED) task_yield(); }
        int go = pm.pwr;
        irq_restore(f);
        if (go) {
            klog("[acpi] power button\r\n");
            acpi_poweroff();
        }
    }
}

static void sci_isr(regs_t* r) {
    (void)r;
    uint16_t st = inw(pm.pm1a_evt);
    if (pm.pm1b_evt) st |= inw(pm.pm1b_evt);
    if (st & 0x100) {                          /* PWRBTN_STS */
        outw(pm.pm1a_evt, 0x100);
        if (pm.pm1b_evt) outw(pm.pm1b_evt, 0x100);
        pm.pwr = 1;
        if (pm.wt && pm.wt->state == T_BLOCKED) task_ready(pm.wt);
    }
    outw(pm.pm1a_evt, st & ~0x100);            /* whatever else, we don't care */
    pic_send_eoi(pm.sci);
}

void acpi_pm_init(void) {
    if (!pm.pm1a_cnt) return;
    if (pm.smi_cmd && pm.acpi_en && !(inw(pm.pm1a_cnt) & 1)) {
        outb(pm.smi_cmd, pm.acpi_en);
        for (int i = 0; i < 300 && !(inw(pm.pm1a_cnt) & 1); i++) { io_wait(); }
    }
    if (!pm.pm1a_evt || !pm.evt_len || pm.sci > 15) return;
    uint16_t en = pm.pm1a_evt + pm.evt_len / 2;
    outw(pm.pm1a_evt, 0xFFFF);
    outw(en, 0x100);                           /* PWRBTN_EN only */
    if (pm.pm1b_evt) { outw(pm.pm1b_evt, 0xFFFF); outw(pm.pm1b_evt + pm.evt_len / 2, 0x100); }
    int id = task_spawn("acpid", pwr_task);
    pm.wt = task_at(id);
    idt_set_handler(0x20 + pm.sci, sci_isr);
    pic_clear_mask(pm.sci);
}

void acpi_poweroff(void) {
    ext2_sync_all();
    fatfs_sync_all();
    cli();
    if (pm.pm1a_cnt && pm.s5a >= 0) {
        outw(pm.pm1a_cnt, (pm.s5a << 10) | (1 << 13));
        if (pm.pm1b_cnt) outw(pm.pm1b_cnt, (pm.s5b << 10) | (1 << 13));
        for (int i = 0; i < 100000; i++) io_wait();
    }
    outw(0x604, 0x2000);                       /* qemu */
    outw(0xB004, 0x2000);                      /* old qemu, bochs */
    outw(0x4004, 0x3400);                      /* virtualbox */
    for (;;) hlt();
}

void acpi_reboot(void) {
    ext2_sync_all();
    fatfs_sync_all();
    cli();
    if (pm.reset_addr) {
        if (pm.reset_sp == 1) outb((uint16_t)pm.reset_addr, pm.reset_val);
        else *(volatile uint8_t*)P2V(pm.reset_addr) = pm.reset_val;
        for (int i = 0; i < 20000; i++) io_wait();
    }
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    for (int i = 0; i < 20000; i++) io_wait();
    outb(0xCF9, 0x06);                         /* pci reset */
    for (int i = 0; i < 20000; i++) io_wait();
    // nothing worked, triple fault
    __asm__ volatile ("lidt (%0); int3" : : "r"((void*)0));
    for (;;) hlt();
}
