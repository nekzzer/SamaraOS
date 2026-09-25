#ifndef SAMARA_VGA_H
#define SAMARA_VGA_H
#include "core/types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LGREY,
    VGA_DGREY, VGA_LBLUE, VGA_LGREEN, VGA_LCYAN,
    VGA_LRED, VGA_LMAGENTA, VGA_YELLOW, VGA_WHITE
};

void vga_init(void);
void vga_set_text_mode_3(void);   /* program VGA back to standard 80x25 text mode */
void vga_use_text(void);          /* route subsequent vga_* output to VGA text VRAM */
void vga_use_gfx_term(int px, int py); /* route output to gfx_term grid at framebuffer (px,py) */
void vga_clear(void);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_putc(char c);
void vga_puts(const char* s);
void vga_putn(int v);
void vga_putx(uint32_t v);
void vga_putcell_at(int x, int y, char c, uint8_t color);
uint16_t vga_get_cell(int x, int y);
void vga_set_cell(int x, int y, uint16_t cell);
void vga_get_cursor(int* x, int* y);
void vga_set_cursor(int x, int y);
void vga_update_hw_cursor(void);
void vga_erase(int x0, int y, int x1);
void vga_scroll_region(int top, int bot, int n);   /* n > 0: scroll up */
void vga_shift_chars(int x, int y, int n);         /* n > 0: insert blanks, < 0: delete */
void vga_printf(const char* fmt, ...);

#endif
