#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "gfx/font.h"
#include "core/string.h"
#include "core/io.h"
#include "core/heap.h"

static uint8_t* fb;            /* current draw target */
static uint8_t* real_fb;       /* actual framebuffer */
static uint8_t* back_buf;      /* allocated when double buffering */
static int fb_w, fb_h;
static int fb_pitch_bytes;
static int fb_bpp;
static bool fb_ok = false;
static bool indexed = false;       /* mode 13h */

/* Every primitive clips to this box; the WM narrows it per damaged region. */
static int clip_x0, clip_y0, clip_x1, clip_y1;

void gfx_reset_clip(void) { clip_x0 = 0; clip_y0 = 0; clip_x1 = fb_w; clip_y1 = fb_h; }

void gfx_set_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > fb_w ? fb_w : x + w;
    clip_y1 = y + h > fb_h ? fb_h : y + h;
    if (clip_x1 < clip_x0) clip_x1 = clip_x0;
    if (clip_y1 < clip_y0) clip_y1 = clip_y0;
}

/* Curated 16-colour palette for indexed mode (mode 13h). */
static const uint32_t pal16[16] = {
    RGB(0x00,0x00,0x00),  /* 0  black */
    RGB(0xFF,0xFF,0xFF),  /* 1  white */
    RGB(0x1A,0x1F,0x3A),  /* 2  desktop bg */
    RGB(0x10,0x14,0x28),  /* 3  taskbar */
    RGB(0x6A,0x82,0xFB),  /* 4  accent */
    RGB(0x40,0x44,0x58),  /* 5  dgrey */
    RGB(0x90,0x90,0xA0),  /* 6  grey */
    RGB(0xC0,0xC0,0xC8),  /* 7  lgrey */
    RGB(0xF0,0xF0,0xF5),  /* 8  window bg */
    RGB(0x10,0x10,0x18),  /* 9  window fg */
    RGB(0xE0,0x40,0x40),  /* 10 red */
    RGB(0x40,0xC0,0x60),  /* 11 green */
    RGB(0x42,0x53,0xD4),  /* 12 title */
    RGB(0x6E,0x82,0xFF),  /* 13 title hi */
    RGB(0xE0,0xE5,0xF0),  /* 14 button face */
    RGB(0x80,0xA0,0xFF),  /* 15 hover */
};

static uint8_t pal_index(uint32_t c) {
    int best = 0, best_d = 0x7FFFFFFF;
    int cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    for (int i = 0; i < 16; i++) {
        int dr = cr - (int)((pal16[i] >> 16) & 0xFF);
        int dg = cg - (int)((pal16[i] >> 8)  & 0xFF);
        int db = cb - (int)( pal16[i]        & 0xFF);
        int d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

bool gfx_ready(void) { return fb_ok; }
int  gfx_w(void) { return fb_w; }
int  gfx_h(void) { return fb_h; }

uint8_t* gfx_front_fb(int* pitch_bytes, int* bpp) {
    if (!fb_ok || indexed) return NULL;
    if (pitch_bytes) *pitch_bytes = fb_pitch_bytes;
    if (bpp) *bpp = fb_bpp;
    return real_fb;
}

bool gfx_init(multiboot_info_t* mbi) {
    if (!(mbi->flags & MBI_FLAG_FRAMEBUFFER)) return false;
    if (mbi->framebuffer_type != 1) return false;
    if (mbi->framebuffer_bpp != 32 && mbi->framebuffer_bpp != 24) return false;
    real_fb  = (uint8_t*)(uint32_t)mbi->framebuffer_addr;
    fb       = real_fb;
    back_buf = NULL;
    fb_w     = (int)mbi->framebuffer_width;
    fb_h     = (int)mbi->framebuffer_height;
    fb_pitch_bytes = (int)mbi->framebuffer_pitch;
    fb_bpp   = mbi->framebuffer_bpp;
    indexed  = false;
    fb_ok    = true;
    gfx_reset_clip();
    return true;
}

/* ----------------------- Bochs VBE (QEMU stdvga) ----------------------- */

#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF
enum {
    VBE_ID = 0, VBE_XRES, VBE_YRES, VBE_BPP,
    VBE_ENABLE, VBE_BANK, VBE_VIRT_W, VBE_VIRT_H,
    VBE_X_OFF, VBE_Y_OFF
};
#define VBE_DISABLED 0x00
#define VBE_ENABLED  0x01
#define VBE_LFB      0x40

static void bga_write(uint16_t idx, uint16_t val) {
    outw(VBE_INDEX, idx);
    outw(VBE_DATA, val);
}
static uint16_t bga_read(uint16_t idx) {
    outw(VBE_INDEX, idx);
    return inw(VBE_DATA);
}

/* PCI configuration space access (mechanism #1). */
static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000U
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func << 8)
                  | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

/* Find first PCI display controller, return its BAR0 (memory) address. */
static uint32_t pci_find_vga_lfb(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t v = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((v & 0xFFFF) == 0xFFFF) continue;
            uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x08);
            uint8_t baseclass = (cls >> 24) & 0xFF;
            if (baseclass == 0x03) {
                uint32_t bar0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x10);
                if ((bar0 & 1) == 0) return bar0 & 0xFFFFFFF0U;
            }
        }
    }
    return 0;
}

