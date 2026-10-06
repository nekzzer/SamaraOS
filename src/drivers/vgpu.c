#include "drivers/vgpu.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "drivers/vga.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/smp.h"
#include "core/vmm.h"
#include "boot/pit.h"

#include "core/io.h"
static void sp(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }
typedef struct { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; } hdr_t;
typedef struct { uint32_t x, y, w, h; } rect_t;

static vm_t vm;
static vmq_t cq, curq;
static bool ok;
static volatile bool busy;
static uint32_t feat;
static bool virgl, venus;
static uint64_t fence_id;
static struct { rect_t r; uint32_t on, flags; } info[16];
static bool changed;

static uint32_t cfg32(int o) { return *(volatile uint32_t*)(vm.dev + o); }

/* control queue: 2 descs per request, slot s uses descs 2s and 2s+1. async ones
   (3d submit/xfer) own their cmd buffer, the answer is just a hdr in the slot */
#define NSLOT 64
static struct { void *cmd, *resp; uint64_t fence; uint8_t st; bool async; hdr_t r; } fl[NSLOT];
static uint64_t done_fence;
static uint64_t dset[1024];          /* fences finished out of order (venus rings) */

static bool qlock(void) {
    uint32_t f = irq_save();
    bool got = !busy;
    if (got) busy = true;
    irq_restore(f);
    return got;
}

/* drain the used ring, never sleeps. skips if somebody else is in the queue */
static void reap(void) {
    if (!qlock()) return;
    vmq_t* q = &cq;
    while (*(volatile uint16_t*)(q->used + 1) != q->last_used) {
        __asm__ volatile ("" ::: "memory");
        uint32_t s = q->used[2 + 4 * (q->last_used % q->n)] / 2;
        q->last_used++;
        if (s >= NSLOT) continue;
        hdr_t* r = fl[s].resp;
        if (r->flags & 1) dset[r->fence & 1023] = r->fence;
        if (fl[s].async) { kfree(fl[s].cmd); fl[s].st = 0; }
        else fl[s].st = 2;
    }
    // rings finish in any order: done_fence = everything below the oldest one still in flight
    uint64_t lo = fence_id + 1;
    for (int i = 0; i < NSLOT; i++) if (fl[i].st == 1 && fl[i].fence && fl[i].fence < lo) lo = fl[i].fence;
    done_fence = lo - 1;
    busy = false;
}

static int post(void* cmd, uint32_t clen, void* resp, uint32_t rlen, bool async) {
    int s = -1;
    uint32_t t0 = pit_uptime_ms();
    for (;;) {
        if (qlock()) {
            for (int i = 0; i < NSLOT && i < cq.n / 2; i++) if (!fl[i].st) { s = i; break; }
            if (s >= 0) break;
            busy = false;
        }
        reap();
        bkl_yield(this_cpu());
        if (pit_uptime_ms() - t0 > 3000) { sp("vgpu: no slot\r\n"); return -1; }
    }
    vmq_t* q = &cq;
    hdr_t* h = cmd;
    if (h->flags & 1) h->fence = ++fence_id;
    fl[s].cmd = cmd; fl[s].fence = h->fence; fl[s].st = 1; fl[s].async = async;
    if (async) { resp = &fl[s].r; rlen = sizeof(hdr_t); }
    fl[s].resp = resp;
    memset(resp, 0, sizeof(hdr_t));
    q->desc[2 * s].addr = V2P(cmd); q->desc[2 * s].len = clen; q->desc[2 * s].flags = 1; q->desc[2 * s].next = 2 * s + 1;
    q->desc[2 * s + 1].addr = V2P(resp); q->desc[2 * s + 1].len = rlen; q->desc[2 * s + 1].flags = 2; q->desc[2 * s + 1].next = 0;
    q->desc[2 * s].addr_hi = q->desc[2 * s + 1].addr_hi = 0;
    uint16_t i = q->avail[1];
    q->avail[2 + i % q->n] = 2 * s;
    __asm__ volatile ("" ::: "memory");
    q->avail[1] = i + 1;
    __asm__ volatile ("" ::: "memory");
    busy = false;
    // the notify write is a vmexit and virgl runs the cmd inside it (ms), don't sit on the bkl for that
    uint64_t f = irq_save();
    struct cpu* c = this_cpu();
    int hb = c->bkl;
    if (hb) bkl_drop(c);
    vmq_kick(q, 0);
    if (hb) bkl_take(c);
    irq_restore(f);
    return s;
}

