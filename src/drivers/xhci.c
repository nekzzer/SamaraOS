#include "drivers/xhci.h"
#include "drivers/usb.h"
#include "drivers/pci.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"

void slog(const char* s);

typedef struct { uint32_t p0, p1, st, ctl; } trb_t;
typedef struct { trb_t* t; uint64_t pa; int n, enq, cyc; } ring_t;
typedef struct {
    ring_t r;
    int dci, used;
    volatile int done, cc;
    volatile uint32_t resid;
    hid_t* hid;
    int rlen;
    uint8_t rep[64];
} xep_t;

#define MAXSLOT 8
struct xdev {
    uint8_t out[2048] __attribute__((aligned(64)));
    uint8_t in[2112] __attribute__((aligned(64)));
    trb_t tr[5][64] __attribute__((aligned(1024)));
    uint8_t buf[1024] __attribute__((aligned(64)));
    xep_t ep[5];
    int slot, port, speed, used;
};

static volatile uint8_t *cap, *op, *rt, *db;
static int csz, nports, maxslots;
static uint64_t dcbaa[256] __attribute__((aligned(4096)));
static trb_t cmdr_t[256] __attribute__((aligned(4096)));
static trb_t evr[256] __attribute__((aligned(4096)));
static uint64_t erst[2] __attribute__((aligned(64)));
static struct xdev devs[MAXSLOT + 1] __attribute__((aligned(4096)));
static ring_t cmdr;
static int evi, evc = 1;
static volatile int polling, cmd_done, cmd_cc, cmd_slot;
static uint8_t* bounce;
static bool up;
static uint32_t retry[32];

#define R32(b, o) (*(volatile uint32_t*)((b) + (o)))
#define W32(b, o, v) (*(volatile uint32_t*)((b) + (o)) = (v))
#define PORTSC(p) (0x400 + 0x10 * (p))

static void ring_init(ring_t* r, trb_t* t, int n) {
    memset(t, 0, n * sizeof(trb_t));
    r->t = t; r->pa = V2P(t); r->n = n; r->enq = 0; r->cyc = 1;
    t[n - 1].p0 = (uint32_t)r->pa; t[n - 1].p1 = r->pa >> 32;
    t[n - 1].ctl = (6 << 10) | 2;
}

static uint64_t ring_push(ring_t* r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl) {
    trb_t* t = &r->t[r->enq];
    uint64_t pa = r->pa + r->enq * 16;
    t->p0 = p0; t->p1 = p1; t->st = st;
    __asm__ volatile ("" ::: "memory");
    t->ctl = ctl | r->cyc;
    if (++r->enq == r->n - 1) {
        __asm__ volatile ("" ::: "memory");
        r->t[r->enq].ctl = (6 << 10) | 2 | r->cyc;
        r->enq = 0;
        r->cyc ^= 1;
    }
    return pa;
}

static xdev_t* by_slot(int s) { return s > 0 && s <= MAXSLOT && devs[s].used ? &devs[s] : 0; }

static void ev_poll(void) {
    if (polling) return;
    polling = 1;
    for (;;) {
        trb_t* e = &evr[evi];
        if ((int)(e->ctl & 1) != evc) break;
        int type = e->ctl >> 10 & 63;
        if (type == 32) {
            xdev_t* d = by_slot(e->ctl >> 24);
            int dci = e->ctl >> 16 & 31;
            if (d) for (int i = 0; i < 5; i++) {
                xep_t* x = &d->ep[i];
                if (!x->used || x->dci != dci) continue;
                x->cc = e->st >> 24;
                x->resid = e->st & 0xFFFFFF;
                x->done = 1;
            }
        } else if (type == 33) {
            cmd_cc = e->st >> 24;
            cmd_slot = e->ctl >> 24;
            cmd_done = 1;
        }
        if (++evi == 256) { evi = 0; evc ^= 1; }
    }
    uint64_t pa = V2P(evr) + evi * 16;
    W32(rt, 0x38, (uint32_t)pa | 8);
    W32(rt, 0x3C, pa >> 32);
    polling = 0;
}

static int cmd(uint32_t p0, uint32_t p1, uint32_t ctl) {
    cmd_done = 0;
    ring_push(&cmdr, p0, p1, 0, ctl);
    W32(db, 0, 0);
    uint32_t end = pit_uptime_ms() + 1000;
    while (!cmd_done) {
        ev_poll();
        if ((int32_t)(pit_uptime_ms() - end) > 0) { slog("xhci: cmd timeout"); return -1; }
        __builtin_ia32_pause();
    }
    return cmd_cc == 1 ? 0 : -cmd_cc;
}

