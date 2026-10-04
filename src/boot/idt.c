#include "boot/idt.h"
#include "core/string.h"
#include "core/io.h"
#include "boot/pic.h"
#include "drivers/vga.h"
#include "proc/proc.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;
static void (*handlers[256])(regs_t*);
static regs_t* (*sched_h[256])(regs_t*);

extern uint8_t isr_stubs[];

static void set_gate(int n, uint64_t addr, uint8_t flags, uint8_t ist) {
    idt[n].off_lo  = addr & 0xFFFF;
    idt[n].off_mid = (addr >> 16) & 0xFFFF;
    idt[n].off_hi  = addr >> 32;
    idt[n].selector = 0x08;
    idt[n].ist = ist;
    idt[n].flags = flags;
    idt[n].zero = 0;
}

void idt_set_handler(int vec, void (*fn)(regs_t*)) { handlers[vec] = fn; }
void idt_set_sched(int vec, regs_t* (*fn)(regs_t*)) { sched_h[vec] = fn; }
void idt_set_dpl(int vec, int dpl) { idt[vec].flags = 0x8E | (dpl << 5); }

/* IRQs nobody handles: real hardware raises stuff qemu never does */
static void irq_default(int irq) {
    if (irq == 7 || irq == 15) {                   /* spurious unless in service */
        if (!(pic_isr() & (1u << irq))) {
            if (irq == 15) outb(PIC1_CMD, PIC_EOI);
            return;
        }
    }
    pic_set_mask((uint8_t)irq);
    pic_send_eoi((uint8_t)irq);
}

static void com_str(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

static void exc_default(regs_t* f) {
    if ((f->cs & 3) == 3) proc_fault_kill("CPU exception", 11, f->rip, f->vec);
    char b[24];
    com_str("\r\n[EXC] vector ");
    itoa((int)f->vec, b, 10); com_str(b);
    com_str(" err="); utoa(f->err, b, 16); com_str(b);
    com_str(" rip="); utoa(f->rip, b, 16); com_str(b);
    com_str("\r\n");
    vga_printf("\n[EXC %d] err=0x%lx rip=0x%lx -- halted\n", (int)f->vec, f->err, f->rip);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

int last_vec;
regs_t* isr_dispatch(regs_t* r) {
    int v = (int)r->vec;
    if (v != 14) last_vec = v;
    if (sched_h[v]) return sched_h[v](r);
    if (handlers[v]) handlers[v](r);
    else if (v >= 0x20 && v < 0x30) irq_default(v - 0x20);
    else if (v < 32) exc_default(r);
    return r;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < 256; i++) set_gate(i, (uint64_t)isr_stubs + i * 16, 0x8E, 0);
    idt[8].ist = 1;                                /* #DF */
    idt[2].ist = 2;                                /* NMI */
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt (%0)" : : "r"(&idtp));
}