/* one request, wait for its own answer. returns the response type or 0 on timeout */
static uint32_t do_cmd(void* cmd, uint32_t clen, void* resp, uint32_t rlen) {
    int s = post(cmd, clen, resp, rlen, false);
    if (s < 0) return 0;
    uint32_t spin = 0, t0 = pit_uptime_ms();
    // virgl flush can take 20ms+, let the other cpus have the kernel meanwhile
    while (fl[s].st != 2) {
        reap();
        if (!(spin & 1023)) bkl_yield(this_cpu());
        if (++spin > 40000000 && pit_uptime_ms() - t0 > 3000) break;       // virgl is slow on the first cmds
    }
    if (fl[s].st != 2) { vga_printf("vgpu: timeout\n"); sp("vgpu: timeout\r\n"); return 0; }
    fl[s].st = 0;
    return ((hdr_t*)resp)->type;
}

uint64_t vg_done(void) { reap(); return done_fence; }

bool vg_fence_done(uint64_t f) { reap(); return f <= done_fence || dset[f & 1023] == f; }

/* wait for a fence, bkl is dropped while we spin. false = host is dead */
bool vg_wait(uint64_t f) {
    uint32_t t0 = pit_uptime_ms();
    while (!vg_fence_done(f)) {
        bkl_yield(this_cpu());
        task_yield();
        if (pit_uptime_ms() - t0 > 10000) return false;
    }
    return true;
}

void vg_idle(void) { bkl_yield(this_cpu()); task_yield(); }

static int simple(void* cmd, uint32_t len) {
    hdr_t r;
    return do_cmd(cmd, len, &r, sizeof r) == 0x1100 ? 0 : -1;
}

static void get_info(void) {
    hdr_t h = { .type = 0x100 };
    static struct { hdr_t h; struct { rect_t r; uint32_t on, flags; } p[16]; } r;
    if (do_cmd(&h, sizeof h, &r, sizeof r) != 0x1101) return;
    memcpy(info, r.p, sizeof info);
}

bool vg_init(void) {
    pci_dev_t d;
    if (!pci_find(0x1AF4, 0x1050, &d)) { sp("vg: nopci\r\n"); return false; }
    if (vm_probe(&d, &vm) < 0 || !vm.dev) { sp("vg: probe\r\n"); return false; }
    if (vm_start(&vm, 0x1b, &feat) < 0) { sp("vg: start\r\n"); return false; }          /* virgl and edid, only edid is used */
    virgl = feat & 1;
    venus = (feat & 0x18) == 0x18 && vm.shm;
    if (vm_queue(&vm, &cq, 0) < 0 || vm_queue(&vm, &curq, 1) < 0) return false;
    vm_ready(&vm);
    ok = true;
    get_info();
    vga_printf("    virtio-gpu: %dx%d%s\n", (int)info[0].r.w, (int)info[0].r.h, venus ? " venus" : virgl ? " virgl" : "");
    return true;
}

bool vg_present(void) { return ok; }
bool vg_virgl(void) { return ok && virgl; }
bool vg_venus(void) { return ok && venus; }
uint64_t vg_shm(void) { return vm.shm; }
uint64_t vg_shm_len(void) { return vm.shm_len; }
int vg_pref_w(void) { return info[0].on ? (int)info[0].r.w : 0; }
int vg_pref_h(void) { return info[0].on ? (int)info[0].r.h : 0; }

