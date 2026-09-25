#include "boot/pic.h"
#include "core/io.h"

void pic_remap(void) {

    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();   /* IRQ0..7 -> 0x20..0x27 */
    outb(PIC2_DATA, 0x28); io_wait();   /* IRQ8..15 -> 0x28..0x2F */
    outb(PIC1_DATA, 0x04); io_wait();   /* slave at IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();   /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* Start with every line masked (except the cascade). Restoring the
       BIOS's masks left lines such as a USB controller's IRQ 11 open on real
       hardware; drivers unmask exactly the IRQs they handle. */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

/* In-service register: tells a real IRQ 7/15 from a spurious one. */
uint16_t pic_isr(void) {
    outb(PIC1_CMD, 0x0B);
    outb(PIC2_CMD, 0x0B);
    return (uint16_t)(inb(PIC1_CMD) | (inb(PIC2_CMD) << 8));
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}
