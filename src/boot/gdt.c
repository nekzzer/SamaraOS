#include "boot/gdt.h"
#include "core/string.h"
#include "core/types.h"

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss32 {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[7];                  /* 0..4 segments, 5 TSS, 6 user TLS */
static struct gdt_ptr   gdtp;
static struct tss32     tss;

extern uint8_t boot_stack[];
#define BOOT_STACK_TOP ((uint32_t)boot_stack + (4u * 1024u * 1024u))

static void set_gate(int n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[n].base_lo  = base & 0xFFFF;
    gdt[n].base_mid = (base >> 16) & 0xFF;
    gdt[n].base_hi  = (base >> 24) & 0xFF;
    gdt[n].limit_lo = limit & 0xFFFF;
    gdt[n].flags_limit_hi = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[n].access   = access;
}

static void tss_install(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0  = 0x10;                              /* kernel data */
    tss.esp0 = BOOT_STACK_TOP;
    tss.iomap_base = sizeof(tss);                 /* no I/O bitmap */

    uint32_t base  = (uint32_t)&tss;
    uint32_t limit = sizeof(tss) - 1;
    /* access=0x89: present, ring0, 32-bit available TSS. gran=0x00 (byte). */
    set_gate(5, base, limit, 0x89, 0x00);
}

void tss_set_esp0(uint32_t esp0) { tss.esp0 = esp0; }

/* Slot 6 is the per-process thread pointer segment that musl's
   set_thread_area() asks for; user code loads it into %gs as 0x33. */
void gdt_set_tls(uint32_t base) {
    set_gate(GDT_TLS_INDEX, base, 0xFFFFF, 0xF2, 0xCF);
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;

    set_gate(0, 0, 0, 0, 0);                       /* null */
    set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);        /* kernel code */
    set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);        /* kernel data */
    set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);        /* user code */
    set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);        /* user data */
    tss_install();
    gdt_set_tls(0);

    __asm__ volatile (
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n"
        "1:\n\t"
        "mov $0x28, %%ax\n\t"                      /* TSS selector (index 5, RPL 0) */
        "ltr %%ax\n\t"
        : : "r"(&gdtp) : "ax", "memory"
    );
}