bool gfx_init_vbe(int w, int h, int bpp) {
    uint16_t id = bga_read(VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return false;     /* not Bochs VBE */

    bga_write(VBE_ENABLE, VBE_DISABLED);
    bga_write(VBE_XRES,   (uint16_t)w);
    bga_write(VBE_YRES,   (uint16_t)h);
    bga_write(VBE_BPP,    (uint16_t)bpp);
    bga_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB);

    /* Verify mode set actually took effect. */
    if (bga_read(VBE_XRES) != w) return false;
    if (bga_read(VBE_YRES) != h) return false;

    uint32_t lfb = pci_find_vga_lfb();
    if (!lfb) return false;

    real_fb  = (uint8_t*)lfb;
    fb       = real_fb;
    back_buf = NULL;
    fb_w     = w;
    fb_h     = h;
    fb_bpp   = bpp;
    fb_pitch_bytes = w * (bpp / 8);
    indexed  = false;
    fb_ok    = true;
    gfx_reset_clip();
    return true;
}

/* ---------------- Mode 13h fallback (320x200x8) ---------------- */

static void set_mode_13h(void) {
    outb(0x3C2, 0x63);

    static const uint8_t seq[5] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
    for (int i = 0; i < 5; i++) { outb(0x3C4, i); outb(0x3C5, seq[i]); }

    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);

    static const uint8_t crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
        0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF
    };
    for (int i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, crtc[i]); }

    static const uint8_t gc[9] = {
        0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF
    };
    for (int i = 0; i < 9; i++) { outb(0x3CE, i); outb(0x3CF, gc[i]); }

    inb(0x3DA);
    static const uint8_t ac[21] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x41,0x00,0x0F,0x00,0x00
    };
    for (int i = 0; i < 21; i++) { outb(0x3C0, i); outb(0x3C0, ac[i]); }
    outb(0x3C0, 0x20);
}

static void set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, idx);
    outb(0x3C9, r >> 2);
    outb(0x3C9, g >> 2);
    outb(0x3C9, b >> 2);
}

bool gfx_init_mode13h(void) {
    set_mode_13h();
    for (int i = 0; i < 16; i++) {
        uint8_t r = (pal16[i] >> 16) & 0xFF;
        uint8_t g = (pal16[i] >> 8)  & 0xFF;
        uint8_t b =  pal16[i]        & 0xFF;
        set_palette((uint8_t)i, r, g, b);
    }
    real_fb  = (uint8_t*)0xA0000;
    fb       = real_fb;
    back_buf = NULL;
    fb_w     = 320;
    fb_h     = 200;
    fb_pitch_bytes = 320;
    fb_bpp   = 8;
    indexed  = true;
    fb_ok    = true;
    gfx_reset_clip();
    return true;
}

/* ---- Double buffering ---- */
bool gfx_enable_double_buffer(void) {
    if (back_buf) return true;
    if (!fb_ok || fb_bpp != 32) return false;
    int sz = fb_pitch_bytes * fb_h;
    back_buf = (uint8_t*)kmalloc(sz);
    if (!back_buf) return false;
    /* seed back buffer with current screen */
    memcpy(back_buf, real_fb, sz);
    return true;
}
void gfx_disable_double_buffer(void) {
    if (back_buf) { kfree(back_buf); back_buf = NULL; }
    fb = real_fb;
}
void gfx_target_back(void)  { if (back_buf) fb = back_buf; }
void gfx_target_front(void) { fb = real_fb; }
void gfx_present(void) {
    if (!back_buf || !real_fb) return;
    uint32_t n = (uint32_t)(fb_pitch_bytes * fb_h) / 4;
    void* d = real_fb;
    void* s = back_buf;
    __asm__ volatile (
        "cld\n\t"
        "rep movsl"
        : "+D"(d), "+S"(s), "+c"(n)
        :
        : "memory"
    );
}

