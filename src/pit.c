#include "pit.h"
#include "io.h"
#include "pic.h"

/* Sentinel guarded at the bottom of the boot stack so we notice an overflow
   before it scribbles over critical .bss / page tables. */
extern uint32_t* boot_stack_sentinel;
#define BOOT_STACK_GUARD 0xDEADC0DEu

static void com_putc_local(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) {}
    outb(0x3F8, c);
}
static void com_str_local(const char* s) { while (*s) com_putc_local(*s++); }
static void com_hex_local(uint32_t v) {
    com_str_local("0x");
    const char* h = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) com_putc_local(h[(v >> (i*4)) & 0xF]);
}

void pit_check_stack_guard(uint32_t cur_esp) {
    if (!boot_stack_sentinel) return;
    if (*boot_stack_sentinel == BOOT_STACK_GUARD) return;
    /* Sentinel clobbered — stack overflow has happened or is about to. */
    com_str_local("\r\n[STACK OVERFLOW] sentinel=");
    com_hex_local(*boot_stack_sentinel);
    com_str_local(" esp=");
    com_hex_local(cur_esp);
    com_str_local(" -- halting\r\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

#define PIT_FREQ 1193182U

static volatile uint32_t ticks_ = 0;
static uint32_t hz_ = 100;

uint32_t pit_ticks(void) { return ticks_; }
uint32_t pit_uptime_ms(void) { return (ticks_ * 1000U) / hz_; }
void     pit_tick_inc(void) { ticks_++; }    /* called from scheduler ISR */

void pit_init(uint32_t hz) {
    hz_ = hz;
    uint32_t div = PIT_FREQ / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)((div >> 8) & 0xFF));
    pic_clear_mask(0);
}