static int wait_ep(xep_t* e, int ms) {
    uint32_t end = pit_uptime_ms() + ms;
    while (!e->done) {
        ev_poll();
        if ((int32_t)(pit_uptime_ms() - end) > 0) return -999;
        __builtin_ia32_pause();
    }
    return e->cc == 1 || e->cc == 13 ? 0 : -e->cc;
}

static int ctl_xfer(xdev_t* d, uint8_t rtype, uint8_t rq, uint16_t val, uint16_t idx, uint16_t len, void* data) {
    xep_t* e = &d->ep[0];
    int in = rtype & 0x80;
    if (len > 1024) return -1;
    e->done = 0;
    ring_push(&e->r, rtype | rq << 8 | val << 16, idx | len << 16, 8, (2 << 10) | (1 << 6) | ((len ? in ? 3 : 2 : 0) << 16));
    if (len) {
        if (!in) memcpy(d->buf, data, len);
        uint64_t pa = V2P(d->buf);
        ring_push(&e->r, (uint32_t)pa, pa >> 32, len, (3 << 10) | (in ? 1 << 16 : 0));
    }
    ring_push(&e->r, 0, 0, 0, (4 << 10) | (1 << 5) | (len && in ? 0 : 1 << 16));
    W32(db, d->slot * 4, 1);
    int r = wait_ep(e, 1000);
    if (r == 0 && in && len) memcpy(data, d->buf, len);
    return r;
}

int xhci_bulk(xdev_t* d, int ep, bool in, void* buf, int len) {
    xep_t* e = &d->ep[ep];
    if (!d->used || len > 65536) return -1;
    if (!in) memcpy(bounce, buf, len);
    uint64_t pa = V2P(bounce);
    e->done = 0;
    ring_push(&e->r, (uint32_t)pa, pa >> 32, len, (1 << 10) | (1 << 5));
    W32(db, d->slot * 4, e->dci);
    int r = wait_ep(e, 10000);
    if (r < 0) return r;
    int got = len - (int)e->resid;
    if (in) memcpy(buf, bounce, got);
    return got;
}

// stall recovery: reset the endpoint, drop the ring, clear_halt on the device
int xhci_unstall(xdev_t* d, int ep) {
    xep_t* e = &d->ep[ep];
    uint8_t addr = (e->dci / 2) | (e->dci & 1 ? 0x80 : 0);
    cmd(0, 0, (14 << 10) | d->slot << 24 | e->dci << 16);
    ring_init(&e->r, d->tr[ep], 64);
    cmd((uint32_t)e->r.pa | 1, e->r.pa >> 32, (16 << 10) | d->slot << 24 | e->dci << 16);
    return ctl_xfer(d, 2, 1, 0, addr, 0, 0);
}

static uint32_t* cx(uint8_t* base, int i) { return (uint32_t*)(base + i * csz); }

static void ep_ctx(uint32_t* c, int type, int mps, int ival, ring_t* r) {
    memset(c, 0, 20);
    c[0] = ival << 16;
    c[1] = (3 << 1) | type << 3 | mps << 16;
    c[2] = (uint32_t)r->pa | 1;
    c[3] = r->pa >> 32;
    c[4] = (mps > 8 ? mps : 8) | mps << 16;
}

static void str_desc(xdev_t* d, int idx, char* out, int max) {
    uint8_t s[64];
    out[0] = 0;
    if (!idx || ctl_xfer(d, 0x80, 6, 0x300 | idx, 0x409, 64, s) < 0) return;
    int n = 0;
    for (int i = 2; i + 1 < s[0] && n < max - 1; i += 2) out[n++] = s[i + 1] ? '?' : s[i];
    out[n] = 0;
}

static void remove_dev(xdev_t* d) {
    for (int i = 1; i < 5; i++) {
        xep_t* e = &d->ep[i];
        if (e->hid) hid_free(e->hid), e->hid = 0;
    }
    usbms_detach(d);
    d->used = 0;
    dcbaa[d->slot] = 0;
    cmd(0, 0, (10 << 10) | d->slot << 24);
    slog("xhci: device gone");
}

