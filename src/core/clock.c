#include "core/clock.h"
#include "core/io.h"
#include "boot/pit.h"
#include "boot/apic.h"
#include "boot/idt.h"
#include "boot/pic.h"
#include "core/smp.h"
#include "core/wq.h"

static int64_t wall_off;                 /* us: wall clock = uptime us + this */
static spin_t cmos_lk;
static volatile uint32_t rtc_ups;       /* update-ended irqs so far */
static int rtc_uie;
wq_t rtc_wq;

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

static uint32_t civil_days(int y, int m, int d) { return days_from_civil(y, m, d); }

static void civil_from_days(uint32_t z, int* y, int* m, int* d) {
    int64_t zz = (int64_t)z + 719468;
    int64_t era = zz / 146097;
    int doe = (int)(zz - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static uint32_t rtc_read_epoch(void) {
    uint64_t fl = spin_lock(&cmos_lk);
    while (cmos(0x0A) & 0x80) {}
    int s = cmos(0x00), mi = cmos(0x02), h = cmos(0x04);
    int d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    int regb = cmos(0x0B);
    spin_unlock(&cmos_lk, fl);
    bool pm = (h & 0x80) != 0;
    h &= 0x7F;
    if (!(regb & 0x04)) { s = bcd(s); mi = bcd(mi); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(regb & 0x02)) { h %= 12; if (pm) h += 12; }
    return civil_days(2000 + y, mo, d) * 86400u + (uint32_t)(h * 3600 + mi * 60 + s);
}

static void rtc_write_epoch(uint32_t t) {
    int y, mo, d;
    civil_from_days(t / 86400, &y, &mo, &d);
    int h = t % 86400 / 3600, mi = t % 3600 / 60, s = t % 60;
    uint64_t fl = spin_lock(&cmos_lk);
    int regb = cmos(0x0B);
    int wr[6] = { s, mi, h, d, mo, y % 100 };
    if (!(regb & 0x04)) for (int i = 0; i < 6; i++) wr[i] = (wr[i] / 10) << 4 | (wr[i] % 10);
    if (!(regb & 0x02)) { outb(0x70, 0x0B); outb(0x71, regb | 2); }       // 24h, we never write 12h
    outb(0x70, 0x0B); outb(0x71, cmos(0x0B) | 0x80);
    static const uint8_t regs[6] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };
    for (int i = 0; i < 6; i++) { outb(0x70, regs[i]); outb(0x71, wr[i]); }
    outb(0x70, 0x0B); outb(0x71, cmos(0x0B) & ~0x80);
    spin_unlock(&cmos_lk, fl);
}

static void rtc_isr(regs_t* r) {
    (void)r;
    uint64_t fl = spin_lock(&cmos_lk);
    uint8_t c = cmos(0x0C);
    spin_unlock(&cmos_lk, fl);
    if (c & 0x10) { rtc_ups++; wq_wake(&rtc_wq); }
    pic_send_eoi(8);
}

static uint64_t now_us(void) {
    uint64_t us = tsc_us();
    if (!us) us = (uint64_t)pit_uptime_ms() * 1000;
    return us + wall_off;
}

void clock_init(void) {
    uint32_t e = rtc_read_epoch();
    uint64_t us = tsc_us();
    if (!us) us = (uint64_t)pit_uptime_ms() * 1000;
    wall_off = (int64_t)e * 1000000 - (int64_t)us;
    idt_set_handler(0x28, rtc_isr);
}

/* the irq only gets unmasked once somebody opens /dev/rtc0 */
void clock_rtc_uie(int on) {
    uint64_t fl = spin_lock(&cmos_lk);
    outb(0x70, 0x0B);
    uint8_t b = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, on ? b | 0x10 : b & ~0x10);
    cmos(0x0C);
    spin_unlock(&cmos_lk, fl);
    if (on && !rtc_uie) pic_clear_mask(8);
    else if (!on) pic_set_mask(8);
    rtc_uie = on;
}

uint32_t clock_rtc_ups(void) { return rtc_ups; }

void clock_rtc_get(int* tm) {
    uint32_t t = rtc_read_epoch();
    int y, mo, d;
    civil_from_days(t / 86400, &y, &mo, &d);
    tm[0] = t % 60; tm[1] = t % 3600 / 60; tm[2] = t % 86400 / 3600;
    tm[3] = d; tm[4] = mo - 1; tm[5] = y - 1900;
    tm[6] = (t / 86400 + 4) % 7;
    tm[7] = civil_days(y, mo, d) - civil_days(y, 1, 1);
    tm[8] = 0;
}

int clock_rtc_set(const int* tm) {
    if (tm[0] < 0 || tm[0] > 59 || tm[1] < 0 || tm[1] > 59 || tm[2] < 0 || tm[2] > 23 ||
        tm[3] < 1 || tm[3] > 31 || tm[4] < 0 || tm[4] > 11 || tm[5] < 100 || tm[5] > 199) return -1;
    uint32_t t = civil_days(tm[5] + 1900, tm[4] + 1, tm[3]) * 86400u + tm[2] * 3600 + tm[1] * 60 + tm[0];
    rtc_write_epoch(t);
    return 0;
}

/* settimeofday / clock_settime: wall clock and the cmos both */
void clock_set(uint32_t sec, uint32_t nsec) {
    uint64_t us = tsc_us();
    if (!us) us = (uint64_t)pit_uptime_ms() * 1000;
    wall_off = ((int64_t)sec * 1000000 + nsec / 1000) - (int64_t)us;
    rtc_write_epoch(sec);
}

void clock_now(uint32_t* sec, uint32_t* nsec) {
    uint64_t us = now_us();
    *sec = (uint32_t)(us / 1000000);
    if (nsec) *nsec = (uint32_t)(us % 1000000) * 1000;
}

void clock_now_us(uint32_t* sec, uint32_t* usec) {
    uint64_t us = now_us();
    *sec = (uint32_t)(us / 1000000);
    *usec = (uint32_t)(us % 1000000);
}

uint32_t clock_epoch(void) { return (uint32_t)(now_us() / 1000000); }
