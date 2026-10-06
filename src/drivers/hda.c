#include "drivers/hda.h"
#include "drivers/pci.h"
#include "core/vmm.h"
#include "core/io.h"
#include "core/string.h"
#include "boot/apic.h"

#define GCAP    0x00
#define GCTL    0x08
#define STATESTS 0x0E
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP  0x48
#define CORBRP  0x4A
#define CORBCTL 0x4C
#define CORBSIZE 0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP  0x58
#define RINTCNT 0x5A
#define RIRBCTL 0x5C
#define RIRBSIZE 0x5E

int snprintf(char* buf, size_t n, const char* fmt, ...);
static volatile uint8_t* mmio;
static bool present;
static const char* status = "no hda";
static uint32_t* corb;
static uint64_t* rirb;
static uint8_t* ring;
static uint32_t* bdl;
static int wp, sd, cad;
static int cad_afg, dac, vol_nid, vol_steps, vol_off;
static int path[8], psel[8], np;
static int vol[2], vsw = 1;

static uint8_t  r8(int r)  { return *(volatile uint8_t*)(mmio + r); }
static uint16_t r16(int r) { return *(volatile uint16_t*)(mmio + r); }
static uint32_t r32(int r) { return *(volatile uint32_t*)(mmio + r); }
static void w8(int r, uint8_t v)   { *(volatile uint8_t*)(mmio + r) = v; }
static void w16(int r, uint16_t v) { *(volatile uint16_t*)(mmio + r) = v; }
static void w32(int r, uint32_t v) { *(volatile uint32_t*)(mmio + r) = v; }

// tsc, not port 0x80: every inb was a vmexit and udelay(1) took way more than 1us under kvm
static void udelay(int us) {
    uint64_t t = tsc_us();
    for (int n = 0; tsc_us() - t < (uint64_t)us && n < us * 1000; n++) __asm__ volatile ("pause");   // no tsc yet: bounded anyway
}

// codec answers in a few us on qemu. the old loop was 200k port reads under the bkl:
// pipewire reconfiguring the stream froze the whole desktop (typing lagged)
static uint32_t cmd(int nid, uint32_t verb) {
    wp = (wp + 1) & 255;
    corb[wp] = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | verb;
    uint16_t old = r16(RIRBWP) & 255;
    w16(CORBWP, wp);
    uint64_t t = tsc_us();
    for (int n = 0; tsc_us() - t < 2000 && n < 2000000; n++) {
        if ((r16(RIRBWP) & 255) != old) { w8(0x5D, 5); return (uint32_t)rirb[r16(RIRBWP) & 255]; }
        __asm__ volatile ("pause");
    }
    return 0xFFFFFFFF;
}

static uint32_t par(int nid, int p) { return cmd(nid, 0xF0000 | p); }
static uint32_t get(int nid, int v) { return cmd(nid, (uint32_t)v << 8); }

static int wtype(int nid) { return (par(nid, 9) >> 20) & 15; }

static void amp_caps(int nid, int in, int* steps, int* off) {
    uint32_t wc = par(nid, 9);
    uint32_t c = 0;
    *steps = -1;
    if (wc & (in ? 2 : 4)) c = par(nid, in ? 0x0D : 0x12);
    if (!c && (wc & 8) == 0) c = par(cad_afg, in ? 0x0D : 0x12);
    if (!(wc & (in ? 2 : 4))) return;
    *steps = (c >> 8) & 0x7F;
    *off = c & 0x7F;
}

// dfs from the pin down to a dac, path[0] is the pin
static bool find(int nid, int depth) {
    int t = wtype(nid);
    if (t == 0) { dac = nid; path[depth] = nid; psel[depth] = 0; np = depth + 1; return true; }
    if (depth > 5 || t == 1) return false;
    uint32_t wc = par(nid, 9);
    if (!(wc & 0x100)) return false;
    uint32_t n = par(nid, 0xE);
    int len = n & 0x7F;
    bool lng = n & 0x80;
    for (int i = 0; i < len; i++) {
        uint32_t r = cmd(nid, 0xF0200 | i);
        int c = lng ? (r >> ((i & 1) * 16)) & 0x7FFF : (r >> ((i & 3) * 8)) & 0x7F;
        if (find(c, depth + 1)) { path[depth] = nid; psel[depth] = i; return true; }
    }
    return false;
}