void gfx_present_rect(int x, int y, int w, int h) {
    if (!back_buf || !real_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    int bpp = fb_bpp / 8;
    if (bpp < 1) bpp = 1;
    int row_bytes = w * bpp;
    for (int yy = 0; yy < h; yy++) {
        memcpy(real_fb  + (y + yy) * fb_pitch_bytes + x * bpp,
               back_buf + (y + yy) * fb_pitch_bytes + x * bpp,
               (size_t)row_bytes);
    }
}

/* ---------- Drawing primitives ---------- */

static inline void put32(int x, int y, uint32_t c) {
    *(uint32_t*)(fb + y * fb_pitch_bytes + x * 4) = c;
}
static inline void put24(int x, int y, uint32_t c) {
    uint8_t* p = fb + y * fb_pitch_bytes + x * 3;
    p[0] = c & 0xFF;
    p[1] = (c >> 8) & 0xFF;
    p[2] = (c >> 16) & 0xFF;
}
static inline void put8(int x, int y, uint32_t c) {
    fb[y * fb_pitch_bytes + x] = pal_index(c);
}
static inline uint32_t get32(int x, int y) {
    return *(uint32_t*)(fb + y * fb_pitch_bytes + x * 4);
}
static inline uint32_t get24(int x, int y) {
    uint8_t* p = fb + y * fb_pitch_bytes + x * 3;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
static inline uint32_t get8(int x, int y) {
    uint8_t i = fb[y * fb_pitch_bytes + x];
    return (i < 16) ? pal16[i] : 0;
}

void gfx_pixel(int x, int y, uint32_t c) {
    if (!fb_ok || x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    if (fb_bpp == 32) put32(x, y, c);
    else if (fb_bpp == 24) put24(x, y, c);
    else put8(x, y, c);
}

uint32_t gfx_get_pixel(int x, int y) {
    if (!fb_ok || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return 0;
    if (fb_bpp == 32) return get32(x, y);
    if (fb_bpp == 24) return get24(x, y);
    return get8(x, y);
}

void gfx_clear(uint32_t c) {
    if (!fb_ok) return;
    gfx_rect_fill(0, 0, fb_w, fb_h, c);
}

void gfx_rect_fill(int x, int y, int w, int h, uint32_t c) {
    if (!fb_ok) return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w; if (x1 > clip_x1) x1 = clip_x1;
    int y1 = y + h; if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) return;
    if (fb_bpp == 32) {
        uint32_t span = (uint32_t)(x1 - x0);
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* row = (uint32_t*)(fb + yy * fb_pitch_bytes) + x0;
            uint32_t n = span;
            void* d = row;
            __asm__ volatile (
                "cld\n\t"
                "rep stosl"
                : "+D"(d), "+c"(n)
                : "a"(c)
                : "memory"
            );
        }
    } else if (fb_bpp == 8) {
        uint8_t pi = pal_index(c);
        for (int yy = y0; yy < y1; yy++) {
            uint8_t* row = fb + yy * fb_pitch_bytes;
            for (int xx = x0; xx < x1; xx++) row[xx] = pi;
        }
    } else {
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) put24(xx, yy, c);
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t c) {
    gfx_rect_fill(x, y, w, 1, c);
    gfx_rect_fill(x, y + h - 1, w, 1, c);
    gfx_rect_fill(x, y, 1, h, c);
    gfx_rect_fill(x + w - 1, y, 1, h, c);
}

void gfx_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int safety = 0;
    while (1) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        if (++safety > 4000) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_disc(int cx, int cy, int r, uint32_t c) {
    if (r <= 0) { gfx_pixel(cx, cy, c); return; }
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int rr = r2 - dy*dy;
        if (rr < 0) continue;
        int dx;
        for (dx = 0; dx*dx <= rr; dx++) {}
        dx--;
        gfx_rect_fill(cx - dx, cy + dy, 2*dx + 1, 1, c);
    }
}

void gfx_circle(int cx, int cy, int r, uint32_t c) {
    int x = r, y = 0, err = 0;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx + y, cy + x, c);
        gfx_pixel(cx - y, cy + x, c); gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx - x, cy - y, c); gfx_pixel(cx - y, cy - x, c);
        gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx + x, cy - y, c);
        y++;
        if (err <= 0) { err += 2*y + 1; }
        if (err > 0)  { x--; err -= 2*x + 1; }
    }
}

