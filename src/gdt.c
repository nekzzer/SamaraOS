#include "gdt.h"
#include "types.h"

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

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdtp;

static void set_gate(int n, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[n].base_lo  = base & 0xFFFF;
    gdt[n].base_mid = (base >> 16) & 0xFF;
    gdt[n].base_hi  = (base >> 24) & 0xFF;
    gdt[n].limit_lo = limit & 0xFFFF;
    gdt[n].flags_limit_hi = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[n].access   = access;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;

    set_gate(0, 0, 0, 0, 0);                       /* null */
    set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);        /* kernel code */
    set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);        /* kernel data */
    set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);        /* user code */
    set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);        /* user data */

    __asm__ volatile (
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n"
        "1:\n"
        : : "r"(&gdtp) : "ax", "memory"
    );
}