bool vg_events(void) {
    if (!ok) return false;
    uint32_t e = cfg32(0);
    if (!e) return false;
    *(volatile uint32_t*)(vm.dev + 4) = e;               /* events_clear */
    if (e & 1) get_info();
    return e & 1;
}

int vg_edid(uint8_t* buf) {
    if (!ok || !(feat & 2)) return 0;
    struct { hdr_t h; uint32_t scanout, pad; } c = { .h = { .type = 0x10a } };
    static struct { hdr_t h; uint32_t size, pad; uint8_t edid[1024]; } r;
    if (do_cmd(&c, sizeof c, &r, sizeof r) != 0x1104) return 0;
    int n = r.size > 1024 ? 1024 : (int)r.size;
    memcpy(buf, r.edid, (size_t)n);
    return n;
}

int vg_create(uint32_t res, uint32_t fmt, uint32_t w, uint32_t h) {
    struct { hdr_t hd; uint32_t id, fmt, w, h; } c = { { .type = 0x101 }, res, fmt, w, h };
    return simple(&c, sizeof c);
}

int vg_attach(uint32_t res, const uintptr_t* pages, int n) {
    /* glue neighbours together, a dumb buffer is mostly one big run */
    typedef struct { uint64_t addr; uint32_t len, pad; } ent_t;
    uint32_t sz = sizeof(hdr_t) + 8 + (uint32_t)n * sizeof(ent_t);
    uint8_t* b = kmalloc(sz);
    if (!b) return -1;
    memset(b, 0, sz);
    ent_t* e = (ent_t*)(b + sizeof(hdr_t) + 8);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && e[k - 1].addr + e[k - 1].len == pages[i]) e[k - 1].len += 4096;
        else { e[k].addr = pages[i]; e[k].len = 4096; k++; }
    }
    hdr_t* h = (hdr_t*)b;
    h->type = 0x106;
    ((uint32_t*)(h + 1))[0] = res;
    ((uint32_t*)(h + 1))[1] = (uint32_t)k;
    int r = simple(b, sizeof(hdr_t) + 8 + (uint32_t)k * sizeof(ent_t));
    kfree(b);
    return r;
}

int vg_unref(uint32_t res) {
    struct { hdr_t h; uint32_t id, pad; } c = { { .type = 0x102 }, res, 0 };
    return simple(&c, sizeof c);
}

int vg_scanout(uint32_t res, uint32_t w, uint32_t h) {
    struct { hdr_t h; rect_t r; uint32_t scanout, res; } c = { { .type = 0x103 }, { 0, 0, w, h }, 0, res };
    return simple(&c, sizeof c);
}

int vg_xfer(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch) {
    struct { hdr_t h; rect_t r; uint64_t off; uint32_t res, pad; } c =
        { { .type = 0x105 }, { x, y, w, h }, (uint64_t)y * pitch + x * 4, res, 0 };
    return simple(&c, sizeof c);
}

int vg_flush(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct { hdr_t h; rect_t r; uint32_t res, pad; } c = { { .type = 0x104 }, { x, y, w, h }, res, 0 };
    return simple(&c, sizeof c);
}