void gfx_glyph(int x, int y, char c, uint32_t fg, uint32_t bg, bool draw_bg) {
    if (!fb_ok) return;
    if (x >= clip_x1 || y >= clip_y1 || x + FONT_W <= clip_x0 || y + FONT_H <= clip_y0)
        return;
    const uint8_t* g = font_glyph((uint8_t)c);
    if (fb_bpp == 32 && x >= clip_x0 && y >= clip_y0 &&
        x + FONT_W <= clip_x1 && y + FONT_H <= clip_y1) {
        for (int r = 0; r < FONT_H; r++) {
            uint32_t* row = (uint32_t*)(fb + (y + r) * fb_pitch_bytes) + x;
            uint8_t bits = g[r];
            for (int b = 0; b < FONT_W; b++) {
                if (bits & (0x80 >> b)) row[b] = fg;
                else if (draw_bg)      row[b] = bg;
            }
        }
        return;
    }
    for (int r = 0; r < FONT_H; r++) {
        uint8_t row = g[r];
        for (int b = 0; b < FONT_W; b++) {
            if (row & (0x80 >> b)) gfx_pixel(x + b, y + r, fg);
            else if (draw_bg)      gfx_pixel(x + b, y + r, bg);
        }
    }
}

/* 8x16 text in the anti-aliased terminal face (DejaVu Sans Mono); box
   drawing and anything the face lacks falls back to the VGA bitmap. */
void gfx_string(int x, int y, const char* s, uint32_t fg, uint32_t bg, bool draw_bg) {
    while (*s) {
        uif_mono_char(x, y, (uint8_t)*s, fg, bg, draw_bg);
        x += FONT_W;
        s++;
    }
}

void gfx_get_clip(int* x, int* y, int* w, int* h) {
    *x = clip_x0; *y = clip_y0; *w = clip_x1 - clip_x0; *h = clip_y1 - clip_y0;
}

void gfx_save_rect(int x, int y, int w, int h, uint32_t* dst) {
    if (!fb_ok || !dst) return;
    if (fb_bpp == 32 && x >= 0 && y >= 0 && x + w <= fb_w && y + h <= fb_h) {
        for (int yy = 0; yy < h; yy++) {
            memcpy(dst + yy * w,
                   fb + (y + yy) * fb_pitch_bytes + x * 4,
                   (size_t)w * 4);
        }
        return;
    }
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx, sy = y + yy;
            if (sx < 0 || sy < 0 || sx >= fb_w || sy >= fb_h) dst[yy*w + xx] = 0;
            else dst[yy*w + xx] = gfx_get_pixel(sx, sy);
        }
    }
}

void gfx_restore_rect(int x, int y, int w, int h, const uint32_t* src) {
    if (!fb_ok || !src) return;
    if (fb_bpp == 32 && x >= 0 && y >= 0 && x + w <= fb_w && y + h <= fb_h) {
        for (int yy = 0; yy < h; yy++) {
            memcpy(fb + (y + yy) * fb_pitch_bytes + x * 4,
                   src + yy * w,
                   (size_t)w * 4);
        }
        return;
    }
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx, sy = y + yy;
            if (sx < 0 || sy < 0 || sx >= fb_w || sy >= fb_h) continue;
            gfx_pixel(sx, sy, src[yy*w + xx]);
        }
    }
}

