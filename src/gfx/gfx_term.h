#ifndef SAMARA_GFX_TERM_H
#define SAMARA_GFX_TERM_H
#include "core/types.h"

/* Default grid; the desktop terminal can be resized up to the max. */
#define TERM_COLS 80
#define TERM_ROWS 25
#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 128
#define TERM_SB_LINES 1000       /* scrollback lines */

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

void gfx_term_set_rgb(uint32_t fg, uint32_t bg);   /* 0xRRGGBB true colour */
void gfx_term_set_attr(uint8_t at);                /* TF_BOLD | TF_UNDERLINE | ... */
void gfx_term_putu(uint32_t cp);                   /* one Unicode code point */
void gfx_term_show_cursor(bool on);                /* draw the text cursor (console) */
void gfx_term_cursor_visible(bool on);             /* DECTCEM */
void gfx_term_alt(bool on);                        /* alternate screen */
int  gfx_term_cell_w(void);                        /* current cell size in pixels */
int  gfx_term_cell_h(void);
int  gfx_term_cols(void);
int  gfx_term_rows(void);
/* New grid size; returns how many lines the content moved up (into the
   scrollback) to keep the cursor row visible. */
int  gfx_term_resize(int cols, int rows);
/* Scrollback view: lines > 0 moves back in history, < 0 towards live. */
void gfx_term_view_scroll(int lines);
bool gfx_term_scrolled(void);

/* Called with the pixel rect of every cell range the terminal just drew
   outside of gfx_term_repos, so a compositor can present or damage it. */
void gfx_term_set_draw_hook(void (*hook)(int x, int y, int w, int h));
uint32_t gfx_term_color(int idx);

#endif