static void amp_set(int nid, int out, int idx, int gain, int mute) {
    uint32_t p = (out ? 0x8000 : 0x4000) | 0x3000 | (idx << 8) | (gain & 0x7F) | (mute ? 0x80 : 0);
    cmd(nid, 0x30000 | p);
}

static void vol_apply(void) {
    if (vol_nid < 0) return;
    uint32_t l = 0xA000 | (vol[0] & 0x7F) | (vsw ? 0 : 0x80);
    uint32_t r = 0x9000 | (vol[1] & 0x7F) | (vsw ? 0 : 0x80);
    cmd(vol_nid, 0x30000 | l);
    cmd(vol_nid, 0x30000 | r);
}

static int pick_pin(int afg, int start, int count) {
    int best = -1, bs = 0;
    for (int n = start; n < start + count; n++) {
        if (wtype(n) != 4) continue;
        if (!(par(n, 0xC) & 0x10)) continue;
        uint32_t cfg = get(n, 0xF1C);
        if ((cfg >> 30) == 1) continue;       // no connection
        int dev = (cfg >> 20) & 15, s = 1;
        if (dev == 0) s = 4; else if (dev == 1) s = 3; else if (dev == 2) s = 2;
        if (s > bs) { bs = s; best = n; }
    }
    return best;
}

bool hda_init(void) {
    static pci_dev_t devs[32];
    pci_dev_t d;
    int n = pci_scan(devs, 32), ok = 0;
    for (int i = 0; i < n; i++)
        if (devs[i].class_c == 4 && devs[i].subclass == 3) { d = devs[i]; ok = 1; break; }
    if (!ok) return false;
    pci_enable_io_busmaster(&d);
    mmio = (volatile uint8_t*)P2V(d.bar[0] & ~0xFu);

    w32(GCTL, 0);
    for (int i = 0; i < 1000 && (r32(GCTL) & 1); i++) udelay(10);
    udelay(100);
    w32(GCTL, 1);
    int i;
    for (i = 0; i < 1000 && !(r32(GCTL) & 1); i++) udelay(10);
    if (i == 1000) { status = "reset failed"; return false; }
    for (i = 0; i < 20000 && !(r16(STATESTS) & 0x7FFF); i++) udelay(10);
    int st = r16(STATESTS) & 0x7FFF;
    if (!st) { status = "no codec"; return false; }
    cad = 0;
    while (!(st & (1 << cad))) cad++;

    uint64_t pg = pmm_alloc();
    uint64_t pg2 = pmm_alloc();
    uint64_t rg = pmm_alloc_run(HDA_BUF / 4096);
    if (!pg || !pg2 || !rg) { status = "no mem"; return false; }
    corb = P2V(pg);
    rirb = (uint64_t*)((uint8_t*)P2V(pg) + 2048);
    bdl = P2V(pg2);
    ring = P2V(rg);

    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    w8(CORBSIZE, 2);      // 256 entries
    w8(RIRBSIZE, 2);
    w32(CORBLBASE, (uint32_t)pg); w32(CORBUBASE, (uint32_t)(pg >> 32));
    w32(RIRBLBASE, (uint32_t)(pg + 2048)); w32(RIRBUBASE, (uint32_t)(pg >> 32));
    w16(CORBRP, 0x8000);
    for (i = 0; i < 1000 && !(r16(CORBRP) & 0x8000); i++) udelay(10);
    w16(CORBRP, 0);
    for (i = 0; i < 1000 && (r16(CORBRP) & 0x8000); i++) udelay(10);
    w16(CORBWP, 0);
    wp = 0;
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 255);
    w8(CORBCTL, 2);
    w8(RIRBCTL, 2);

    uint32_t sub = par(0, 4);
    int afg = -1;
    for (int f = (sub >> 16) & 255; f < (int)(((sub >> 16) & 255) + (sub & 255)); f++)
        if ((par(f, 5) & 255) == 1) { afg = f; break; }
    if (afg < 0) { static char b[48]; snprintf(b, 48, "no afg sub=%x p5=%x", sub, par(1, 5)); status = b; return false; }
    cad_afg = afg;
    cmd(afg, 0x70500);     // D0
    uint32_t ws = par(afg, 4);
    int start = (ws >> 16) & 255, cnt = ws & 255;
    for (int k = start; k < start + cnt; k++) cmd(k, 0x70500);
    udelay(1000);

    // every output pin that reaches a dac, first good one wins
    int pin = pick_pin(afg, start, cnt);
    if (pin < 0 || !find(pin, 0)) { status = "no output path"; return false; }

    for (int k = 0; k < np; k++) {
        int nid = path[k], steps, off;
        if (k < np - 1) cmd(nid, 0x70100 | psel[k]);
        amp_caps(nid, 0, &steps, &off);
        if (steps >= 0) amp_set(nid, 1, 0, off, 0);
        amp_caps(nid, 1, &steps, &off);
        if (steps >= 0) for (int j = 0; j < 8; j++) amp_set(nid, 0, j, off, 0);
    }
    // volume lives on the dac amp, else on the pin
    int steps, off;
    amp_caps(dac, 0, &steps, &off);
    vol_nid = dac;
    if (steps <= 0) { amp_caps(pin, 0, &steps, &off); vol_nid = pin; }
    if (steps <= 0) { vol_nid = -1; steps = 0; off = 0; }
    vol_steps = steps; vol_off = off;
    vol[0] = vol[1] = steps ? steps * 3 / 4 : 0;
    vol_apply();
    cmd(pin, 0x70700 | 0x40);                       // out enable
    if (par(pin, 0xC) & 0x10000) cmd(pin, 0x70C02); // eapd

    int iss = (r16(GCAP) >> 8) & 15;
    sd = 0x80 + 0x20 * iss;
    present = true;
    status = "ok";
    return true;
}

