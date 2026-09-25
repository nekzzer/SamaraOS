#ifndef SAMARA_GFX_TERM_H
#define SAMARA_GFX_TERM_H
#include "core/types.h"

#define TERM_COLS 80
#define TERM_ROWS 25

void gfx_term_init(int x_pixel, int y_pixel);
void gfx_term_repos(int x_pixel, int y_pixel);    /* move grid + redraw at new pos */
void gfx_term_putc(char c);
void gfx_term_puts(const char* s);
void gfx_term_clear(void);
void gfx_term_set_color(uint8_t fg, uint8_t bg);
void gfx_term_get_cursor(int* x, int* y);
void gfx_term_set_cursor(int x, int y);
void gfx_term_erase(int x0, int y, int x1);   /* blank cells, cursor stays */
void gfx_term_scroll_region(int top, int bot, int n);
void gfx_term_shift_chars(int x, int y, int n);

/* Called with the pixel rect of every cell range the terminal just drew
   outside of gfx_term_repos, so a compositor can present or damage it. */
void gfx_term_set_draw_hook(void (*hook)(int x, int y, int w, int h));
uint32_t gfx_term_color(int idx);

#endif
