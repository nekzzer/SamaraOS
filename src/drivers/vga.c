#include "drivers/vga.h"
#include "core/io.h"
#include "core/string.h"
#include "gfx/gfx_term.h"
#include "gfx/termfont.h"
#include <stdarg.h>

static volatile uint16_t* const VRAM = (uint16_t*)0xB8000;
static int cur_x = 0, cur_y = 0;
static uint8_t cur_color = (VGA_BLACK << 4) | VGA_LGREY;

typedef enum { CM_TEXT, CM_GFX } console_mode_t;
static console_mode_t mode = CM_TEXT;

static inline uint16_t cell(char c, uint8_t color) {
    return ((uint16_t)color << 8) | (uint8_t)c;
}

/* ---------------- TEXT MODE BACKEND ---------------- */

static void text_set_color(uint8_t fg, uint8_t bg) {
    cur_color = (bg << 4) | (fg & 0x0F);
}

static void text_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VRAM[y * VGA_WIDTH + x] = cell(' ', cur_color);
    cur_x = cur_y = 0;
    vga_update_hw_cursor();
}

static void scroll_if_needed(void) {
    if (cur_y < VGA_HEIGHT) return;
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VRAM[(y - 1) * VGA_WIDTH + x] = VRAM[y * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        VRAM[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell(' ', cur_color);
    cur_y = VGA_HEIGHT - 1;
}

static void text_putc(char c) {
    if (c == '\n') { cur_x = 0; cur_y++; }
    else if (c == '\r') { cur_x = 0; }
    else if (c == '\b') {
        if (cur_x > 0) { cur_x--; VRAM[cur_y * VGA_WIDTH + cur_x] = cell(' ', cur_color); }
    } else if (c == '\t') {
        cur_x = (cur_x + 8) & ~7;
        if (cur_x >= VGA_WIDTH) { cur_x = 0; cur_y++; }
    } else if ((uint8_t)c >= 0x20) {
        /* Deferred wrap (like a VT100 / the gfx terminal): the cursor may
           rest past the last column until the next character arrives. */
        if (cur_x >= VGA_WIDTH) { cur_x = 0; cur_y++; scroll_if_needed(); }
        VRAM[cur_y * VGA_WIDTH + cur_x] = cell(c, cur_color);
        cur_x++;
    }
    scroll_if_needed();
    vga_update_hw_cursor();
}

static void text_get_cursor(int* x, int* y) { *x = cur_x; *y = cur_y; }
static void text_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    cur_x = x; cur_y = y; vga_update_hw_cursor();
}

/* ---------------- DISPATCHERS ---------------- */

void vga_putc(char c) {
    if (mode == CM_GFX) gfx_term_putc(c);
    else text_putc(c);
}

void vga_puts(const char* s) { while (*s) vga_putc(*s++); }

void vga_putn(int v) { char b[16]; itoa(v, b, 10); vga_puts(b); }

void vga_putx(uint32_t v) { char b[16]; utoa(v, b, 16); vga_puts("0x"); vga_puts(b); }

void vga_set_color(uint8_t fg, uint8_t bg) {
    if (mode == CM_GFX) gfx_term_set_color(fg, bg);
    else text_set_color(fg, bg);
}

/* True colour where the console can show it (the desktop terminal); the
   VGA text screen gets the nearest of its 16 colours. */
void vga_set_rgb(uint32_t fg, uint32_t bg) {
    if (mode == CM_GFX) { gfx_term_set_rgb(fg, bg); return; }
    static const uint32_t pal[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };
    uint8_t best[2];
    uint32_t want[2] = { fg, bg };
    for (int k = 0; k < 2; k++) {
        int bi = 0; uint32_t bd = 0xFFFFFFFF;
        for (int i = 0; i < 16; i++) {
            int dr = (int)((want[k] >> 16) & 255) - (int)((pal[i] >> 16) & 255);
            int dg = (int)((want[k] >> 8) & 255) - (int)((pal[i] >> 8) & 255);
            int db = (int)(want[k] & 255) - (int)(pal[i] & 255);
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < bd) { bd = d; bi = i; }
        }
        best[k] = (uint8_t)bi;
    }
    text_set_color(best[0], best[1]);
}

/* Unicode output: the graphical console shows it as is, the VGA text
   screen gets the CP866 equivalent (or '?'). */
void vga_putu(uint32_t cp) {
    if (mode == CM_GFX) { gfx_term_putu(cp); return; }
    int c = cp < 0x80 ? (int)cp : uni_to_cp866(cp);
    text_putc((char)(c < 0 ? '?' : c));
}

void vga_set_attr(uint8_t at)       { if (mode == CM_GFX) gfx_term_set_attr(at); }
void vga_alt_screen(bool on)        { if (mode == CM_GFX) gfx_term_alt(on); }
void vga_cursor_visible(bool on)    { if (mode == CM_GFX) gfx_term_cursor_visible(on); }
bool vga_is_gfx(void)               { return mode == CM_GFX; }

void vga_clear(void) {
    if (mode == CM_GFX) gfx_term_clear();
    else text_clear();
}

void vga_get_cursor(int* x, int* y) {
    if (mode == CM_GFX) gfx_term_get_cursor(x, y);
    else text_get_cursor(x, y);
}

void vga_set_cursor(int x, int y) {
    if (mode == CM_GFX) gfx_term_set_cursor(x, y);
    else text_set_cursor(x, y);
}

