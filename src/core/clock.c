#include "core/clock.h"
#include "core/io.h"
#include "boot/pit.h"

static uint32_t boot_epoch, boot_ms;

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static int bcd(int v) { return (v >> 4) * 10 + (v & 0x0F); }

static uint32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

void clock_init(void) {
    while (cmos(0x0A) & 0x80) {}
    int s = cmos(0x00), mi = cmos(0x02), h = cmos(0x04);
    int d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    int regb = cmos(0x0B);
    bool pm = (h & 0x80) != 0;
    h &= 0x7F;
    if (!(regb & 0x04)) { s = bcd(s); mi = bcd(mi); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(regb & 0x02)) { h %= 12; if (pm) h += 12; }
    boot_epoch = days_from_civil(2000 + y, mo, d) * 86400u + (uint32_t)(h * 3600 + mi * 60 + s);
    boot_ms = pit_uptime_ms();
}

void clock_now(uint32_t* sec, uint32_t* nsec) {
    uint32_t ms = pit_uptime_ms() - boot_ms;
    *sec = boot_epoch + ms / 1000;
    if (nsec) *nsec = (ms % 1000) * 1000000u;
}

uint32_t clock_epoch(void) { uint32_t s; clock_now(&s, 0); return s; }
