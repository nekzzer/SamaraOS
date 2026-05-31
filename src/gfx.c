#include "gfx.h"
#include "font.h"
#include "string.h"
#include "io.h"
#include "heap.h"

static uint8_t* fb;            /* current draw target */
static uint8_t* real_fb;       /* actual framebuffer */
static uint8_t* back_buf;      /* allocated when double buffering */
static int fb_w, fb_h;
static int fb_pitch_bytes;
static int fb_bpp;
static bool fb_ok = false;
static bool indexed = false;       /* mode 13h */

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
    if (!fb_ok || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
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
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > fb_w) x1 = fb_w;
    int y1 = y + h; if (y1 > fb_h) y1 = fb_h;
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
    const uint8_t* g = font_glyph((uint8_t)c);
    for (int r = 0; r < FONT_H; r++) {
        uint8_t row = g[r];
        for (int b = 0; b < FONT_W; b++) {
            if (row & (0x80 >> b)) gfx_pixel(x + b, y + r, fg);
            else if (draw_bg)      gfx_pixel(x + b, y + r, bg);
        }
    }
}

void gfx_string(int x, int y, const char* s, uint32_t fg, uint32_t bg, bool draw_bg) {
    while (*s) {
        gfx_glyph(x, y, *s, fg, bg, draw_bg);
        x += FONT_W;
        s++;
    }
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
    if (x0 < 0) { sx0 = -x0; x0 = 0; }
    if (y0 < 0) { sy0 = -y0; y0 = 0; }
    int x1 = dst_x + src_w; if (x1 > fb_w) x1 = fb_w;
    int y1 = dst_y + src_h; if (y1 > fb_h) y1 = fb_h;
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
