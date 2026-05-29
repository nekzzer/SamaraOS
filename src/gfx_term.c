#include "gfx_term.h"
#include "gfx.h"
#include "string.h"

static int term_x_px, term_y_px;
static int cx, cy;
static uint8_t cur_attr = 0x07;       /* lgrey on black, like VGA text */

typedef struct { char c; uint8_t attr; } cell_t;
static cell_t grid[TERM_COLS * TERM_ROWS];

/* CGA-style 16 colour palette indexed by VGA attribute nibbles. */
static const uint32_t color16[16] = {
    RGB(0x00,0x00,0x00),
    RGB(0x00,0x00,0xAA),
    RGB(0x00,0xAA,0x00),
    RGB(0x00,0xAA,0xAA),
    RGB(0xAA,0x00,0x00),
    RGB(0xAA,0x00,0xAA),
    RGB(0xAA,0x55,0x00),
    RGB(0xAA,0xAA,0xAA),
    RGB(0x55,0x55,0x55),
    RGB(0x55,0x55,0xFF),
    RGB(0x55,0xFF,0x55),
    RGB(0x55,0xFF,0xFF),
    RGB(0xFF,0x55,0x55),
    RGB(0xFF,0x55,0xFF),
    RGB(0xFF,0xFF,0x55),
    RGB(0xFF,0xFF,0xFF),
};

static void render_cell(int x, int y) {
    cell_t* p = &grid[y * TERM_COLS + x];
    uint32_t fg = color16[p->attr & 0x0F];
    uint32_t bg = color16[(p->attr >> 4) & 0x0F];
    gfx_glyph(term_x_px + x * 8, term_y_px + y * 16, p->c, fg, bg, true);
}

static void render_all(void) {
    for (int y = 0; y < TERM_ROWS; y++)
        for (int x = 0; x < TERM_COLS; x++)
            render_cell(x, y);
}

void gfx_term_init(int x_pixel, int y_pixel) {
    term_x_px = x_pixel;
    term_y_px = y_pixel;
    cx = 0; cy = 0;
    cur_attr = 0x07;
    for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
        grid[i].c = ' ';
        grid[i].attr = cur_attr;
    }
    gfx_rect_fill(term_x_px, term_y_px, TERM_COLS * 8, TERM_ROWS * 16, color16[0]);
}

void gfx_term_repos(int x_pixel, int y_pixel) {
    term_x_px = x_pixel;
    term_y_px = y_pixel;
    gfx_rect_fill(term_x_px, term_y_px, TERM_COLS * 8, TERM_ROWS * 16,
                  color16[(cur_attr >> 4) & 0x0F]);
    render_all();
}

static void scroll(void) {
    for (int y = 0; y < TERM_ROWS - 1; y++)
        for (int x = 0; x < TERM_COLS; x++)
            grid[y * TERM_COLS + x] = grid[(y + 1) * TERM_COLS + x];
    for (int x = 0; x < TERM_COLS; x++) {
        grid[(TERM_ROWS - 1) * TERM_COLS + x].c = ' ';
        grid[(TERM_ROWS - 1) * TERM_COLS + x].attr = cur_attr;
    }
    cy = TERM_ROWS - 1;
    render_all();
}

void gfx_term_putc(char c) {
    if (c == '\n') {
        cx = 0; cy++;
    } else if (c == '\r') {
        cx = 0;
    } else if (c == '\b') {
        if (cx > 0) {
            cx--;
            grid[cy * TERM_COLS + cx].c = ' ';
            grid[cy * TERM_COLS + cx].attr = cur_attr;
            render_cell(cx, cy);
        }
    } else if (c == '\t') {
        cx = (cx + 8) & ~7;
        if (cx >= TERM_COLS) { cx = 0; cy++; }
    } else if ((uint8_t)c >= 0x20) {
        if (cx >= TERM_COLS) { cx = 0; cy++; }
        if (cy < TERM_ROWS) {
            grid[cy * TERM_COLS + cx].c = c;
            grid[cy * TERM_COLS + cx].attr = cur_attr;
            render_cell(cx, cy);
            cx++;
        }
    }
    if (cy >= TERM_ROWS) scroll();
}

void gfx_term_puts(const char* s) { while (*s) gfx_term_putc(*s++); }

void gfx_term_clear(void) {
    for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
        grid[i].c = ' ';
        grid[i].attr = cur_attr;
    }
    cx = 0; cy = 0;
    gfx_rect_fill(term_x_px, term_y_px, TERM_COLS * 8, TERM_ROWS * 16,
                  color16[(cur_attr >> 4) & 0x0F]);
}

void gfx_term_set_color(uint8_t fg, uint8_t bg) {
    cur_attr = (uint8_t)(((bg & 0x0F) << 4) | (fg & 0x0F));
}

void gfx_term_get_cursor(int* x, int* y) { *x = cx; *y = cy; }
void gfx_term_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= TERM_COLS) x = TERM_COLS - 1;
    if (y >= TERM_ROWS) y = TERM_ROWS - 1;
    cx = x; cy = y;
}
