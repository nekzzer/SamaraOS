#include "pit.h"
#include "io.h"
#include "pic.h"

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
