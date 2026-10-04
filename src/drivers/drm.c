#include "drivers/drm.h"
#include "drivers/vgpu.h"
#include "drivers/fbdev.h"
#include "drivers/pci.h"
#include "drivers/vga.h"
#include "gfx/gfx.h"
#include "fs/fs.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"
#include "core/io.h"
#include "boot/pit.h"

int snprintf(char* buf, size_t n, const char* fmt, ...);
bool user_ok(const void* p, uint32_t len);       /* syscall.c */

#define ENOENT 2
#define ENXIO 6
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EINVAL 22
#define ENOTTY 25
#define EOPNOTSUPP 95

#define PTR(v) ((void*)(uintptr_t)(v))
#define U(p, n) do { if (!user_ok((p), (n))) return -EFAULT; } while (0)

/* object ids, the numbers mean nothing */
#define ID_PLANE  40
#define ID_CURSOR 41
#define ID_CRTC   50
#define ID_ENC    60
#define ID_CONN   70
#define FB_FIRST  100
#define BLOB_EDID 200
#define BLOB_MODE 201
#define BLOB_USER 300

#define OBJ_CRTC 0xcccccccc
#define OBJ_CONN 0xc0c0c0c0
#define OBJ_ENC  0xe0e0e0e0
#define OBJ_FB   0xfbfbfbfb
#define OBJ_PLANE 0xeeeeeeee

#define MAXGEM 64
#define GEM_SLOT 0x4000000u          /* mmap offset of handle n = n * 64M */
#define FOURCC_XR24 0x34325258
#define FOURCC_AR24 0x34325241

typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
} dmode_t;

typedef struct { uint32_t type, len; uint64_t ud; uint32_t sec, usec, seq, crtc; } ev_t;

typedef struct drm_fd {
    bool master, render, univ, atomic;
    ev_t ev[64];
    int head, tail;
} dfd_t;

typedef struct {
    dfd_t* owner;
    uint32_t w, h, pitch, size;
    int np;
    uintptr_t* pg;
    uint32_t res, res_a;             /* virtio resource ids, res_a = argb twin for the cursor */
    uint32_t aw, ah;
} gem_t;

typedef struct { dfd_t* owner; uint32_t id, gem, w, h, pitch, fmt; } fb_t;

static int be;                        /* 0 none, 1 virtio-gpu, 2 bochs */
static pci_dev_t pdev;
static gem_t gems[MAXGEM];
static fb_t fbs[32];
static uint32_t next_fb = FB_FIRST;
static dfd_t* master;
static dmode_t modes[24];
static int nmodes;
static uint8_t edid[1024];
static int edid_len;

static struct {
    bool on;
    dmode_t mode;
    uint32_t fb;
    int cx, cy, chx, chy;            /* cursor pos and hotspot */
    uint32_t cur_gem;
} crtc;

static struct {
    bool pending;
    dfd_t* fd;
    uint64_t ud;
    uint32_t due;
} flip;
static struct { dfd_t* fd; uint64_t ud; uint32_t due; } vbl[8];

static bool grabbed, grab_fb;
static uint32_t last_flip_ms, last_push_ms;

/* console on virtio: ram framebuffer behind resource 1 */
static uint8_t *con_fb, *con_raw;
static int con_w, con_h;

/* bochs */
static volatile uint8_t* lfb;
static uint32_t vram;

static struct { uint32_t id, len; uint8_t* data; } blobs[8];

static volatile int big;
static void lock(void) {
    for (;;) {
        uint32_t f = irq_save();
        if (!big) { big = 1; irq_restore(f); return; }
        irq_restore(f);
        task_yield();
    }
}
static void unlock(void) { big = 0; }

