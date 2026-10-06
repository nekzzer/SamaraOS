#include "core/lat.h"
#include "core/string.h"
#include "core/smp.h"
#include "core/io.h"
#include "boot/apic.h"
#include "boot/pit.h"

extern int snprintf(char* buf, size_t n, const char* fmt, ...);

#define RING 1024
static const char* nm[LAT_N] = { "wm_frame", "wm_gap", "in2present", "in2content", "wl_gap", "wl_in" };
static uint32_t ring[LAT_N][RING];
static uint64_t cnt[LAT_N], sum[LAT_N];
static uint32_t mx[LAT_N];
static uint64_t in_t, in_c, c_pend;
static spin_t ll;

uint64_t lat_now(void) {
    if (!tsc_khz) return (uint64_t)pit_uptime_ms() * 1000;
    uint32_t a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return ((((uint64_t)d << 32) | a) * 1000) / tsc_khz;
}

uint32_t lat_us(uint64_t cyc) { return tsc_khz ? (uint32_t)(cyc * 1000 / tsc_khz) : 0; }

void lat_add(int m, uint32_t us) {
    uint64_t f = spin_lock(&ll);
    ring[m][cnt[m] % RING] = us;
    cnt[m]++;
    sum[m] += us;
    if (us > mx[m]) mx[m] = us;
    spin_unlock(&ll, f);
}

void lat_input(void) {
    uint64_t t = lat_now() + 1;
    if (!in_t) in_t = t;
    if (!in_c) in_c = t;
}

void lat_uw_present(void) {
    if (in_c && !c_pend) { c_pend = in_c; in_c = 0; }
}

void lat_presented(void) {
    uint64_t t = lat_now();
    if (in_t) { lat_add(LAT_IN2PRES, t - in_t + 1); in_t = 0; }
    if (c_pend) {
        uint64_t d = t - c_pend + 1;
        if (d < 500000) lat_add(LAT_IN2CONT, d);    // else it was some unrelated commit
        c_pend = 0;
    }
}

static void com(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

void lat_cmd(const char* s, uint32_t n) {
    if (n >= 4 && !strncmp(s, "mark", 4)) {          // host scripts wait for these on COM1
        com("[lat] ");
        for (uint32_t i = 5; i < n && s[i] != '\n'; i++) { char c[2] = { s[i], 0 }; com(c); }
        com("\r\n");
    }
    if (n >= 4 && !strncmp(s, "dump", 4)) {
        static char b[1500];
        int l = lat_dump(b, sizeof(b));
        com("[lat] begin\r\n");
        for (int i = 0; i < l; i++) { if (b[i] == '\n') com("\r"); char c[2] = { b[i], 0 }; com(c); }
        com("[lat] end\r\n");
    }
    if (n >= 5 && !strncmp(s, "reset", 5)) {
        uint64_t f = spin_lock(&ll);
        memset(cnt, 0, sizeof(cnt));
        memset(sum, 0, sizeof(sum));
        memset(mx, 0, sizeof(mx));
        spin_unlock(&ll, f);
    }
}

int lat_dump(char* buf, int cap) {
    static uint32_t tmp[RING];
    int l = 0;
    for (int m = 0; m < LAT_N && l < cap - 100; m++) {
        uint64_t f = spin_lock(&ll);
        int n = cnt[m] < RING ? (int)cnt[m] : RING;
        memcpy(tmp, ring[m], n * 4);
        uint64_t c = cnt[m], s = sum[m];
        uint32_t x = mx[m];
        spin_unlock(&ll, f);
        // insertion sort, 1k samples, only on read
        for (int i = 1; i < n; i++) {
            uint32_t v = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = v;
        }
        if (!n) { l += snprintf(buf + l, cap - l, "%s n=0\n", nm[m]); continue; }
        l += snprintf(buf + l, cap - l, "%s n=%lu avg=%lu p50=%u p95=%u p99=%u max=%u\n", nm[m],
            (unsigned long)c, (unsigned long)(s / c), tmp[n / 2], tmp[n * 95 / 100], tmp[n * 99 / 100], x);
    }
    return l;
}
