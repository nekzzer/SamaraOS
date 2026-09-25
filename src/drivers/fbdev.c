#include "drivers/fbdev.h"
#include "gfx/gfx.h"
#include "core/heap.h"
#include "core/string.h"

static volatile int  users;
static volatile bool active;
static uint8_t*      snap;
static uint32_t      snap_bytes;
static bool          cleared;
static uint32_t      rowbuf[4096];

static uint8_t* fb32(int* pitch, int* w, int* h) {
    int bpp;
    uint8_t* p = gfx_front_fb(pitch, &bpp);
    if (!p || bpp != 32) return NULL;
    *w = gfx_w(); *h = gfx_h();
    return p;
}

bool fbdev_open(void) {
    int pitch, w, h;
    uint8_t* fb = fb32(&pitch, &w, &h);
    if (!fb) return false;
    if (users++ == 0) {
        snap_bytes = (uint32_t)pitch * (uint32_t)h;
        snap = (uint8_t*)kmalloc(snap_bytes);          /* NULL: WM redraws instead */
        if (snap) memcpy(snap, fb, snap_bytes);
        cleared = false;
        active = true;
    }
    return true;
}

void fbdev_restore(void) {
    if (!snap) return;
    int pitch, w, h;
    uint8_t* fb = fb32(&pitch, &w, &h);
    if (fb) memcpy(fb, snap, snap_bytes);
    kfree(snap);
    snap = NULL;
}

void fbdev_close(void) {
    if (users > 0 && --users == 0) {
        active = false;
        fbdev_restore();
    }
}

bool fbdev_active(void) { return active; }
void fbdev_force_release(void) { active = false; }

int fbdev_write(uint32_t off, const char* buf, uint32_t n) {
    int pitch, w, h;
    uint8_t* fb = fb32(&pitch, &w, &h);
    if (!fb || !active) return -5;                     /* EIO */
    uint32_t total = (uint32_t)pitch * (uint32_t)h;
    if (off >= total) return -28;                      /* ENOSPC */
    if (n > total - off) n = total - off;
    memcpy(fb + off, buf, n);
    return (int)n;
}

void fbdev_vscreeninfo(uint32_t* o) {
    memset(o, 0, 40 * 4);
    o[0] = o[2] = (uint32_t)gfx_w();                   /* xres, xres_virtual */
    o[1] = o[3] = (uint32_t)gfx_h();                   /* yres, yres_virtual */
    o[6] = 32;                                         /* bits_per_pixel */
    o[8] = 16; o[9] = 8;                               /* red   offset, length */
    o[11] = 8; o[12] = 8;                              /* green */
    o[14] = 0; o[15] = 8;                              /* blue */
}

void fbdev_fscreeninfo(uint32_t* o) {
    int pitch, bpp;
    memset(o, 0, 17 * 4);
    uint8_t* fb = gfx_front_fb(&pitch, &bpp);
    o[4] = (uint32_t)fb;                               /* smem_start */
    o[5] = (uint32_t)pitch * (uint32_t)gfx_h();        /* smem_len */
    o[11] = (uint32_t)pitch;                           /* line_length */
}

int fbdev_blit8(const uint8_t* src, int w, int h, const uint32_t* pal) {
    int pitch, W, H;
    uint8_t* fb = fb32(&pitch, &W, &H);
    if (!fb || !active) return -5;                     /* EIO */
    if (w < 1 || h < 1 || w > W || h > H) return -22;
    int scale = W / w < H / h ? W / w : H / h;
    if (scale > 4096 / w) scale = 4096 / w;
    if (scale < 1) scale = 1;
    int ox = (W - w * scale) / 2, oy = (H - h * scale) / 2;
    if (!cleared) {                                    /* black bars, once */
        memset(fb, 0, (size_t)pitch * (size_t)H);
        cleared = true;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + y * w;
        uint32_t* d = rowbuf;
        if (scale == 1) for (int x = 0; x < w; x++) *d++ = pal[s[x]];
        else for (int x = 0; x < w; x++) {
            uint32_t c = pal[s[x]];
            for (int k = 0; k < scale; k++) *d++ = c;
        }
        for (int k = 0; k < scale; k++)
            memcpy(fb + (size_t)(oy + y * scale + k) * pitch + ox * 4, rowbuf,
                   (size_t)w * scale * 4);
    }
    return 0;
}
