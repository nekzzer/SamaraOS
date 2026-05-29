#include "vga.h"
#include "io.h"
#include "string.h"
#include "gfx_term.h"
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
        VRAM[cur_y * VGA_WIDTH + cur_x] = cell(c, cur_color);
        cur_x++;
        if (cur_x >= VGA_WIDTH) { cur_x = 0; cur_y++; }
    }
    scroll_if_needed();
    vga_update_hw_cursor();
}

static void text_get_cursor(int* x, int* y) { *x = cur_x; *y = cur_y; }
static void text_set_cursor(int x, int y) { cur_x = x; cur_y = y; vga_update_hw_cursor(); }

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
    uint16_t pos = (uint16_t)(cur_y * VGA_WIDTH + cur_x);
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

void vga_init(void) {
    mode = CM_TEXT;
    text_set_color(VGA_LGREY, VGA_BLACK);
    text_clear();
}

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