static int enum_port(int p) {
    uint32_t sc = R32(op, PORTSC(p));
    if (!(sc & 2)) {
        W32(op, PORTSC(p), 0x200 | 0x10);
        uint32_t end = pit_uptime_ms() + 500;
        while (!(R32(op, PORTSC(p)) & (1 << 21)) && (int32_t)(pit_uptime_ms() - end) < 0) task_sleep_ms(1);
        W32(op, PORTSC(p), 0x200 | (R32(op, PORTSC(p)) & 0xFE0000));
        task_sleep_ms(20);
        sc = R32(op, PORTSC(p));
        if (!(sc & 2)) return -1;
    }
    int speed = sc >> 10 & 15;
    if (cmd(0, 0, 9 << 10) < 0) return -1;
    int slot = cmd_slot;
    if (slot < 1 || slot > MAXSLOT) return -1;
    xdev_t* d = &devs[slot];
    memset(d, 0, sizeof(*d));
    d->slot = slot; d->port = p; d->speed = speed;
    ring_init(&d->ep[0].r, d->tr[0], 64);
    d->ep[0].dci = 1; d->ep[0].used = 1;
    int mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    uint32_t* ic = cx(d->in, 0);
    ic[1] = 3;
    cx(d->in, 1)[0] = 1 << 27 | speed << 20;
    cx(d->in, 1)[1] = (p + 1) << 16;
    ep_ctx(cx(d->in, 2), 4, mps, 0, &d->ep[0].r);
    dcbaa[slot] = V2P(d->out);
    d->used = 1;
    uint64_t ipa = V2P(d->in);
    if (cmd((uint32_t)ipa, ipa >> 32, (11 << 10) | slot << 24) < 0) { slog("xhci: addr failed"); goto bad; }

    uint8_t dd[18];
    if (ctl_xfer(d, 0x80, 6, 0x100, 0, 8, dd) < 0) { slog("xhci: desc failed"); goto bad; }
    int m = speed == 4 ? 1 << dd[7] : dd[7];
    if (m != mps && m >= 8) {
        ic[1] = 2;
        ep_ctx(cx(d->in, 2), 4, m, 0, &d->ep[0].r);
        cmd((uint32_t)ipa, ipa >> 32, (13 << 10) | slot << 24);
    }
    if (ctl_xfer(d, 0x80, 6, 0x100, 0, 18, dd) < 0) goto bad;
    uint16_t vid = dd[8] | dd[9] << 8, pid = dd[10] | dd[11] << 8;
    char name[48];
    str_desc(d, dd[15], name, 48);
    uint8_t cd[9];
    if (ctl_xfer(d, 0x80, 6, 0x200, 0, 9, cd) < 0) goto bad;
    int total = cd[2] | cd[3] << 8;
    if (total > 1000) total = 1000;
    uint8_t* cfg = kmalloc(1024);
    if (!cfg) goto bad;
    if (ctl_xfer(d, 0x80, 6, 0x200, 0, total, cfg) < 0) { kfree(cfg); goto bad; }
    if (ctl_xfer(d, 0, 9, cd[5], 0, 0, 0) < 0) { kfree(cfg); goto bad; }

    usb_if_t ifs[8];
    int ni = usb_ifaces(cfg, total, ifs, 8);
    kfree(cfg);
    int nep = 0, maxdci = 1;
    uint32_t add = 1;
    struct { int ifn, kind, ei, eo; hid_t* h; } use[4];
    int nu = 0;
    memcpy(cx(d->in, 1), cx(d->out, 0), csz);
    for (int i = 0; i < ni && nu < 4; i++) {
        usb_if_t* f = &ifs[i];
        int ei = -1, eo = -1;
        if (f->cls == 3) {
            for (int k = 0; k < f->nep; k++)
                if ((f->ep[k].attr & 3) == 3 && (f->ep[k].addr & 0x80) && nep < 4) {
                    xep_t* e = &d->ep[++nep];
                    int dci = (f->ep[k].addr & 15) * 2 + 1;
                    int iv = f->ep[k].interval;
                    iv = speed >= 3 ? iv - 1 : (31 - __builtin_clz((iv ? iv : 1) * 8));
                    if (iv < 0) iv = 0;
                    if (iv > 15) iv = 15;
                    ring_init(&e->r, d->tr[nep], 64);
                    e->dci = dci; e->used = 1;
                    ep_ctx(cx(d->in, 1 + dci), 7, f->ep[k].mps & 0x7FF, iv, &e->r);
                    e->rlen = f->ep[k].mps & 0x7FF;
                    if (e->rlen > 64) e->rlen = 64;
                    add |= 1 << dci;
                    if (dci > maxdci) maxdci = dci;
                    ei = nep;
                    break;
                }
        } else if (f->cls == 8 && f->sub == 6 && f->proto == 0x50) {
            for (int k = 0; k < f->nep; k++) {
                if ((f->ep[k].attr & 3) != 2 || nep >= 4) continue;
                int in = f->ep[k].addr & 0x80;
                if (in ? ei >= 0 : eo >= 0) continue;
                xep_t* e = &d->ep[++nep];
                int dci = (f->ep[k].addr & 15) * 2 + (in ? 1 : 0);
                ring_init(&e->r, d->tr[nep], 64);
                e->dci = dci; e->used = 1;
                ep_ctx(cx(d->in, 1 + dci), in ? 6 : 2, f->ep[k].mps & 0x7FF, 0, &e->r);
                add |= 1 << dci;
                if (dci > maxdci) maxdci = dci;
                if (in) ei = nep; else eo = nep;
            }
            if (ei < 0 || eo < 0) continue;
        } else continue;
        if (ei < 0) continue;
        use[nu].ifn = f->num; use[nu].kind = f->cls == 3 ? 1 : 2; use[nu].ei = ei; use[nu].eo = eo;
        use[nu].h = 0;
        nu++;
    }
    if (!nu) { slog("xhci: nothing to drive"); return 0; }
    ic[0] = 0; ic[1] = add;
    cx(d->in, 1)[0] = (cx(d->in, 1)[0] & 0x07FFFFFF) | maxdci << 27;
    if (cmd((uint32_t)ipa, ipa >> 32, (12 << 10) | slot << 24) < 0) { slog("xhci: cfg ep failed"); goto bad; }

    for (int i = 0; i < nu; i++) {
        if (use[i].kind == 2) { usbms_attach(d, use[i].ei, use[i].eo); continue; }
        usb_if_t* f = 0;
        for (int k = 0; k < ni; k++) if (ifs[k].num == use[i].ifn) f = &ifs[k];
        int boot = f->sub == 1 && f->proto == 1;
        uint8_t* rd = 0;
        int rl = 0;
        if (boot) ctl_xfer(d, 0x21, 0x0B, 0, f->num, 0, 0);
        else if (f->rdlen && f->rdlen <= 1024) {
            rd = kmalloc(f->rdlen);
            rl = f->rdlen;
            if (rd && ctl_xfer(d, 0x81, 6, 0x2200, f->num, f->rdlen, rd) < 0) { kfree(rd); rd = 0; }
        }
        if (!boot && !rd) continue;
        ctl_xfer(d, 0x21, 0x0A, 0, f->num, 0, 0);
        char nm[48];
        if (name[0]) strcpy(nm, name);
        else strcpy(nm, boot ? "USB keyboard" : "USB pointer");
        xep_t* e = &d->ep[use[i].ei];
        e->hid = hid_new(nm, vid, pid, boot, rd, rl);
        if (rd) kfree(rd);
        if (!e->hid) continue;
        e->done = 0;
        uint64_t pa = V2P(e->rep);
        ring_push(&e->r, (uint32_t)pa, pa >> 32, e->rlen, (1 << 10) | (1 << 5) | (1 << 2));
        W32(db, slot * 4, e->dci);
        slog(boot ? "xhci: usb keyboard" : "xhci: usb pointer");
    }
    return 0;
bad:
    remove_dev(d);
    return -1;
}

