#include "core/prof.h"
#include "core/string.h"
#include "core/smp.h"
#include "core/heap.h"
#include "core/io.h"
#include "core/task.h"
#include "boot/apic.h"
#include "boot/gdt.h"
#include "proc/proc.h"

int prof_on, prof_gen;
uint64_t pf_anon, pf_cow, pf_file, pf_kern, tlb_ipis, tlb_rounds, uwin_bytes, bkl_wait, bkl_takes, bkl_slow;

#define HN 8192
static struct { uint64_t rip; uint32_t n; } ktab[HN];
static struct { uint64_t rip; uint32_t n; char name[12]; } utab[HN];
static struct { uint64_t rip; uint32_t n; } htab[2048];     // rips of cpus that hold the bkl
static uint32_t n_idle, n_user, n_kern, n_lost;
static uint64_t sc_n[512], sc_cyc[512];

void prof_sample(uint64_t rip, bool user, bool idle, const char* name) {
    if (idle) { n_idle++; return; }
    uint32_t h = (uint32_t)((rip >> 2) * 2654435761u) % HN;
    if (user) {
        n_user++;
        h = (h + name[0] * 31 + name[1]) % HN;
        for (int i = 0; i < 64; i++, h = (h + 1) % HN) {
            if (!utab[h].n) { utab[h].rip = rip; strncpy(utab[h].name, name, 11); }
            if (utab[h].rip == rip && !strncmp(utab[h].name, name, 11)) { utab[h].n++; return; }
        }
        n_lost++;
        return;
    }
    n_kern++;
    for (int i = 0; i < 64; i++, h = (h + 1) % HN) {
        if (!ktab[h].n) ktab[h].rip = rip;
        if (ktab[h].rip == rip) { ktab[h].n++; return; }
    }
    n_lost++;
}

// nmi from another cpu's tick: iflag doesn't matter here, that was the point
void prof_nmi(struct regs* r) {
    if (!prof_on) return;
    struct cpu* c = NULL;
    int id = lapic_id();
    for (int i = 0; i < ncpu; i++) if (cpus[i].apic_id == id) c = &cpus[i];
    const char* nm = "?";
    if (c && c->cur && c->cur->proc) nm = c->cur->proc->name;
    bool u = (r->cs & 3) == 3;
    if (c && c->bkl && !u) {
        uint32_t h = (uint32_t)((r->rip >> 2) * 2654435761u) % 2048;
        for (int i = 0; i < 32; i++, h = (h + 1) % 2048) {
            if (!htab[h].n) htab[h].rip = r->rip;
            if (htab[h].rip == r->rip) { htab[h].n++; break; }
        }
    }
    prof_sample(r->rip, u, !u && *(uint8_t*)(r->rip - 1) == 0xF4, nm);   // right after hlt = parked
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
    else if (n >= 4 && !strncmp(s, "dump", 4)) {        // straight to COM1, for detached stuff
        static char b[300000];
        int l = prof_dump(b, 300000);
        for (int i = 0; i < l; i++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, b[i]); }
    }
    else if (n >= 5 && !strncmp(s, "reset", 5)) {
        memset(ktab, 0, sizeof(ktab));
        memset(utab, 0, sizeof(utab));
        memset(htab, 0, sizeof(htab));
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
    for (int i = 0; i < HN && p < end; i++) {
        if (!utab[i].n) continue;
        p = ps(p, "U "); p = ps(p, utab[i].name); *p++ = ' '; p = pn(p, utab[i].rip, 16); *p++ = ' '; p = pn(p, utab[i].n, 10); *p++ = '\n';
    }
    for (int i = 0; i < 2048 && p < end; i++) {
        if (!htab[i].n) continue;
        p = ps(p, "H "); p = pn(p, htab[i].rip, 16); *p++ = ' '; p = pn(p, htab[i].n, 10); *p++ = '\n';
    }
    for (int i = 0; i < HN && p < end; i++) {
        if (!ktab[i].n) continue;
        p = ps(p, "K "); p = pn(p, ktab[i].rip, 16); *p++ = ' '; p = pn(p, ktab[i].n, 10); *p++ = '\n';
    }
    return p - buf;
}
