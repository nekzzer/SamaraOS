#include "idt.h"
#include "string.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t off_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void idt_set_gate(int n, void* handler, uint16_t selector, uint8_t flags) {
    uint32_t addr = (uint32_t)handler;
    idt[n].off_lo   = addr & 0xFFFF;
    idt[n].off_hi   = (addr >> 16) & 0xFFFF;
    idt[n].selector = selector;
    idt[n].zero     = 0;
    idt[n].flags    = flags;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    __asm__ volatile ("lidt (%0)" : : "r"(&idtp));
}