int xhci_up(void) {
    pci_dev_t devs_[32], *pd = 0;
    int n = pci_scan(devs_, 32);
    for (int i = 0; i < n; i++)
        if (devs_[i].class_c == 0x0C && devs_[i].subclass == 3 && devs_[i].prog_if == 0x30) { pd = &devs_[i]; break; }
    if (!pd) return 0;
    uint64_t base = pd->bar[0] & ~0xFu;
    if ((pd->bar[0] & 6) == 4) base |= (uint64_t)pd->bar[1] << 32;
    pci_cfg_write16(pd->bus, pd->dev, pd->fn, 4, pci_cfg_read16(pd->bus, pd->dev, pd->fn, 4) | 6);
    cap = mmio_map(base, 0x10000);
    if (!cap) return 0;
    op = cap + (cap[0]);
    rt = cap + (R32(cap, 0x18) & ~0x1Fu);
    db = cap + (R32(cap, 0x14) & ~3u);
    uint32_t hcs1 = R32(cap, 4), hcs2 = R32(cap, 8);
    csz = R32(cap, 0x10) & 4 ? 64 : 32;
    nports = hcs1 >> 24;
    maxslots = hcs1 & 0xFF;
    if (maxslots > MAXSLOT) maxslots = MAXSLOT;

    W32(op, 0, R32(op, 0) & ~1u);
    for (int i = 0; i < 100 && !(R32(op, 4) & 1); i++) task_sleep_ms(1);
    W32(op, 0, 2);
    for (int i = 0; i < 200 && (R32(op, 0) & 2); i++) task_sleep_ms(1);
    for (int i = 0; i < 200 && (R32(op, 4) & 0x800); i++) task_sleep_ms(1);

    W32(op, 0x38, maxslots);
    int sp = (hcs2 >> 27 & 31) | (hcs2 >> 21 & 31) << 5;
    if (sp) {
        uint8_t* raw = kmalloc(4096 * (sp + 2));
        uint64_t* arr = (uint64_t*)(((uintptr_t)raw + 4095) & ~4095ul);
        memset(arr, 0, 4096 * (sp + 1));
        for (int i = 0; i < sp; i++) arr[i] = V2P((uint8_t*)arr + 4096 * (i + 1));
        dcbaa[0] = V2P(arr);
    }
    uint64_t pa = V2P(dcbaa);
    W32(op, 0x30, (uint32_t)pa); W32(op, 0x34, pa >> 32);
    ring_init(&cmdr, cmdr_t, 256);
    W32(op, 0x18, (uint32_t)cmdr.pa | 1); W32(op, 0x1C, cmdr.pa >> 32);
    memset(evr, 0, sizeof evr);
    erst[0] = V2P(evr); erst[1] = 256;
    pa = V2P(erst);
    W32(rt, 0x28, 1);
    W32(rt, 0x38, (uint32_t)V2P(evr)); W32(rt, 0x3C, V2P(evr) >> 32);
    W32(rt, 0x30, (uint32_t)pa); W32(rt, 0x34, pa >> 32);
    bounce = kmalloc(65536 * 2);
    bounce = (uint8_t*)(((uintptr_t)bounce + 65535) & ~65535ul);
    W32(op, 0, 1);
    for (int i = 0; i < 100 && (R32(op, 4) & 1); i++) task_sleep_ms(1);
    up = true;
    slog("xhci up");
    return 1;
}

