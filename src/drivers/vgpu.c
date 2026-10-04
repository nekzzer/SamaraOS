#include "drivers/vgpu.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "drivers/vga.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"

#include "core/io.h"
static void sp(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }
typedef struct { uint32_t type, flags; uint64_t fence; uint32_t ctx; uint8_t ring, pad[3]; } hdr_t;
typedef struct { uint32_t x, y, w, h; } rect_t;

static vm_t vm;
static vmq_t cq, curq;
static bool ok;
static volatile bool busy;
static uint32_t feat;
static struct { rect_t r; uint32_t on, flags; } info[16];
static bool changed;

static uint32_t cfg32(int o) { return *(volatile uint32_t*)(vm.dev + o); }

/* one request, one answer, queue 0. returns the response type or 0 on timeout */
static uint32_t do_cmd(void* cmd, uint32_t clen, void* resp, uint32_t rlen) {
    for (;;) {
        uint32_t f = irq_save();
        if (!busy) { busy = true; irq_restore(f); break; }
        irq_restore(f);
        task_yield();
    }
    vmq_t* q = &cq;
    q->desc[0].addr = V2P(cmd); q->desc[0].len = clen; q->desc[0].flags = 1; q->desc[0].next = 1;
    q->desc[1].addr = V2P(resp); q->desc[1].len = rlen; q->desc[1].flags = 2; q->desc[1].next = 0;
    q->desc[0].addr_hi = q->desc[1].addr_hi = 0;
    memset(resp, 0, sizeof(hdr_t));
    uint16_t i = q->avail[1];
    q->avail[2 + i % q->n] = 0;
    __asm__ volatile ("" ::: "memory");
    q->avail[1] = i + 1;
    __asm__ volatile ("" ::: "memory");
    vmq_kick(q, 0);
    uint32_t spin = 0;
    while (*(volatile uint16_t*)(q->used + 1) == q->last_used)
        if (++spin > 40000000) break;
    uint32_t t = 0;
    if (q->used[1] != q->last_used) { q->last_used++; t = ((hdr_t*)resp)->type; }
    else vga_printf("vgpu: timeout\n");
    busy = false;
    return t;
}

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
    if (vm_start(&vm, 3, &feat) < 0) { sp("vg: start\r\n"); return false; }          /* virgl and edid, only edid is used */
    feat &= 2;
    if (vm_queue(&vm, &cq, 0) < 0 || vm_queue(&vm, &curq, 1) < 0) return false;
    vm_ready(&vm);
    ok = true;
    get_info();
    vga_printf("    virtio-gpu: %dx%d\n", (int)info[0].r.w, (int)info[0].r.h);
    return true;
}

bool vg_present(void) { return ok; }
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