void gfx_blit_argb(int dst_x, int dst_y, int src_w, int src_h, const uint32_t* src) {
    if (!fb_ok || !src) return;
    int x0 = dst_x, y0 = dst_y;
    int sx0 = 0, sy0 = 0;
    if (x0 < clip_x0) { sx0 = clip_x0 - x0; x0 = clip_x0; }
    if (y0 < clip_y0) { sy0 = clip_y0 - y0; y0 = clip_y0; }
    int x1 = dst_x + src_w; if (x1 > clip_x1) x1 = clip_x1;
    int y1 = dst_y + src_h; if (y1 > clip_y1) y1 = clip_y1;
    int cw = x1 - x0, ch = y1 - y0;
    if (cw <= 0 || ch <= 0) return;

    if (fb_bpp == 32) {
        for (int yy = 0; yy < ch; yy++) {
            uint32_t* row = (uint32_t*)(fb + (y0 + yy) * fb_pitch_bytes) + x0;
            const uint32_t* srow = src + (sy0 + yy) * src_w + sx0;
            memcpy(row, srow, (size_t)cw * 4);
        }
    } else {
        for (int yy = 0; yy < ch; yy++) {
            const uint32_t* srow = src + (sy0 + yy) * src_w + sx0;
            for (int xx = 0; xx < cw; xx++)
                gfx_pixel(x0 + xx, y0 + yy, srow[xx]);
        }
    }
}

/* ---------- Alpha compositing (32bpp only; others fall back to opaque) ---------- */

/* a in 0..256 */
static inline uint32_t mix(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t na = 256 - a;
    uint32_t rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * na) >> 8) & 0xFF00FF;
    uint32_t g  = (((s & 0x00FF00) * a + (d & 0x00FF00) * na) >> 8) & 0x00FF00;
    return rb | g;
}

void gfx_blend_pixel(int x, int y, uint32_t c, int alpha) {
    if (alpha <= 0) return;
    if (!fb_ok || x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    if (fb_bpp != 32) { if (alpha >= 128) gfx_pixel(x, y, c); return; }
    uint32_t a = (uint32_t)alpha + ((uint32_t)alpha >> 7);
    uint32_t* p = (uint32_t*)(fb + y * fb_pitch_bytes) + x;
    *p = mix(*p, c, a);
}

/* Coverage mask blit for anti-aliased text: a[] is w*h alpha, row stride. */
void gfx_alpha_mask(int x, int y, int w, int h, const uint8_t* a, int stride, uint32_t c) {
    if (!fb_ok) return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int py = y0; py < y1; py++) {
        const uint8_t* src = a + (py - y) * stride + (x0 - x);
        if (fb_bpp != 32) {
            for (int px = x0; px < x1; px++, src++) if (*src >= 128) gfx_pixel(px, py, c);
            continue;
        }
        uint32_t* row = (uint32_t*)(fb + py * fb_pitch_bytes);
        for (int px = x0; px < x1; px++, src++) {
            uint32_t al = *src;
            if (!al) continue;
            if (al == 255) { row[px] = c; continue; }
            row[px] = mix(row[px], c, al + (al >> 7));
        }
    }
}

void gfx_rect_blend(int x, int y, int w, int h, uint32_t c, int alpha) {
    if (alpha >= 255) { gfx_rect_fill(x, y, w, h, c); return; }
    if (alpha <= 0 || !fb_ok) return;
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    if (fb_bpp != 32) { if (alpha >= 128) gfx_rect_fill(x, y, w, h, c); return; }
    uint32_t a = (uint32_t)alpha + ((uint32_t)alpha >> 7);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = (uint32_t*)(fb + yy * fb_pitch_bytes);
        for (int xx = x0; xx < x1; xx++) row[xx] = mix(row[xx], c, a);
    }
}

/* Anti-aliased quarter-circle coverage for radius r, 4x4 supersampled.
   cov[dy*r+dx] is 0..256 for the top-left corner square. */
#define RR_MAX 12
static uint16_t rr_cov[RR_MAX * RR_MAX];
static int      rr_cov_r = -1;

static void rr_build(int r) {
    if (r == rr_cov_r) return;
    int c = r * 8, lim = c * c;
    for (int dy = 0; dy < r; dy++)
        for (int dx = 0; dx < r; dx++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px = c - (dx * 8 + sx * 2 + 1);
                    int py = c - (dy * 8 + sy * 2 + 1);
                    if (px * px + py * py <= lim) n++;
                }
            rr_cov[dy * r + dx] = (uint16_t)(n * 16);
        }
    rr_cov_r = r;
}