void xhci_poll(void) {
    if (!up) return;
    ev_poll();
    for (int p = 0; p < nports; p++) {
        uint32_t sc = R32(op, PORTSC(p));
        if (sc & 0xFE0000) W32(op, PORTSC(p), 0x200 | (sc & 0xFE0000));
        xdev_t* d = 0;
        for (int s = 1; s <= MAXSLOT; s++) if (devs[s].used && devs[s].port == p) d = &devs[s];
        if ((sc & 1) && !d) {
            if ((int32_t)(pit_uptime_ms() - retry[p & 31]) < 0) continue;
            if (enum_port(p) < 0) { slog("xhci: enum failed"); retry[p & 31] = pit_uptime_ms() + 3000; }
        } else if (!(sc & 1) && d) remove_dev(d);
    }
    for (int s = 1; s <= MAXSLOT; s++) {
        xdev_t* d = &devs[s];
        if (!d->used) continue;
        for (int i = 1; i < 5; i++) {
            xep_t* e = &d->ep[i];
            if (!e->hid) continue;
            hid_tick(e->hid);
            if (!e->done) continue;
            if (e->cc == 1 || e->cc == 13) hid_report(e->hid, e->rep, e->rlen - e->resid);
            e->done = 0;
            if (e->cc != 1 && e->cc != 13 && e->cc != 4) continue;     // stalled or dead, leave it
            uint64_t pa = V2P(e->rep);
            ring_push(&e->r, (uint32_t)pa, pa >> 32, e->rlen, (1 << 10) | (1 << 5) | (1 << 2));
            W32(db, s * 4, e->dci);
        }
    }
}