static void com_s(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

static void bga_w(uint16_t i, uint16_t v) { outw(0x1CE, i); outw(0x1CF, v); }
static uint16_t bga_r(uint16_t i) { outw(0x1CE, i); return inw(0x1CF); }

static void bga_mode(int w, int h) {
    bga_w(4, 0);
    bga_w(1, (uint16_t)w);
    bga_w(2, (uint16_t)h);
    bga_w(3, 32);
    bga_w(6, (uint16_t)w);
    bga_w(8, 0); bga_w(9, 0);
    bga_w(4, 0x41);
}

static gem_t* gem_get(dfd_t* d, uint32_t h) {
    if (h == 0 || h >= MAXGEM || !gems[h].pg || gems[h].owner != d) return NULL;
    return &gems[h];
}

static void gem_free(uint32_t h) {
    gem_t* g = &gems[h];
    if (be == 1) {
        if (g->res) vg_unref(g->res);
        if (g->res_a) vg_unref(g->res_a);
    }
    for (int i = 0; i < g->np; i++) pmm_unref(g->pg[i]);
    kfree(g->pg);
    memset(g, 0, sizeof *g);
}

static void gem_read(gem_t* g, uint32_t off, uint8_t* dst, uint32_t len) {
    while (len) {
        uint32_t in = off & 4095, k = 4096 - in;
        if (k > len) k = len;
        memcpy(dst, (uint8_t*)P2V(g->pg[off >> 12]) + in, k);
        dst += k; off += k; len -= k;
    }
}

static int fb_of(uint32_t id) {
    if (!id) return -1;
    for (int i = 0; i < 32; i++) if (fbs[i].id == id) return i;
    return -1;
}

static gem_t* gem_of_fb(fb_t* f) { return f->gem < MAXGEM && gems[f->gem].pg ? &gems[f->gem] : NULL; }

static void push(gem_t* g, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t mw = crtc.mode.hdisplay, mh = crtc.mode.vdisplay;
    if (x >= mw || y >= mh) return;
    if (x + w > mw) w = mw - x;
    if (y + h > mh) h = mh - y;
    if (be == 1) {
        vg_xfer(g->res, x, y, w, h, g->pitch);
        vg_flush(g->res, x, y, w, h);
    } else if (be == 2) {
        for (uint32_t r = y; r < y + h; r++)
            gem_read(g, r * g->pitch + x * 4, (uint8_t*)lfb + (r * mw + x) * 4, w * 4);
    }
    last_push_ms = pit_uptime_ms();
}

static void grab(void) {
    if (grabbed) return;
    grabbed = true;
    grab_fb = fbdev_open();
}

/* the console gets its screen back */
static void release(void) {
    if (!grabbed) return;
    grabbed = false;
    crtc.on = false;
    flip.pending = false;
    if (be == 1) {
        vg_cursor(0, 0, 0, 0, 0);
        crtc.cur_gem = 0;
        if (con_fb) { vg_scanout(1, (uint32_t)con_w, (uint32_t)con_h); vg_flush(1, 0, 0, (uint32_t)con_w, (uint32_t)con_h); }
        else vg_scanout(0, 0, 0);
    } else if (be == 2) {
        if (gfx_ready()) gfx_remode();
        else bga_w(4, 0);
    }
    if (grab_fb) fbdev_close();
    grab_fb = false;
}

static int set_mode(dmode_t* m, uint32_t fbid) {
    int i = fb_of(fbid);
    if (i < 0) return -ENOENT;
    gem_t* g = gem_of_fb(&fbs[i]);
    if (!g) return -ENOENT;
    if (fbs[i].w < m->hdisplay || fbs[i].h < m->vdisplay) return -EINVAL;
    if (be == 1) {
        if (vg_scanout(g->res, m->hdisplay, m->vdisplay) < 0) return -EINVAL;
    } else if (be == 2) {
        if ((uint32_t)m->hdisplay * m->vdisplay * 4 > vram) return -EINVAL;
        bga_mode(m->hdisplay, m->vdisplay);
    }
    grab();
    crtc.on = true;
    crtc.mode = *m;
    crtc.fb = fbid;
    push(g, 0, 0, m->hdisplay, m->vdisplay);
    return 0;
}

static void crtc_off(void) {
    if (!crtc.on) return;
    crtc.on = false;
    crtc.fb = 0;
    if (be == 1) vg_scanout(0, 0, 0);
    else if (be == 2) bga_w(4, 0);
}

static void mk_mode(dmode_t* m, int w, int h, bool pref) {
    memset(m, 0, sizeof *m);
    m->hdisplay = (uint16_t)w; m->hsync_start = (uint16_t)(w + 48); m->hsync_end = (uint16_t)(w + 80); m->htotal = (uint16_t)(w + 160);
    m->vdisplay = (uint16_t)h; m->vsync_start = (uint16_t)(h + 3); m->vsync_end = (uint16_t)(h + 8); m->vtotal = (uint16_t)(h + 22);
    m->clock = (uint32_t)m->htotal * m->vtotal / 1000 * 60;
    m->vrefresh = 60;
    m->flags = 0x5;                    /* +hsync +vsync */
    m->type = pref ? 0x48 : 0x40;      /* driver, preferred */
    snprintf(m->name, 32, "%dx%d", w, h);
}

static void build_modes(void) {
    static const short std[][2] = { {640,480}, {800,600}, {1024,768}, {1152,864}, {1280,720}, {1280,800},
        {1280,1024}, {1366,768}, {1440,900}, {1600,900}, {1680,1050}, {1920,1080}, {1920,1200}, {2560,1440} };
    int pw = 0, ph = 0;
    if (be == 1) { pw = vg_pref_w(); ph = vg_pref_h(); }
    else if (gfx_ready()) { pw = gfx_w(); ph = gfx_h(); }
    if (pw < 64 || ph < 64) { pw = 1024; ph = 768; }
    nmodes = 0;
    mk_mode(&modes[nmodes++], pw, ph, true);
    for (unsigned i = 0; i < sizeof std / sizeof std[0] && nmodes < 24; i++) {
        int w = std[i][0], h = std[i][1];
        if (w == pw && h == ph) continue;
        if (be == 2 && (uint32_t)w * h * 4 > vram) continue;
        mk_mode(&modes[nmodes++], w, h, false);
    }
}

static uint32_t vseq(void) {
    uint32_t ms = pit_uptime_ms();
    return ms / 1000 * 60 + ms % 1000 * 60 / 1000;
}

static void post(dfd_t* d, uint32_t type, uint64_t ud, uint32_t seq) {
    if (!d) return;
    int n = (d->head + 1) & 63;
    if (n == d->tail) return;
    uint32_t ms = pit_uptime_ms();
    ev_t* e = &d->ev[d->head];
    e->type = type; e->len = sizeof(ev_t); e->ud = ud;
    e->sec = ms / 1000; e->usec = ms % 1000 * 1000; e->seq = seq; e->crtc = ID_CRTC;
    d->head = n;
}

static void tick(void) {
    uint32_t now = vseq();
    if (flip.pending && (int32_t)(now - flip.due) >= 0) {
        post(flip.fd, 2, flip.ud, now);
        flip.pending = false;
    }
    for (int i = 0; i < 8; i++)
        if (vbl[i].fd && (int32_t)(now - vbl[i].due) >= 0) { post(vbl[i].fd, 1, vbl[i].ud, now); vbl[i].fd = NULL; }
    if (be == 1 && vg_events()) {
        build_modes();
        com_s("drm: host display changed\r\n");
    }
    if (grabbed) {
        if (grab_fb && !fbdev_active()) { release(); return; }       // hotkey took the screen back
        uint32_t ms = pit_uptime_ms();
        if (crtc.on && ms - last_push_ms > 40 && ms - last_flip_ms > 40) {
            int i = fb_of(crtc.fb);
            gem_t* g = i >= 0 ? gem_of_fb(&fbs[i]) : NULL;
            if (g) push(g, 0, 0, crtc.mode.hdisplay, crtc.mode.vdisplay);
        }
    } else if (be == 1 && con_fb && pit_uptime_ms() - last_push_ms > 30) {
        // no dirty tracking in gfx, just push all of it. fine for qemu
        vg_xfer(1, 0, 0, (uint32_t)con_w, (uint32_t)con_h, (uint32_t)con_w * 4);
        vg_flush(1, 0, 0, (uint32_t)con_w, (uint32_t)con_h);
        last_push_ms = pit_uptime_ms();
    }
}

static void drm_task(void) {
    for (;;) {
        task_sleep_ms(16);
        uint32_t f = irq_save();
        bool got = !big;
        if (got) big = 1;
        irq_restore(f);
        if (!got) continue;
        tick();
        unlock();
    }
}

uint8_t* drm_console_fb(int w, int h) {
    if (be != 1) return NULL;
    lock();
    if (con_fb && con_w == w && con_h == h) { unlock(); return con_fb; }
    uint32_t sz = (uint32_t)w * (uint32_t)h * 4;
    uint8_t* raw = kmalloc_big(sz + 4096);
    if (!raw) { unlock(); return NULL; }
    uint8_t* fb = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(fb, 0, sz);
    int np = (int)((sz + 4095) / 4096);
    uintptr_t* pg = kmalloc((size_t)np * sizeof(uintptr_t));
    if (!pg) { kfree(raw); unlock(); return NULL; }
    for (int i = 0; i < np; i++) pg[i] = V2P(fb + i * 4096);
    if (con_fb) vg_unref(1);
    vg_create(1, VG_FMT_XRGB, (uint32_t)w, (uint32_t)h);
    vg_attach(1, pg, np);
    kfree(pg);
    if (!grabbed) { vg_scanout(1, (uint32_t)w, (uint32_t)h); vg_flush(1, 0, 0, (uint32_t)w, (uint32_t)h); }
    // old buffer is kept, gfx may still hold the pointer for a moment
    con_raw = raw; con_fb = fb; con_w = w; con_h = h;
    unlock();
    return fb;
}

void drm_console_flush(int x, int y, int w, int h) {
    if (be != 1 || !con_fb || grabbed) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > con_w) w = con_w - x;
    if (y + h > con_h) h = con_h - y;
    if (w <= 0 || h <= 0) return;
    vg_xfer(1, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, (uint32_t)con_w * 4);
    vg_flush(1, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

bool drm_pref_size(int* w, int* h) {
    if (be != 1 || vg_pref_w() < 64) return false;
    *w = vg_pref_w(); *h = vg_pref_h();
    return true;
}

bool drm_virtio(void) { return be == 1; }
bool drm_active(void) { return grabbed && crtc.on; }
bool drm_ready(void) { return be != 0; }

dfd_t* drm_open(bool render) {
    if (!be) return NULL;
    dfd_t* d = kmalloc(sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    d->render = render;
    lock();
    if (!render && !master) { master = d; d->master = true; }
    unlock();
    return d;
}

void drm_close(dfd_t* d) {
    lock();
    for (int i = 0; i < 32; i++) if (fbs[i].owner == d) {
        if (fbs[i].id == crtc.fb && grabbed) crtc_off();
        memset(&fbs[i], 0, sizeof fbs[i]);
    }
    for (uint32_t h = 1; h < MAXGEM; h++) if (gems[h].owner == d) {
        if (crtc.cur_gem == h) { crtc.cur_gem = 0; if (be == 1) vg_cursor(0, 0, 0, 0, 0); }
        gem_free(h);
    }
    if (flip.fd == d) flip.pending = false;
    for (int i = 0; i < 8; i++) if (vbl[i].fd == d) vbl[i].fd = NULL;
    if (master == d) { master = NULL; release(); }
    unlock();
    kfree(d);
}

bool drm_readable(dfd_t* d) { return d->head != d->tail; }

int drm_read(dfd_t* d, char* buf, uint32_t n) {
    int got = 0;
    while (d->head != d->tail && n >= sizeof(ev_t)) {
        memcpy(buf + got, &d->ev[d->tail], sizeof(ev_t));
        d->tail = (d->tail + 1) & 63;
        got += sizeof(ev_t); n -= sizeof(ev_t);
    }
    return got;
}

int drm_mmap(dfd_t* d, uint64_t pd, uint64_t va, uint64_t len, uint64_t off, bool rw) {
    uint32_t h = off / GEM_SLOT, in = off % GEM_SLOT;
    gem_t* g = gem_get(d, h);
    if (!g || in >= g->size) return -EINVAL;
    for (uint32_t k = 0; k < len / 4096 && in / 4096 + k < (uint32_t)g->np; k++)
        vmm_map_frame(pd, va + k * 4096, g->pg[in / 4096 + k], rw);
    return 0;
}

typedef struct { int maj, min, patch; size_t nlen; char* name; size_t dlen; char* date; size_t desc_len; char* desc; } s_version;
typedef struct { uint64_t cap, val; } s_cap;
typedef struct { uint64_t fb, crtc, conn, enc; uint32_t cfb, ccrtc, cconn, cenc, minw, maxw, minh, maxh; } s_res;
typedef struct { uint64_t conns; uint32_t nconn, crtc_id, fb_id, x, y, gamma, mode_valid; dmode_t mode; } s_crtc;
typedef struct { uint32_t id, type, crtc, poss_crtcs, poss_clones; } s_enc;
typedef struct {
    uint64_t encs, modes, props, vals;
    uint32_t nmodes, nprops, nencs, enc_id, id, type, type_id, conn, mm_w, mm_h, subpix, pad;
} s_conn;
typedef struct { uint32_t flags, crtc, x, y, w, h, handle; int32_t hx, hy; } s_cursor;
typedef struct { uint32_t h, w, bpp, flags, handle, pitch; uint64_t size; } s_dumb;
typedef struct { uint32_t handle, pad; uint64_t off; } s_mapdumb;
typedef struct { uint32_t id, w, h, pitch, bpp, depth, handle; } s_fbcmd;
typedef struct { uint32_t id, w, h, fmt, flags, handles[4], pitches[4], offsets[4], mod[8]; } s_fbcmd2;
typedef struct { uint32_t crtc, fb, flags, res; uint64_t ud; } s_flip;
typedef struct { uint32_t fb, flags, color, nclips; uint64_t clips; } s_dirty;
typedef struct { uint64_t ids; uint32_t n; } s_planeres;
typedef struct { uint32_t id, crtc, fb, poss, gamma, nfmt; uint64_t fmts; } s_plane;
typedef struct { uint64_t props, vals; uint32_t n, id, type; } s_objprops;
typedef struct { uint64_t val; uint32_t prop, id, type; } s_objset;
typedef struct { uint64_t vals, enums; uint32_t id, flags; char name[32]; uint32_t nvals, nenums; } s_prop;
typedef struct { uint64_t val; char name[32]; } s_enum;
typedef struct { uint32_t id, len; uint64_t data; } s_blob;
typedef struct { uint32_t flags, nobjs; uint64_t objs, nprops, props, vals, res, ud; } s_atomic;
typedef struct { uint32_t type, seq; long sec, usec; } s_vblank;

#define P_TYPE 1
#define P_FB_ID 2
#define P_CRTC_ID 3
#define P_SRC_X 4
#define P_CRTC_X 8
#define P_DPMS 13
#define P_EDID 14
#define P_ACTIVE 15
#define P_MODE_ID 16

static const struct { uint32_t id; const char* name; uint32_t flags; } pdefs[] = {
    { 1, "type", 0xC }, { 2, "FB_ID", 0x40 }, { 3, "CRTC_ID", 0x40 },
    { 4, "SRC_X", 2 }, { 5, "SRC_Y", 2 }, { 6, "SRC_W", 2 }, { 7, "SRC_H", 2 },
    { 8, "CRTC_X", 0x80 }, { 9, "CRTC_Y", 0x80 }, { 10, "CRTC_W", 2 }, { 11, "CRTC_H", 2 },
    { 13, "DPMS", 8 }, { 14, "EDID", 0x14 }, { 15, "ACTIVE", 2 }, { 16, "MODE_ID", 0x10 },
};

/* value of a property on an object, 0 when the object doesn't have it */
static uint64_t pval(uint32_t obj, uint32_t p) {
    if (obj == ID_PLANE || obj == ID_CURSOR) {
        bool pri = obj == ID_PLANE;
        switch (p) {
            case P_TYPE: return pri ? 1 : 2;
            case P_FB_ID: return pri ? (crtc.on ? crtc.fb : 0) : crtc.cur_gem;
            case P_CRTC_ID: return (pri ? crtc.on : crtc.cur_gem != 0) ? ID_CRTC : 0;
            case P_SRC_X + 2: return pri ? (uint64_t)crtc.mode.hdisplay << 16 : 64 << 16;
            case P_SRC_X + 3: return pri ? (uint64_t)crtc.mode.vdisplay << 16 : 64 << 16;
            case P_CRTC_X: return pri ? 0 : (uint64_t)(int64_t)crtc.cx;
            case P_CRTC_X + 1: return pri ? 0 : (uint64_t)(int64_t)crtc.cy;
            case P_CRTC_X + 2: return pri ? crtc.mode.hdisplay : 64;
            case P_CRTC_X + 3: return pri ? crtc.mode.vdisplay : 64;
        }
    } else if (obj == ID_CRTC) {
        if (p == P_ACTIVE) return crtc.on;
        if (p == P_MODE_ID) return crtc.on ? BLOB_MODE : 0;
    } else if (obj == ID_CONN) {
        if (p == P_EDID) return edid_len ? BLOB_EDID : 0;
        if (p == P_CRTC_ID) return crtc.on ? ID_CRTC : 0;
    }
    return 0;
}

static int obj_props(uint32_t obj, uint32_t* ids) {
    int n = 0;
    if (obj == ID_PLANE || obj == ID_CURSOR) for (int p = 1; p <= 11; p++) ids[n++] = (uint32_t)p;
    else if (obj == ID_CRTC) { ids[n++] = P_ACTIVE; ids[n++] = P_MODE_ID; }
    else if (obj == ID_CONN) { ids[n++] = P_DPMS; if (edid_len) ids[n++] = P_EDID; ids[n++] = P_CRTC_ID; }
    return n;
}

static int need_master(dfd_t* d) { return d->master ? 0 : -EACCES; }

static int put_u32s(uint64_t ptr, const uint32_t* v, int n, uint32_t have) {
    if (!ptr || have < (uint32_t)n || !n) return 0;
    U(PTR(ptr), (uint32_t)n * 4);
    memcpy(PTR(ptr), v, (size_t)n * 4);
    return 0;
}

static int io_version(s_version* v) {
    const char* nm = be == 1 ? "virtio_gpu" : "bochs-drm";
    const char* ds = be == 1 ? "virtio GPU" : "bochs dispi vga interface (qemu stdvga)";
    v->maj = be == 1 ? 0 : 1; v->min = be == 1 ? 1 : 0; v->patch = 0;
    size_t l = strlen(nm), sl = strlen(ds);
    if (v->name && v->nlen) { U(v->name, v->nlen); memcpy(v->name, nm, l < v->nlen ? l : v->nlen); }
    if (v->date && v->dlen) { U(v->date, v->dlen); v->date[0] = '0'; }
    if (v->desc && v->desc_len) { U(v->desc, v->desc_len); memcpy(v->desc, ds, sl < v->desc_len ? sl : v->desc_len); }
    v->nlen = l; v->dlen = 1; v->desc_len = sl;
    return 0;
}

static int io_cap(s_cap* c) {
    switch (c->cap) {
        case 1: c->val = 1; break;                 /* dumb buffer */
        case 2: c->val = 1; break;                 /* vblank high crtc */
        case 3: c->val = 24; break;
        case 4: c->val = 0; break;
        case 5: c->val = 0; break;                 /* prime */
        case 6: c->val = 1; break;                 /* monotonic timestamps */
        case 7: c->val = 0; break;
        case 8: case 9: c->val = be == 1 ? 64 : 0; break;
        case 0x10: c->val = 0; break;
        case 0x11: c->val = 0; break;
        case 0x12: c->val = 1; break;
        case 0x13: c->val = 0; break;
        default: return -EINVAL;
    }
    return 0;
}

static int io_getres(dfd_t* d, s_res* r) {
    uint32_t ids[32];
    int n = 0;
    for (int i = 0; i < 32; i++) if (fbs[i].owner == d) ids[n++] = fbs[i].id;
    if (put_u32s(r->fb, ids, n, r->cfb) < 0) return -EFAULT;
    r->cfb = (uint32_t)n;
    uint32_t one = ID_CRTC;
    if (put_u32s(r->crtc, &one, 1, r->ccrtc) < 0) return -EFAULT;
    r->ccrtc = 1;
    one = ID_CONN;
    if (put_u32s(r->conn, &one, 1, r->cconn) < 0) return -EFAULT;
    r->cconn = 1;
    one = ID_ENC;
    if (put_u32s(r->enc, &one, 1, r->cenc) < 0) return -EFAULT;
    r->cenc = 1;
    r->minw = 0; r->minh = 0; r->maxw = 4096; r->maxh = 4096;
    return 0;
}

static int io_getcrtc(s_crtc* c) {
    if (c->crtc_id != ID_CRTC) return -ENOENT;
    c->fb_id = crtc.on ? crtc.fb : 0;
    c->x = c->y = 0;
    c->gamma = 0;
    c->mode_valid = crtc.on;
    if (crtc.on) c->mode = crtc.mode;
    else memset(&c->mode, 0, sizeof c->mode);
    return 0;
}

static int io_setcrtc(dfd_t* d, s_crtc* c) {
    int e = need_master(d);
    if (e) return e;
    if (c->crtc_id != ID_CRTC) return -ENOENT;
    if (!c->mode_valid || !c->fb_id) { crtc_off(); return 0; }
    if (c->x || c->y) return -EINVAL;
    if (c->mode.hdisplay < 64 || c->mode.vdisplay < 64 || c->mode.hdisplay > 4096 || c->mode.vdisplay > 4096) return -EINVAL;
    return set_mode(&c->mode, c->fb_id);
}

static int io_getenc(s_enc* e) {
    if (e->id != ID_ENC) return -ENOENT;
    e->type = be == 1 ? 6 : 1;                    /* VIRTUAL / DAC */
    e->crtc = crtc.on ? ID_CRTC : 0;
    e->poss_crtcs = 1;
    e->poss_clones = 0;
    return 0;
}

static int io_getconn(s_conn* c) {
    if (c->id != ID_CONN) return -ENOENT;
    uint32_t ids[4], n = (uint32_t)obj_props(ID_CONN, ids);
    if (c->modes && c->nmodes >= (uint32_t)nmodes) {
        U(PTR(c->modes), (uint32_t)nmodes * sizeof(dmode_t));
        memcpy(PTR(c->modes), modes, (size_t)nmodes * sizeof(dmode_t));
    }
    if (c->props && c->vals && c->nprops >= n) {
        U(PTR(c->props), n * 4); U(PTR(c->vals), n * 8);
        for (uint32_t i = 0; i < n; i++) { ((uint32_t*)PTR(c->props))[i] = ids[i]; ((uint64_t*)PTR(c->vals))[i] = pval(ID_CONN, ids[i]); }
    }
    if (c->encs && c->nencs >= 1) { U(PTR(c->encs), 4); *(uint32_t*)PTR(c->encs) = ID_ENC; }
    c->nmodes = (uint32_t)nmodes; c->nprops = n; c->nencs = 1;
    c->enc_id = ID_ENC;
    c->type = be == 1 ? 15 : 1;                   /* VIRTUAL / VGA */
    c->type_id = 1;
    c->conn = 1;
    c->mm_w = c->mm_h = 0;
    c->subpix = 1;
    return 0;
}

static int io_getprop(s_prop* p) {
    int k = -1;
    for (unsigned i = 0; i < sizeof pdefs / sizeof pdefs[0]; i++) if (pdefs[i].id == p->id) k = (int)i;
    if (k < 0) return -ENOENT;
    strncpy(p->name, pdefs[k].name, 31);
    p->name[31] = 0;
    p->flags = pdefs[k].flags;
    uint32_t id = p->id;
    int nv = 0, ne = 0;
    uint64_t v[2] = { 0, 0 };
    s_enum en[4];
    memset(en, 0, sizeof en);
    if (id == P_TYPE) {
        ne = 3; strcpy(en[0].name, "Overlay");
        en[1].val = 1; strcpy(en[1].name, "Primary");
        en[2].val = 2; strcpy(en[2].name, "Cursor");
    } else if (id == P_DPMS) {
        ne = 4; strcpy(en[0].name, "On");
        en[1].val = 1; strcpy(en[1].name, "Standby");
        en[2].val = 2; strcpy(en[2].name, "Suspend");
        en[3].val = 3; strcpy(en[3].name, "Off");
    } else if (id == P_FB_ID) { nv = 1; v[0] = OBJ_FB; }
    else if (id == P_CRTC_ID) { nv = 1; v[0] = OBJ_CRTC; }
    else if (id >= 4 && id <= 7) { nv = 2; v[1] = 0xffffffffu; }
    else if (id == 8 || id == 9) { nv = 2; v[0] = (uint64_t)(int64_t)-0x7fffffff; v[1] = 0x7fffffff; }
    else if (id == 10 || id == 11) { nv = 2; v[1] = 0x7fffffff; }
    else if (id == P_ACTIVE) { nv = 2; v[1] = 1; }
    if (p->vals && p->nvals >= (uint32_t)nv && nv) { U(PTR(p->vals), (uint32_t)nv * 8); memcpy(PTR(p->vals), v, (size_t)nv * 8); }
    if (p->enums && p->nenums >= (uint32_t)ne && ne) { U(PTR(p->enums), (uint32_t)ne * sizeof(s_enum)); memcpy(PTR(p->enums), en, (size_t)ne * sizeof(s_enum)); }
    p->nvals = (uint32_t)nv;
    p->nenums = (uint32_t)ne;
    return 0;
}

static int io_objprops(s_objprops* o) {
    uint32_t ids[16];
    int n = 0;
    if ((o->type == OBJ_PLANE && (o->id == ID_PLANE || o->id == ID_CURSOR)) || (o->type == OBJ_CRTC && o->id == ID_CRTC) ||
        (o->type == OBJ_CONN && o->id == ID_CONN)) n = obj_props(o->id, ids);
    else if (!(o->type == OBJ_ENC && o->id == ID_ENC)) return -ENOENT;
    if (o->props && o->vals && o->n >= (uint32_t)n && n) {
        U(PTR(o->props), (uint32_t)n * 4); U(PTR(o->vals), (uint32_t)n * 8);
        for (int i = 0; i < n; i++) { ((uint32_t*)PTR(o->props))[i] = ids[i]; ((uint64_t*)PTR(o->vals))[i] = pval(o->id, ids[i]); }
    }
    o->n = (uint32_t)n;
    return 0;
}

static int io_getplane(s_plane* p) {
    if (p->id != ID_PLANE && p->id != ID_CURSOR) return -ENOENT;
    bool pri = p->id == ID_PLANE;
    p->crtc = (pri ? crtc.on : crtc.cur_gem != 0) ? ID_CRTC : 0;
    p->fb = pri && crtc.on ? crtc.fb : 0;
    p->poss = 1;
    p->gamma = 0;
    uint32_t f[2] = { FOURCC_XR24, FOURCC_AR24 };
    uint32_t n = pri ? 2 : 1;
    if (!pri) f[0] = FOURCC_AR24;
    if (p->fmts && p->nfmt >= n) { U(PTR(p->fmts), n * 4); memcpy(PTR(p->fmts), f, n * 4); }
    p->nfmt = n;
    return 0;
}

static int new_fb(dfd_t* d, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fmt, uint32_t handle, uint32_t* id) {
    gem_t* g = gem_get(d, handle);
    if (!g) return -ENOENT;
    if (fmt != FOURCC_XR24 && fmt != FOURCC_AR24) return -EINVAL;
    if (w > g->w || h > g->h || pitch != g->pitch) return -EINVAL;
    for (int i = 0; i < 32; i++) if (!fbs[i].id) {
        fbs[i] = (fb_t){ d, next_fb++, handle, w, h, pitch, fmt };
        *id = fbs[i].id;
        return 0;
    }
    return -ENOMEM;
}

static int io_dumb(dfd_t* d, s_dumb* c) {
    if (c->bpp != 32 || c->w < 1 || c->h < 1 || c->w > 4096 || c->h > 4096) return -EINVAL;
    uint32_t h;
    for (h = 1; h < MAXGEM; h++) if (!gems[h].pg) break;
    if (h == MAXGEM) return -ENOMEM;
    gem_t* g = &gems[h];
    memset(g, 0, sizeof *g);
    g->w = c->w; g->h = c->h; g->pitch = c->w * 4;
    g->size = (g->pitch * g->h + 4095) & ~4095u;
    g->np = (int)(g->size / 4096);
    g->pg = kmalloc((size_t)g->np * sizeof(uintptr_t));
    if (!g->pg) return -ENOMEM;
    for (int i = 0; i < g->np; i++) {
        uint32_t fr = pmm_alloc();
        if (!fr) { g->np = i; gem_free(h); return -ENOMEM; }
        g->pg[i] = fr;
    }
    g->owner = d;
    if (be == 1) {
        g->res = 100 + h * 2;
        if (vg_create(g->res, VG_FMT_XRGB, g->w, g->h) < 0 || vg_attach(g->res, g->pg, g->np) < 0) {
            g->res = 0;
            gem_free(h);
            return -ENOMEM;
        }
    }
    c->handle = h; c->pitch = g->pitch; c->size = g->size;
    return 0;
}

/* hide/show/image/move in one place, legacy ioctl and atomic both come here */
static int cursor_set(gem_t* g, uint32_t handle, uint32_t w, uint32_t h, int hx, int hy) {
    if (g->res_a && (g->aw != w || g->ah != h)) { vg_unref(g->res_a); g->res_a = 0; }
    if (!g->res_a) {
        g->res_a = 101 + handle * 2;
        g->aw = w; g->ah = h;
        vg_create(g->res_a, VG_FMT_ARGB, w, h);
        vg_attach(g->res_a, g->pg, (int)((w * h * 4 + 4095) / 4096));
    }
    vg_xfer(g->res_a, 0, 0, w, h, g->pitch);
    crtc.cur_gem = handle;
    crtc.chx = hx; crtc.chy = hy;
    vg_cursor(g->res_a, crtc.cx, crtc.cy, hx, hy);
    return 0;
}

static int io_cursor(dfd_t* d, s_cursor* c, bool v2) {
    int e = need_master(d);
    if (e) return e;
    if (be != 1) return -ENXIO;
    if (c->crtc != ID_CRTC) return -ENOENT;
    if (c->flags & 2) { crtc.cx = c->x; crtc.cy = c->y; }
    if (c->flags & 1) {
        if (!c->handle) {
            crtc.cur_gem = 0;
            vg_cursor(0, crtc.cx, crtc.cy, 0, 0);
            return 0;
        }
        gem_t* g = gem_get(d, c->handle);
        if (!g) return -ENOENT;
        if (c->w > 64 || c->h > 64 || c->w < 1 || c->h < 1 || c->w * 4 != g->pitch) return -EINVAL;
        return cursor_set(g, c->handle, c->w, c->h, v2 ? c->hx : 0, v2 ? c->hy : 0);
    }
    if (c->flags & 2) {
        gem_t* g = crtc.cur_gem ? &gems[crtc.cur_gem] : NULL;
        if (g && g->res_a) vg_cursor_move(g->res_a, c->x, c->y);
    }
    return 0;
}

static int do_flip(dfd_t* d, uint32_t fbid, bool event, uint64_t ud) {
    if (!crtc.on) return -EINVAL;
    if (event && flip.pending) return -EBUSY;
    int i = fb_of(fbid);
    if (i < 0) return -ENOENT;
    gem_t* g = gem_of_fb(&fbs[i]);
    if (!g || fbs[i].w < crtc.mode.hdisplay || fbs[i].h < crtc.mode.vdisplay) return -EINVAL;
    if (be == 1 && fbid != crtc.fb) vg_scanout(g->res, crtc.mode.hdisplay, crtc.mode.vdisplay);
    crtc.fb = fbid;
    push(g, 0, 0, crtc.mode.hdisplay, crtc.mode.vdisplay);
    last_flip_ms = pit_uptime_ms();
    if (event) { flip.pending = true; flip.fd = d; flip.ud = ud; flip.due = vseq() + 1; }
    return 0;
}

static int io_dirty(s_dirty* r) {
    int i = fb_of(r->fb);
    if (i < 0) return -ENOENT;
    if (r->fb != crtc.fb || !crtc.on) return 0;
    gem_t* g = gem_of_fb(&fbs[i]);
    if (!g) return -ENOENT;
    uint32_t x0 = 0, y0 = 0, x1 = crtc.mode.hdisplay, y1 = crtc.mode.vdisplay;
    if (r->clips && r->nclips) {
        uint16_t* c = PTR(r->clips);
        U(c, r->nclips * 8);
        x0 = y0 = 0xffff; x1 = y1 = 0;
        for (uint32_t k = 0; k < r->nclips; k++) {
            if (c[k * 4] < x0) x0 = c[k * 4];
            if (c[k * 4 + 1] < y0) y0 = c[k * 4 + 1];
            if (c[k * 4 + 2] > x1) x1 = c[k * 4 + 2];
            if (c[k * 4 + 3] > y1) y1 = c[k * 4 + 3];
        }
        if (x1 <= x0 || y1 <= y0) return 0;
    }
    push(g, x0, y0, x1 - x0, y1 - y0);
    return 0;
}

typedef struct { uint64_t data; uint32_t len, id; } s_cblob;
typedef struct { uint32_t id, len; uint64_t data; } s_gblob;

static int io_getblob(s_gblob* b) {
    const uint8_t* src = NULL;
    uint32_t n = 0;
    if (b->id == BLOB_EDID && edid_len) { src = edid; n = (uint32_t)edid_len; }
    else if (b->id == BLOB_MODE && crtc.on) { src = (const uint8_t*)&crtc.mode; n = sizeof(dmode_t); }
    else for (int i = 0; i < 8; i++) if (blobs[i].id == b->id && b->id) { src = blobs[i].data; n = blobs[i].len; }
    if (!src) return -ENOENT;
    if (b->data && b->len >= n) { U(PTR(b->data), n); memcpy(PTR(b->data), src, n); }
    b->len = n;
    return 0;
}

static int io_mkblob(s_cblob* c) {
    if (!c->len || c->len > 4096) return -EINVAL;
    for (int i = 0; i < 8; i++) if (!blobs[i].id) {
        U(PTR(c->data), c->len);
        blobs[i].data = kmalloc(c->len);
        if (!blobs[i].data) return -ENOMEM;
        memcpy(blobs[i].data, PTR(c->data), c->len);
        blobs[i].len = c->len;
        static uint32_t nb = BLOB_USER;
        blobs[i].id = c->id = nb++;
        return 0;
    }
    return -ENOMEM;
}

static int io_rmblob(uint32_t id) {
    for (int i = 0; i < 8; i++) if (blobs[i].id == id && id) {
        kfree(blobs[i].data);
        blobs[i].id = 0;
        return 0;
    }
    return -ENOENT;
}

static int io_vblank(dfd_t* d, s_vblank* v) {
    uint32_t type = v->type;
    if (type & 0x3e) return -EINVAL;                 /* one crtc only */
    if (!crtc.on) return -EINVAL;
    uint32_t now = vseq(), want = v->seq;
    if (type & 1) want = now + v->seq;
    if (type & 0x04000000) {
        for (int i = 0; i < 8; i++) if (!vbl[i].fd) {
            vbl[i].fd = d; vbl[i].ud = (uint64_t)(uint32_t)v->sec; vbl[i].due = want;
            return 0;
        }
        return -EBUSY;
    }
    unlock();
    while ((int32_t)(vseq() - want) < 0) task_sleep_ms(4);
    lock();
    uint32_t ms = pit_uptime_ms();
    v->type = type; v->seq = vseq();
    v->sec = (long)(ms / 1000); v->usec = (long)(ms % 1000 * 1000);
    return 0;
}

static int io_setplane(dfd_t* d, uint32_t* a) {
    /* plane_id, crtc_id, fb_id, flags, crtc_x, crtc_y, crtc_w, crtc_h, src_x, src_y, src_h, src_w */
    int e = need_master(d);
    if (e) return e;
    if (a[0] != ID_PLANE) return -ENOENT;
    if (!a[2]) { crtc_off(); return 0; }
    if (!crtc.on) return -EINVAL;
    return do_flip(d, a[2], false, 0);
}

static int io_atomic(dfd_t* d, s_atomic* a) {
    int e = need_master(d);
    if (e) return e;
    if (!d->atomic) return -EINVAL;
    if (a->flags & ~0x0707u) return -EINVAL;
    if (a->nobjs > 8) return -EINVAL;
    uint32_t n = a->nobjs, tot = 0;
    if (!n) return 0;
    U(PTR(a->objs), n * 4); U(PTR(a->nprops), n * 4);
    uint32_t* objs = PTR(a->objs);
    uint32_t* np = PTR(a->nprops);
    for (uint32_t i = 0; i < n; i++) tot += np[i];
    if (tot > 128) return -EINVAL;
    U(PTR(a->props), tot * 4); U(PTR(a->vals), tot * 8);
    uint32_t* pr = PTR(a->props);
    uint64_t* vl = PTR(a->vals);

    uint32_t pfb = crtc.on ? crtc.fb : 0, mode_id = 0, cfb = 0;
    int active = crtc.on, have_cfb = 0, cx = crtc.cx, cy = crtc.cy;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < np[i]; j++, k++) {
            uint32_t o = objs[i], p = pr[k];
            uint64_t v = vl[k];
            if (o == ID_PLANE && p == P_FB_ID) pfb = (uint32_t)v;
            else if (o == ID_PLANE && p == P_CRTC_ID) { if (!v) pfb = 0; }
            else if (o == ID_CURSOR && p == P_FB_ID) { cfb = (uint32_t)v; have_cfb = 1; }
            else if (o == ID_CURSOR && p == P_CRTC_X) cx = (int)v;
            else if (o == ID_CURSOR && p == P_CRTC_X + 1) cy = (int)v;
            else if (o == ID_CRTC && p == P_ACTIVE) active = (int)v;
            else if (o == ID_CRTC && p == P_MODE_ID) mode_id = (uint32_t)v;
            else if (o == ID_CONN && p == P_CRTC_ID) {}
            else if ((o == ID_PLANE || o == ID_CURSOR) && p >= 4 && p <= 11) {}
            else if (o == ID_CONN && p == P_DPMS) {}
            else return -EINVAL;
        }
    }
    dmode_t nm = crtc.mode;
    if (mode_id) {
        s_gblob b = { mode_id, sizeof nm, 0 };
        const uint8_t* src = NULL;
        for (int i = 0; i < 8; i++) if (blobs[i].id == mode_id) src = blobs[i].data;
        if (!src || b.len != sizeof nm) return -EINVAL;
        memcpy(&nm, src, sizeof nm);
    }
    if (a->flags & 0x100) return 0;                  /* test only: nothing to check beyond parsing */
    if (!active || !pfb) { crtc_off(); return 0; }
    if (!crtc.on || (mode_id && memcmp(&nm, &crtc.mode, sizeof nm)) ) {
        if (!nm.hdisplay) return -EINVAL;
        e = set_mode(&nm, pfb);
        if (e) return e;
    } else if (pfb != crtc.fb) {
        e = do_flip(d, pfb, a->flags & 1, a->ud);
        if (e) return e;
    } else if (a->flags & 1) {
        e = do_flip(d, pfb, true, a->ud);
        if (e) return e;
    }
    if (be == 1 && (have_cfb || cx != crtc.cx || cy != crtc.cy)) {
        crtc.cx = cx; crtc.cy = cy;
        if (have_cfb && !cfb) { crtc.cur_gem = 0; vg_cursor(0, cx, cy, 0, 0); }
        else if (have_cfb) {
            int i = fb_of(cfb);
            gem_t* g = i >= 0 ? gem_of_fb(&fbs[i]) : NULL;
            if (g && fbs[i].w <= 64 && fbs[i].h <= 64) cursor_set(g, fbs[i].gem, fbs[i].w, fbs[i].h, 0, 0);
        } else if (crtc.cur_gem && gems[crtc.cur_gem].res_a) vg_cursor_move(gems[crtc.cur_gem].res_a, cx, cy);
    }
    return 0;
}

static int io_objset(dfd_t* d, s_objset* s) {
    int e = need_master(d);
    if (e) return e;
    if (s->type == OBJ_CONN && s->prop == P_DPMS) return 0;
    if (s->type == OBJ_CRTC && s->prop == P_ACTIVE && !s->val) { crtc_off(); return 0; }
    return -EINVAL;
}

static int io_planes(dfd_t* d, s_planeres* p) {
    uint32_t ids[2] = { ID_PLANE, ID_CURSOR };
    uint32_t n = d->univ ? 2 : 0;
    if (p->ids && p->n >= n && n) { U(PTR(p->ids), n * 4); memcpy(PTR(p->ids), ids, n * 4); }
    p->n = n;
    return 0;
}

static int io_gemclose(dfd_t* d, uint32_t h) {
    if (!gem_get(d, h)) return -ENOENT;
    for (int i = 0; i < 32; i++) if (fbs[i].owner == d && fbs[i].gem == h) return -EBUSY;
    if (crtc.cur_gem == h) { crtc.cur_gem = 0; if (be == 1) vg_cursor(0, 0, 0, 0, 0); }
    gem_free(h);
    return 0;
}

int drm_ioctl(dfd_t* d, uint32_t req, void* arg) {
    uint32_t nr = req & 0xff, sz = (req >> 16) & 0x3fff;
    if (((req >> 8) & 0xff) != 'd') return -ENOTTY;
    if (sz && !user_ok(arg, sz)) return -EFAULT;
    uint32_t* a32 = arg;
    uint64_t* a64 = arg;
    int r = 0;
    lock();
    switch (nr) {
    case 0x00: r = io_version(arg); break;
    case 0x01: a32[0] = 0; break;                               /* get_unique, empty */
    case 0x02: a32[0] = 1; break;                               /* get_magic */
    case 0x07: case 0x11: break;                                /* set_version, auth_magic */
    case 0x09: r = io_gemclose(d, a32[0]); break;
    case 0x0c: r = io_cap(arg); break;
    case 0x0d:
        if (a64[0] == 1) d->univ = a64[1] != 0;
        else if (a64[0] == 3) { d->atomic = a64[1] != 0; if (d->atomic) d->univ = true; }
        else if (a64[0] != 2 && a64[0] != 4 && a64[0] != 5) r = -EINVAL;
        break;
    case 0x1e:
        if (d->render) r = -EACCES;
        else if (master && master != d) r = -EBUSY;
        else { master = d; d->master = true; }
        break;
    case 0x1f:
        if (!d->master) r = -EINVAL;
        else { d->master = false; master = NULL; release(); }
        break;
    case 0x3a: r = io_vblank(d, arg); break;
    case 0xa0: r = io_getres(d, arg); break;
    case 0xa1: r = io_getcrtc(arg); break;
    case 0xa2: r = io_setcrtc(d, arg); break;
    case 0xa3: r = io_cursor(d, arg, false); break;
    case 0xa4: r = -EINVAL; break;                              /* gamma, no luts here */
    case 0xa5: break;
    case 0xa6: r = io_getenc(arg); break;
    case 0xa7: r = io_getconn(arg); break;
    case 0xa8: case 0xa9: r = -EINVAL; break;
    case 0xaa: r = io_getprop(arg); break;
    case 0xab: r = need_master(d); break;
    case 0xac: r = io_getblob(arg); break;
    case 0xad: {
        s_fbcmd* c = arg;
        int i = fb_of(c->id);
        if (i < 0) { r = -ENOENT; break; }
        c->w = fbs[i].w; c->h = fbs[i].h; c->pitch = fbs[i].pitch; c->bpp = 32; c->depth = 24;
        c->handle = fbs[i].owner == d ? fbs[i].gem : 0;
        break;
    }
    case 0xae: {
        s_fbcmd* c = arg;
        r = c->bpp != 32 ? -EINVAL : new_fb(d, c->w, c->h, c->pitch, c->depth == 32 ? FOURCC_AR24 : FOURCC_XR24, c->handle, &c->id);
        break;
    }
    case 0xaf: {
        int i = fb_of(a32[0]);
        if (i < 0 || fbs[i].owner != d) { r = -ENOENT; break; }
        if (fbs[i].id == crtc.fb && crtc.on) crtc_off();
        memset(&fbs[i], 0, sizeof fbs[i]);
        break;
    }
    case 0xb0: {
        s_flip* f = arg;
        r = need_master(d);
        if (!r && f->crtc != ID_CRTC) r = -ENOENT;
        if (!r && (f->flags & ~0x7u)) r = -EINVAL;
        if (!r) r = do_flip(d, f->fb, f->flags & 1, f->ud);
        break;
    }
    case 0xb1: r = io_dirty(arg); break;
    case 0xb2: r = io_dumb(d, arg); break;
    case 0xb3: {
        s_mapdumb* m = arg;
        if (!gem_get(d, m->handle)) r = -ENOENT;
        else m->off = (uint64_t)m->handle * GEM_SLOT;
        break;
    }
    case 0xb4: r = io_gemclose(d, a32[0]); break;
    case 0xb5: r = io_planes(d, arg); break;
    case 0xb6: r = io_getplane(arg); break;
    case 0xb7: r = io_setplane(d, arg); break;
    case 0xb8: {
        s_fbcmd2* c = arg;
        r = c->offsets[0] || c->handles[1] ? -EINVAL : new_fb(d, c->w, c->h, c->pitches[0], c->fmt, c->handles[0], &c->id);
        break;
    }
    case 0xb9: r = io_objprops(arg); break;
    case 0xba: r = io_objset(d, arg); break;
    case 0xbb: r = io_cursor(d, arg, true); break;
    case 0xbc: r = io_atomic(d, arg); break;
    case 0xbd: r = io_mkblob(arg); break;
    case 0xbe: r = io_rmblob(a32[0]); break;
    default: r = -ENOTTY;
    }
    unlock();
    return r;
}

static void put_file(const char* path, const char* s) {
    fs_node_t* n = fs_resolve(fs_root(), path);
    if (!n) n = fs_create(fs_root(), path, FS_FILE);
    if (n) fs_write(n, s, (uint32_t)strlen(s));
}

static void mk_dir(const char* path) {
    char part[128];
    int len = 0;
    for (const char* p = path; ; p++) {
        if (*p == '/' || !*p) {
            part[len] = 0;
            if (len > 1 && !fs_resolve(fs_root(), part)) fs_create(fs_root(), part, FS_DIR);
            if (!*p) break;
        }
        if (len < 127) part[len++] = *p;
    }
}

static void mk_sysfs(void) {
    char b[400], path[200];
    const char* dev = "/sys/devices/pci0000:00/drm0";
    snprintf(path, sizeof path, "%s/drm/card0", dev);
    mk_dir(path);
    snprintf(path, sizeof path, "%s/drm/renderD128", dev);
    mk_dir(path);
    mk_dir("/sys/class/drm");
    mk_dir("/sys/dev/char");
    fs_node_t* cl = fs_resolve(fs_root(), "/sys/class/drm");
    fs_symlink(cl, "card0", "../../devices/pci0000:00/drm0/drm/card0");
    fs_symlink(cl, "renderD128", "../../devices/pci0000:00/drm0/drm/renderD128");
    fs_node_t* ch = fs_resolve(fs_root(), "/sys/dev/char");
    fs_symlink(ch, "226:0", "../../devices/pci0000:00/drm0/drm/card0");
    fs_symlink(ch, "226:128", "../../devices/pci0000:00/drm0/drm/renderD128");

    const char* drv = be == 1 ? "virtio-pci" : "bochs-drm";
    snprintf(b, sizeof b, "0x%04x\n", pdev.vendor);
    snprintf(path, sizeof path, "%s/vendor", dev); put_file(path, b);
    snprintf(b, sizeof b, "0x%04x\n", pdev.device);
    snprintf(path, sizeof path, "%s/device", dev); put_file(path, b);
    snprintf(b, sizeof b, "0x%04x\n", 0x1af4);
    snprintf(path, sizeof path, "%s/subsystem_vendor", dev); put_file(path, be == 1 ? b : "0x1af4\n");
    snprintf(path, sizeof path, "%s/subsystem_device", dev); put_file(path, "0x1100\n");
    snprintf(path, sizeof path, "%s/class", dev); put_file(path, "0x030000\n");
    snprintf(path, sizeof path, "%s/revision", dev); put_file(path, be == 1 ? "0x01\n" : "0x02\n");
    snprintf(path, sizeof path, "%s/boot_vga", dev); put_file(path, be == 2 ? "1\n" : "0\n");
    snprintf(b, sizeof b, "pci:v%08Xd%08Xsv00001AF4sd00001100bc03sc00i00\n", pdev.vendor, pdev.device);
    snprintf(path, sizeof path, "%s/modalias", dev); put_file(path, b);
    snprintf(b, sizeof b, "DRIVER=%s\nPCI_CLASS=30000\nPCI_ID=%04X:%04X\nPCI_SUBSYS_ID=1AF4:1100\nPCI_SLOT_NAME=0000:%02x:%02x.%d\nMODALIAS=pci:v%08Xd%08Xsv00001AF4sd00001100bc03sc00i00\n",
             drv, pdev.vendor, pdev.device, pdev.bus, pdev.dev, pdev.fn, pdev.vendor, pdev.device);
    snprintf(path, sizeof path, "%s/uevent", dev); put_file(path, b);
    snprintf(path, sizeof path, "%s/drm/card0/dev", dev); put_file(path, "226:0\n");
    snprintf(path, sizeof path, "%s/drm/renderD128/dev", dev); put_file(path, "226:128\n");
    snprintf(b, sizeof b, "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n");
    snprintf(path, sizeof path, "%s/drm/card0/uevent", dev); put_file(path, b);
    snprintf(b, sizeof b, "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n");
    snprintf(path, sizeof path, "%s/drm/renderD128/uevent", dev); put_file(path, b);
    snprintf(path, sizeof path, "%s/drm/card0/device", dev);
    fs_symlink(fs_resolve(fs_root(), "/sys/devices/pci0000:00/drm0/drm/card0"), "device", "../..");
    fs_symlink(fs_resolve(fs_root(), "/sys/devices/pci0000:00/drm0/drm/renderD128"), "device", "../..");
    // connector dir, like card0-Virtual-1
    snprintf(path, sizeof path, "%s/drm/card0/card0-%s-1", dev, be == 1 ? "Virtual" : "VGA");
    mk_dir(path);
    char p2[200];
    snprintf(p2, sizeof p2, "%s/status", path); put_file(p2, "connected\n");
    snprintf(p2, sizeof p2, "%s/enabled", path); put_file(p2, "enabled\n");
    snprintf(p2, sizeof p2, "%s/dpms", path); put_file(p2, "On\n");
    char ml[1024];
    ml[0] = 0;
    for (int i = 0; i < nmodes && strlen(ml) < 900; i++) { strcat(ml, modes[i].name); strcat(ml, "\n"); }
    snprintf(p2, sizeof p2, "%s/modes", path); put_file(p2, ml);
    if (edid_len) {
        snprintf(p2, sizeof p2, "%s/edid", path);
        fs_node_t* n = fs_create(fs_root(), p2, FS_FILE);
        if (n) fs_write(n, (const char*)edid, (uint32_t)edid_len);
    }
}

void drm_init(void) {
    if (vg_init()) be = 1;
    else {
        pci_dev_t t;
        if (pci_find(0x1234, 0x1111, &t) && (bga_r(0) & 0xfff0) == 0xB0C0) {
            pdev = t;
            vram = (uint32_t)bga_r(0xA) * 65536;
            lfb = mmio_map(t.bar[0] & ~0xfu, vram);
            if (lfb) be = 2;
        }
    }
    if (!be) return;
    if (be == 1) {
        pci_find(0x1af4, 0x1050, &pdev);
        edid_len = vg_edid(edid);
    }
    build_modes();
    fs_node_t* dev = fs_resolve(fs_root(), "/dev");
    mk_dir("/dev/dri");
    dev = fs_resolve(fs_root(), "/dev/dri");
    fs_node_t* c = fs_create(dev, "card0", FS_FILE);
    if (c) { c->dev = FS_DEV_DRM; c->mode = 0666; }
    c = fs_create(dev, "renderD128", FS_FILE);
    if (c) { c->dev = FS_DEV_DRMR; c->mode = 0666; }
    mk_sysfs();
    task_spawn("drm", drm_task);
    com_s(be == 1 ? "drm: virtio-gpu\r\n" : "drm: bochs\r\n");
}