/* cursor queue has no answers, the device only marks the buffer used */
static void cur_cmd(uint32_t type, uint32_t res, int x, int y, int hx, int hy) {
    struct { hdr_t h; uint32_t scanout, px, py, pad; uint32_t res, hx, hy, pad2; } c;
    memset(&c, 0, sizeof c);
    c.h.type = type;
    c.px = (uint32_t)x; c.py = (uint32_t)y;
    c.res = res; c.hx = (uint32_t)hx; c.hy = (uint32_t)hy;
    for (;;) {
        uint32_t f = irq_save();
        if (!busy) { busy = true; irq_restore(f); break; }
        irq_restore(f);
        task_yield();
    }
    vmq_t* q = &curq;
    static struct { hdr_t h; uint32_t a[8]; } buf __attribute__((aligned(16)));
    memcpy(&buf, &c, sizeof c);
    q->desc[0].addr = V2P(&buf); q->desc[0].addr_hi = 0; q->desc[0].len = sizeof c; q->desc[0].flags = 0;
    uint16_t i = q->avail[1];
    q->avail[2 + i % q->n] = 0;
    __asm__ volatile ("" ::: "memory");
    q->avail[1] = i + 1;
    vmq_kick(q, 1);
    uint32_t spin = 0;
    while (*(volatile uint16_t*)(q->used + 1) == q->last_used)
        if (++spin > 40000000) break;
    q->last_used = q->used[1];
    busy = false;
}

int vg_cursor(uint32_t res, int x, int y, int hx, int hy) { cur_cmd(0x300, res, x, y, hx, hy); return 0; }
int vg_cursor_move(uint32_t res, int x, int y) { cur_cmd(0x301, res, x, y, 0, 0); return 0; }

/* 3d part: every command carries a fence, the device answers when it's done so
   it all stays synchronous */
static int cmd3(uint32_t type, uint32_t ctx, void* body, uint32_t blen, void* resp, uint32_t rlen, uint32_t want) {
    uint32_t sz = sizeof(hdr_t) + blen;
    uint8_t* b = kmalloc(sz);
    if (!b) return -1;
    memset(b, 0, sizeof(hdr_t));
    if (blen) memcpy(b + sizeof(hdr_t), body, blen);
    hdr_t* h = (hdr_t*)b;
    h->type = type; h->ctx = ctx; h->flags = 1;
    int r = do_cmd(b, sz, resp, rlen) == want ? 0 : -1;
    kfree(b);
    return r;
}

/* fire and forget, returns the fence or -1 */
static int64_t async3(uint32_t type, uint32_t ctx, void* body, uint32_t blen, int ring) {
    uint32_t sz = sizeof(hdr_t) + blen;
    uint8_t* b = kmalloc(sz);
    if (!b) return -1;
    memset(b, 0, sizeof(hdr_t));
    if (blen) memcpy(b + sizeof(hdr_t), body, blen);
    hdr_t* h = (hdr_t*)b;
    h->type = type; h->ctx = ctx; h->flags = 1;
    if (ring >= 0) { h->flags |= 2; h->ring = (uint8_t)ring; }
    int s = post(b, sz, NULL, 0, true);
    if (s < 0) { kfree(b); return -1; }
    reap();
    return (int64_t)fl[s].fence;
}

static int simple3(uint32_t type, uint32_t ctx, void* body, uint32_t blen) {
    hdr_t r;
    return cmd3(type, ctx, body, blen, &r, sizeof r, 0x1100);
}

int vg_ctx_create(uint32_t ctx, uint32_t init) {
    struct { uint32_t nlen, init; char name[64]; } c = { 5, init, "samar" };
    return simple3(0x200, ctx, &c, sizeof c);
}
int vg_ctx_destroy(uint32_t ctx) { return simple3(0x201, ctx, NULL, 0); }
int vg_ctx_attach(uint32_t ctx, uint32_t res) { uint32_t c[2] = { res, 0 }; return simple3(0x202, ctx, c, 8); }

int vg_create3d(uint32_t res, const uint32_t* p) {   /* p: target fmt bind w h depth array last ns flags */
    uint32_t c[12] = { res };
    memcpy(c + 1, p, 40);
    return simple3(0x204, 0, c, sizeof c);
}

int64_t vg_xfer3d(uint32_t ctx, uint32_t res, bool to_host, const uint32_t* box, uint64_t off, uint32_t level, uint32_t stride, uint32_t lstride) {
    struct { uint32_t box[6]; uint64_t off; uint32_t res, level, stride, lstride; } c;
    memcpy(c.box, box, 24);
    c.off = off; c.res = res; c.level = level; c.stride = stride; c.lstride = lstride;
    return async3(to_host ? 0x205 : 0x206, ctx, &c, sizeof c, -1);
}

