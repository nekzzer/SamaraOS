#include "boot/idt.h"
#include "core/string.h"
#include "core/io.h"
#include "boot/pic.h"
#include "drivers/vga.h"
#include "proc/proc.h"

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

/* ---- defaults, so no vector is ever left without a gate ----
   Real hardware can raise IRQs QEMU never does (and a missing gate turns
   into #GP with an IDT-flagged error code). */

static void irq_default(int irq) {
    if (irq == 7 || irq == 15) {                   /* spurious unless in service */
        if (!(pic_isr() & (1u << irq))) {
            if (irq == 15) outb(PIC1_CMD, PIC_EOI); /* master saw the cascade */
            return;
        }
    }
    pic_set_mask((uint8_t)irq);                    /* nobody handles it: silence */
    pic_send_eoi((uint8_t)irq);
}

#define IRQ_STUB(n) \
    __attribute__((interrupt)) static void irq_stub_##n(struct interrupt_frame* f) { (void)f; irq_default(n); }
IRQ_STUB(0)  IRQ_STUB(1)  IRQ_STUB(2)  IRQ_STUB(3)  IRQ_STUB(4)  IRQ_STUB(5)  IRQ_STUB(6)  IRQ_STUB(7)
IRQ_STUB(8)  IRQ_STUB(9)  IRQ_STUB(10) IRQ_STUB(11) IRQ_STUB(12) IRQ_STUB(13) IRQ_STUB(14) IRQ_STUB(15)

static void com_str(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

static void exc_default(int vec, uint32_t err, struct interrupt_frame* f) {
    if ((f->cs & 3) == 3) proc_fault_kill("CPU exception", 11, f->eip, (uint32_t)vec);
    char b[16];
    com_str("\r\n[EXC] vector ");
    itoa(vec, b, 10); com_str(b);
    com_str(" err="); utoa(err, b, 16); com_str(b);
    com_str(" eip="); utoa(f->eip, b, 16); com_str(b);
    com_str("\r\n");
    vga_printf("\n[EXC %d] err=0x%x eip=0x%x -- halted\n", vec, err, f->eip);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

#define EXC_STUB(n) \
    __attribute__((interrupt)) static void exc_stub_##n(struct interrupt_frame* f) { exc_default(n, 0, f); }
#define EXC_STUB_ERR(n) \
    __attribute__((interrupt)) static void exc_stub_##n(struct interrupt_frame* f, uint32_t e) { exc_default(n, e, f); }
EXC_STUB(0) EXC_STUB(1) EXC_STUB(2) EXC_STUB(3) EXC_STUB(4) EXC_STUB(5) EXC_STUB(6) EXC_STUB(7)
EXC_STUB_ERR(8) EXC_STUB(9) EXC_STUB_ERR(10) EXC_STUB_ERR(11) EXC_STUB_ERR(12) EXC_STUB_ERR(13)
EXC_STUB_ERR(14) EXC_STUB(15) EXC_STUB(16) EXC_STUB_ERR(17) EXC_STUB(18) EXC_STUB(19) EXC_STUB(20)
EXC_STUB_ERR(21) EXC_STUB(22) EXC_STUB(23) EXC_STUB(24) EXC_STUB(25) EXC_STUB(26) EXC_STUB(27)
EXC_STUB(28) EXC_STUB_ERR(29) EXC_STUB_ERR(30) EXC_STUB(31)

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    void* exc[32] = {
        exc_stub_0, exc_stub_1, exc_stub_2, exc_stub_3, exc_stub_4, exc_stub_5, exc_stub_6, exc_stub_7,
        exc_stub_8, exc_stub_9, exc_stub_10, exc_stub_11, exc_stub_12, exc_stub_13, exc_stub_14, exc_stub_15,
        exc_stub_16, exc_stub_17, exc_stub_18, exc_stub_19, exc_stub_20, exc_stub_21, exc_stub_22, exc_stub_23,
        exc_stub_24, exc_stub_25, exc_stub_26, exc_stub_27, exc_stub_28, exc_stub_29, exc_stub_30, exc_stub_31,
    };
    void* irq[16] = {
        irq_stub_0, irq_stub_1, irq_stub_2, irq_stub_3, irq_stub_4, irq_stub_5, irq_stub_6, irq_stub_7,
        irq_stub_8, irq_stub_9, irq_stub_10, irq_stub_11, irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15,
    };
    for (int i = 0; i < 32; i++) idt_set_gate(i, exc[i], 0x08, 0x8E);
    for (int i = 0; i < 16; i++) idt_set_gate(0x20 + i, irq[i], 0x08, 0x8E);
    __asm__ volatile ("lidt (%0)" : : "r"(&idtp));
}