bool hda_present(void) { return present; }
const char* hda_status(void) { return status; }
uint8_t* hda_ring(void) { return ring; }

void hda_run(bool on) {
    uint8_t c = r8(sd);
    w8(sd, on ? (c | 2) : (c & ~2));
}

uint32_t hda_lpib(void) { return r32(sd + 4); }

void hda_setup(uint32_t rate, uint32_t buf, uint32_t per) {
    int i;
    w8(sd, 0);
    w8(sd, 1);
    for (i = 0; i < 1000 && !(r8(sd) & 1); i++) udelay(10);
    w8(sd, 0);
    for (i = 0; i < 1000 && (r8(sd) & 1); i++) udelay(10);
    memset(ring, 0, buf);
    int n = buf / per;
    uint64_t pa = V2P(ring);
    for (i = 0; i < n; i++) {
        uint64_t a = pa + (uint64_t)i * per;
        bdl[i * 4] = (uint32_t)a;
        bdl[i * 4 + 1] = (uint32_t)(a >> 32);
        bdl[i * 4 + 2] = per;
        bdl[i * 4 + 3] = 0;
    }
    uint16_t fmt = (rate == 44100 ? 0x4000 : 0) | 0x11;   // 16 bit, 2 ch
    w8(sd + 2, 1 << 4);            // stream tag 1
    w32(sd + 8, buf);
    w16(sd + 0xC, n - 1);
    w16(sd + 0x12, fmt);
    uint64_t bp = V2P(bdl);
    w32(sd + 0x18, (uint32_t)bp);
    w32(sd + 0x1C, (uint32_t)(bp >> 32));
    w8(sd + 3, 0x1C);              // clear sts
    static uint16_t last_fmt = 0xFFFF;
    if (fmt != last_fmt) {          // pipewire re-prepares a lot, the codec already knows
        cmd(dac, 0x20000 | fmt);
        cmd(dac, 0x70600 | (1 << 4));
        last_fmt = fmt;
    }
}

int hda_vol_max(void) { return vol_steps; }

void hda_vol_get(int* l, int* r, int* sw) { *l = vol[0]; *r = vol[1]; *sw = vsw; }

void hda_vol_set(int l, int r, int sw) {
    if (l > vol_steps) l = vol_steps;
    if (r > vol_steps) r = vol_steps;
    if (l == vol[0] && r == vol[1] && sw == vsw) return;
    vol[0] = l < 0 ? 0 : l;
    vol[1] = r < 0 ? 0 : r;
    vsw = sw;
    vol_apply();
}