/* scanout flush that doesn't wait: the host may sit on it till its vsync */
int64_t vg_flush_async(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct { rect_t r; uint32_t res, pad; } c = { { x, y, w, h }, res, 0 };
    return async3(0x104, 0, &c, sizeof c, -1);
}

int64_t vg_submit(uint32_t ctx, const void* buf, uint32_t size, int ring) {
    uint32_t n = (size + 3) & ~3u;
    uint8_t* b = kmalloc(8 + n);
    if (!b) return -1;
    memset(b, 0, 8 + n);
    ((uint32_t*)b)[0] = size;
    memcpy(b + 8, buf, size);
    int64_t r = async3(0x207, ctx, b, 8 + n, ring);
    kfree(b);
    return r;
}

/* idx-th capset: fills id/ver/size */
int vg_capset_info(uint32_t idx, uint32_t* id, uint32_t* ver, uint32_t* size) {
    uint32_t c[2] = { idx, 0 };
    struct { hdr_t h; uint32_t id, ver, size, pad; } r;
    if (cmd3(0x108, 0, c, 8, &r, sizeof r, 0x1102) < 0) return -1;
    *id = r.id; *ver = r.ver; *size = r.size;
    return 0;
}

int vg_capset(uint32_t id, uint32_t ver, uint8_t* out, uint32_t max) {
    uint32_t c[2] = { id, ver };
    uint32_t rl = sizeof(hdr_t) + max;
    uint8_t* r = kmalloc(rl);
    if (!r) return -1;
    int ret = cmd3(0x109, 0, c, 8, r, rl, 0x1103);
    if (!ret) memcpy(out, r + sizeof(hdr_t), max);
    kfree(r);
    return ret;
}

/* blob resources: mem 1 = guest pages, 2 = host only, 3 = both. host ones get a window in the shm bar */
int vg_blob(uint32_t ctx, uint32_t res, uint32_t mem, uint32_t flags, uint64_t id, uint64_t size, const uintptr_t* pages, int n) {
    typedef struct { uint64_t addr; uint32_t len, pad; } ent_t;
    uint32_t sz = sizeof(hdr_t) + 32 + (uint32_t)n * sizeof(ent_t);
    uint8_t* b = kmalloc(sz);
    if (!b) return -1;
    memset(b, 0, sz);
    uint32_t* c = (uint32_t*)(b + sizeof(hdr_t));
    c[0] = res; c[1] = mem; c[2] = flags;
    *(uint64_t*)(c + 4) = id;
    *(uint64_t*)(c + 6) = size;
    ent_t* e = (ent_t*)(c + 8);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && e[k - 1].addr + e[k - 1].len == pages[i]) e[k - 1].len += 4096;
        else { e[k].addr = pages[i]; e[k].len = 4096; k++; }
    }
    c[3] = (uint32_t)k;
    hdr_t* h = (hdr_t*)b;
    h->type = 0x10c; h->ctx = ctx; h->flags = 1;
    hdr_t r;
    int ret = do_cmd(b, sizeof(hdr_t) + 32 + (uint32_t)k * sizeof(ent_t), &r, sizeof r) == 0x1100 ? 0 : -1;
    kfree(b);
    return ret;
}

int vg_blob_map(uint32_t res, uint64_t off) {
    uint32_t c[4] = { res, 0, (uint32_t)off, (uint32_t)(off >> 32) };
    struct { hdr_t h; uint32_t info, pad; } r;
    return cmd3(0x208, 0, c, 16, &r, sizeof r, 0x1106);
}

int vg_blob_unmap(uint32_t res) { uint32_t c[2] = { res, 0 }; return simple3(0x209, 0, c, 8); }

int vg_ncaps(void) { return virgl ? (int)cfg32(12) : 0; }