void gfx_rrect_fill(int x, int y, int w, int h, int r, int corners, uint32_t c) {
    if (r > RR_MAX) r = RR_MAX;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0 || !corners) { gfx_rect_fill(x, y, w, h, c); return; }
    rr_build(r);
    bool tl = corners & GFX_CORNER_TL, tr = corners & GFX_CORNER_TR;
    bool bl = corners & GFX_CORNER_BL, br = corners & GFX_CORNER_BR;

    gfx_rect_fill(x, y + r, w, h - 2 * r, c);
    gfx_rect_fill(x + r, y, w - 2 * r, r, c);
    gfx_rect_fill(x + r, y + h - r, w - 2 * r, r, c);
    for (int dy = 0; dy < r; dy++)
        for (int dx = 0; dx < r; dx++) {
            int a = rr_cov[dy * r + dx];
            int ya = y + dy, yb = y + h - 1 - dy;
            int xa = x + dx, xb = x + w - 1 - dx;
            if (tl) gfx_blend_pixel(xa, ya, c, a); else gfx_pixel(xa, ya, c);
            if (tr) gfx_blend_pixel(xb, ya, c, a); else gfx_pixel(xb, ya, c);
            if (bl) gfx_blend_pixel(xa, yb, c, a); else gfx_pixel(xa, yb, c);
            if (br) gfx_blend_pixel(xb, yb, c, a); else gfx_pixel(xb, yb, c);
        }
}

/* Soft drop shadow around a rounded rect: quadratic falloff over `size` px,
   darkening whatever is already in the target. Interior rows skip the span
   the window itself will cover. */
#define SH_MAX_D2 2048
static uint16_t sh_alpha[SH_MAX_D2];
static int sh_key_size = -1, sh_key_r = -1, sh_key_op = -1;

static int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void sh_build(int size, int r, int opacity) {
    if (size == sh_key_size && r == sh_key_r && opacity == sh_key_op) return;
    for (int d2 = 0; d2 < SH_MAX_D2; d2++) {
        /* distance in 1/4 px for smoother ramps */
        int d = isqrt(d2 * 16) - r * 4;
        if (d < 0) d = 0;
        int span = size * 4;
        int a = 0;
        if (d < span) {
            int t = span - d;                       /* 0..span */
            a = opacity * t / span * t / span;      /* quadratic */
        }
        sh_alpha[d2] = (uint16_t)a;
    }
    sh_key_size = size; sh_key_r = r; sh_key_op = opacity;
}

void gfx_shadow(int x, int y, int w, int h, int r, int size, int opacity) {
    if (!fb_ok || fb_bpp != 32 || size <= 0) return;
    if ((size + r) * (size + r) * 2 >= SH_MAX_D2) size = isqrt(SH_MAX_D2 / 2) - r - 1;
    sh_build(size, r, opacity);
    int bx0 = x - size, by0 = y - size, bx1 = x + w + size, by1 = y + h + size;
    int x0 = bx0 < clip_x0 ? clip_x0 : bx0;
    int y0 = by0 < clip_y0 ? clip_y0 : by0;
    int x1 = bx1 > clip_x1 ? clip_x1 : bx1;
    int y1 = by1 > clip_y1 ? clip_y1 : by1;
    if (x0 >= x1 || y0 >= y1) return;
    int ix0 = x + r, ix1 = x + w - 1 - r;           /* inner core box */
    int iy0 = y + r, iy1 = y + h - 1 - r;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = (uint32_t*)(fb + py * fb_pitch_bytes);
        int qy = py < iy0 ? iy0 - py : (py > iy1 ? py - iy1 : 0);
        bool interior = (py >= y + r + 2 && py < y + h - r - 2);
        for (int px = x0; px < x1; px++) {
            if (interior && px >= x + r + 2 && px < x + w - r - 2) {
                px = x + w - r - 3;                  /* jump the covered span */
                continue;
            }
            int qx = px < ix0 ? ix0 - px : (px > ix1 ? px - ix1 : 0);
            int d2 = qx * qx + qy * qy;
            if (d2 >= SH_MAX_D2) continue;
            uint32_t a = sh_alpha[d2];
            if (!a) continue;
            uint32_t v = row[px], na = 256 - a;
            row[px] = ((((v & 0xFF00FF) * na) >> 8) & 0xFF00FF) |
                      ((((v & 0x00FF00) * na) >> 8) & 0x00FF00);
        }
    }
}