void vga_update_hw_cursor(void) {
    if (mode != CM_TEXT) return;
    int hx = cur_x >= VGA_WIDTH ? VGA_WIDTH - 1 : cur_x;   /* deferred wrap */
    uint16_t pos = (uint16_t)(cur_y * VGA_WIDTH + hx);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* These three are text-mode-only (used by mouse text cursor & blinker). */
void vga_putcell_at(int x, int y, char c, uint8_t color) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    VRAM[y * VGA_WIDTH + x] = cell(c, color);
}
uint16_t vga_get_cell(int x, int y) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return 0;
    return VRAM[y * VGA_WIDTH + x];
}
void vga_set_cell(int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    VRAM[y * VGA_WIDTH + x] = c;
}

/* Blank [x0, x1) on row y in the current colour without moving the cursor
   (ANSI erase-in-line/display). */
void vga_erase(int x0, int y, int x1) {
    if (mode == CM_GFX) { gfx_term_erase(x0, y, x1); return; }
    if (x0 < 0) x0 = 0;
    if (x1 > VGA_WIDTH) x1 = VGA_WIDTH;
    if (y < 0 || y >= VGA_HEIGHT || x0 >= x1) return;
    for (int x = x0; x < x1; x++) VRAM[y * VGA_WIDTH + x] = cell(' ', cur_color);
}

void vga_scroll_region(int top, int bot, int n) {
    if (mode == CM_GFX) { gfx_term_scroll_region(top, bot, n); return; }
    if (top < 0) top = 0;
    if (bot >= VGA_HEIGHT) bot = VGA_HEIGHT - 1;
    if (top > bot || n == 0) return;
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n < -h) n = -h;
    if (n > 0) {
        for (int y = top; y <= bot - n; y++)
            for (int x = 0; x < VGA_WIDTH; x++) VRAM[y * VGA_WIDTH + x] = VRAM[(y + n) * VGA_WIDTH + x];
        for (int y = bot - n + 1; y <= bot; y++)
            for (int x = 0; x < VGA_WIDTH; x++) VRAM[y * VGA_WIDTH + x] = cell(' ', cur_color);
    } else {
        n = -n;
        for (int y = bot; y >= top + n; y--)
            for (int x = 0; x < VGA_WIDTH; x++) VRAM[y * VGA_WIDTH + x] = VRAM[(y - n) * VGA_WIDTH + x];
        for (int y = top; y < top + n; y++)
            for (int x = 0; x < VGA_WIDTH; x++) VRAM[y * VGA_WIDTH + x] = cell(' ', cur_color);
    }
}

void vga_shift_chars(int x, int y, int n) {
    if (mode == CM_GFX) { gfx_term_shift_chars(x, y, n); return; }
    if (y < 0 || y >= VGA_HEIGHT || x < 0 || x >= VGA_WIDTH || n == 0) return;
    volatile uint16_t* row = VRAM + y * VGA_WIDTH;
    int span = VGA_WIDTH - x, k = n > 0 ? n : -n;
    if (k > span) k = span;
    if (n > 0) {
        for (int i = VGA_WIDTH - 1; i >= x + k; i--) row[i] = row[i - k];
        for (int i = 0; i < k; i++) row[x + i] = cell(' ', cur_color);
    } else {
        for (int i = x; i < VGA_WIDTH - k; i++) row[i] = row[i + k];
        for (int i = VGA_WIDTH - k; i < VGA_WIDTH; i++) row[i] = cell(' ', cur_color);
    }
}

void vga_init(void) {
    mode = CM_TEXT;
    text_set_color(VGA_LGREY, VGA_BLACK);
    text_clear();
}

int vga_cols(void) { return mode == CM_GFX ? gfx_term_cols() : VGA_WIDTH; }
int vga_rows(void) { return mode == CM_GFX ? gfx_term_rows() : VGA_HEIGHT; }

void vga_use_text(void)               { mode = CM_TEXT; }
void vga_use_gfx_term(int px, int py) { gfx_term_init(px, py); mode = CM_GFX; }

void vga_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[32];
    for (; *fmt; fmt++) {
        if (*fmt != '%') { vga_putc(*fmt); continue; }
        fmt++;
        switch (*fmt) {
            case 'd': itoa(va_arg(ap, int), buf, 10); vga_puts(buf); break;
            case 'u': utoa(va_arg(ap, uint32_t), buf, 10); vga_puts(buf); break;
            case 'x': utoa(va_arg(ap, uint32_t), buf, 16); vga_puts(buf); break;
            case 's': vga_puts(va_arg(ap, const char*)); break;
            case 'c': vga_putc((char)va_arg(ap, int)); break;
            case '%': vga_putc('%'); break;
            default:  vga_putc('%'); vga_putc(*fmt); break;
        }
    }
    va_end(ap);
}

/* ---------- VGA standard text mode 03h reprogram ---------- */
void vga_set_text_mode_3(void) {
    outb(0x3C2, 0x67);

    static const uint8_t seq[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
    for (int i = 0; i < 5; i++) { outb(0x3C4, i); outb(0x3C5, seq[i]); }

    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);

    static const uint8_t crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,
        0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x50,
        0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF
    };
    for (int i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, crtc[i]); }

    static const uint8_t gc[9] = {
        0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF
    };
    for (int i = 0; i < 9; i++) { outb(0x3CE, i); outb(0x3CF, gc[i]); }

    inb(0x3DA);
    static const uint8_t ac[21] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
        0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
        0x0C,0x00,0x0F,0x08,0x00
    };
    for (int i = 0; i < 21; i++) { outb(0x3C0, i); outb(0x3C0, ac[i]); }
    outb(0x3C0, 0x20);
}
