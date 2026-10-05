#include "core/prof.h"
#include "core/string.h"
#include "core/smp.h"

int prof_on, prof_gen;
uint64_t pf_anon, pf_cow, pf_file, pf_kern, tlb_ipis, tlb_rounds, uwin_bytes, bkl_wait, bkl_takes, bkl_slow;

#define HN 8192
static struct { uint64_t rip; uint32_t n; } ktab[HN];
static struct { char name[16]; uint32_t n; } utab[64];
static uint32_t n_idle, n_user, n_kern, n_lost;
static uint64_t sc_n[512], sc_cyc[512];

void prof_sample(uint64_t rip, bool user, bool idle, const char* name) {
    if (idle) { n_idle++; return; }
    if (user) {
        n_user++;
        for (int i = 0; i < 64; i++) {
            if (!utab[i].name[0]) strncpy(utab[i].name, name, 15);
            if (!strncmp(utab[i].name, name, 15)) { utab[i].n++; return; }
        }
        return;
    }
    n_kern++;
    uint32_t h = (uint32_t)((rip >> 2) * 2654435761u) % HN;
    for (int i = 0; i < 64; i++, h = (h + 1) % HN) {
        if (!ktab[h].n) ktab[h].rip = rip;
        if (ktab[h].rip == rip) { ktab[h].n++; return; }
    }
    n_lost++;
}

void prof_sys(int nr, uint64_t cyc) {
    if (nr < 0 || nr >= 512) return;
    sc_n[nr]++;
    sc_cyc[nr] += cyc;
}

void prof_cmd(const char* s, uint32_t n) {
    prof_gen++;
    if (n >= 5 && !strncmp(s, "start", 5)) prof_on = 1;
    else if (n >= 4 && !strncmp(s, "stop", 4)) prof_on = 0;
    else if (n >= 5 && !strncmp(s, "reset", 5)) {
        memset(ktab, 0, sizeof(ktab));
        memset(utab, 0, sizeof(utab));
        memset(sc_n, 0, sizeof(sc_n));
        memset(sc_cyc, 0, sizeof(sc_cyc));
        n_idle = n_user = n_kern = n_lost = 0;
        pf_anon = pf_cow = pf_file = pf_kern = tlb_ipis = tlb_rounds = uwin_bytes = bkl_wait = bkl_takes = bkl_slow = 0;
    }
}

static char* pn(char* p, uint64_t v, int base) {
    char t[24]; int i = 0;
    do { t[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (i) *p++ = t[--i];
    return p;
}

static char* ps(char* p, const char* s) { while (*s) *p++ = *s++; return p; }

int prof_dump(char* buf, int cap) {
    char* p = buf;
    char* end = buf + cap - 64;
    p = ps(p, "T idle "); p = pn(p, n_idle, 10);
    p = ps(p, " user "); p = pn(p, n_user, 10);
    p = ps(p, " kern "); p = pn(p, n_kern, 10);
    p = ps(p, " lost "); p = pn(p, n_lost, 10); *p++ = '\n';
    struct { const char* n; uint64_t v; } c[] = {
        { "pf_anon", pf_anon }, { "pf_cow", pf_cow }, { "pf_file", pf_file }, { "pf_kern", pf_kern },
        { "tlb_ipis", tlb_ipis }, { "tlb_rounds", tlb_rounds }, { "uwin_bytes", uwin_bytes },
        { "bkl_takes", bkl_takes }, { "bkl_slow", bkl_slow }, { "bkl_wait_cyc", bkl_wait },
    };
    for (int i = 0; i < 10; i++) { p = ps(p, "C "); p = ps(p, c[i].n); *p++ = ' '; p = pn(p, c[i].v, 10); *p++ = '\n'; }
    for (int i = 0; i < 512 && p < end; i++) {
        if (!sc_n[i]) continue;
        p = ps(p, "Y "); p = pn(p, i, 10); *p++ = ' '; p = pn(p, sc_n[i], 10); *p++ = ' '; p = pn(p, sc_cyc[i], 10); *p++ = '\n';
    }
    for (int i = 0; i < 64 && p < end; i++) {
        if (!utab[i].n) continue;
        p = ps(p, "U "); p = ps(p, utab[i].name); *p++ = ' '; p = pn(p, utab[i].n, 10); *p++ = '\n';
    }
    for (int i = 0; i < HN && p < end; i++) {
        if (!ktab[i].n) continue;
        p = ps(p, "K "); p = pn(p, ktab[i].rip, 16); *p++ = ' '; p = pn(p, ktab[i].n, 10); *p++ = '\n';
    }
    return p - buf;
}
