#include "gfx/gfx_term.h"
#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "core/string.h"

static int term_x_px, term_y_px;
static int cx, cy;
static uint8_t cur_attr = 0x07;       /* lgrey on black, like VGA text */

typedef struct { char c; uint8_t attr; } cell_t;
static cell_t grid[TERM_COLS * TERM_ROWS];

/* 16-colour terminal palette indexed by VGA attribute nibbles. Same hue slots
   as CGA so shell colour choices keep their meaning, but tuned to sit on the
   near-black terminal background without the neon of the original. */
static const uint32_t color16[16] = {
    RGB(0x11,0x12,0x15),   /* black    */
    RGB(0x3E,0x6B,0xA8),   /* blue     */
    RGB(0x5F,0x93,0x4A),   /* green    */
    RGB(0x3F,0x8E,0x8A),   /* cyan     */
    RGB(0xB0,0x4A,0x40),   /* red      */
    RGB(0x8E,0x5B,0x96),   /* magenta  */
    RGB(0xB0,0x80,0x34),   /* brown    */
    RGB(0xC4,0xC0,0xB8),   /* lgrey    */
    RGB(0x5C,0x5A,0x56),   /* dgrey    */
    RGB(0x72,0x9D,0xDB),   /* lblue    */
    RGB(0x98,0xC3,0x72),   /* lgreen   */
    RGB(0x7C,0xC2,0xBA),   /* lcyan    */
    RGB(0xE0,0x76,0x69),   /* lred     */
    RGB(0xC2,0x8C,0xC8),   /* lmagenta */
    RGB(0xE8,0xBE,0x5C),   /* yellow   */
    RGB(0xF2,0xEF,0xE9),   /* white    */
};

static void (*draw_hook)(int x, int y, int w, int h);

void gfx_term_set_draw_hook(void (*hook)(int x, int y, int w, int h)) { draw_hook = hook; }

static void notify(int x, int y, int w, int h) {
    if (draw_hook) draw_hook(x, y, w, h);
}

uint32_t gfx_term_color(int idx) { return color16[idx & 0x0F]; }

static void render_cell(int x, int y) {
    cell_t* p = &grid[y * TERM_COLS + x];
    uint32_t fg = color16[p->attr & 0x0F];
    uint32_t bg = color16[(p->attr >> 4) & 0x0F];
    uif_mono_cell(term_x_px + x * 8, term_y_px + y * 16, (uint8_t)p->c, fg, bg);
}

static void render_all(void) {
    for (int y = 0; y < TERM_ROWS; y++)
        for (int x = 0; x < TERM_COLS; x++)
            render_cell(x, y);
}

static void notify_all(void) { notify(term_x_px, term_y_px, TERM_COLS * 8, TERM_ROWS * 16); }
static void notify_cell(int x, int y) { notify(term_x_px + x * 8, term_y_px + y * 16, 8, 16); }

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
    notify_all();
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
            notify_cell(cx, cy);
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
            notify_cell(cx, cy);
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
    notify_all();
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

void gfx_term_erase(int x0, int y, int x1) {
    if (y < 0 || y >= TERM_ROWS) return;
    if (x0 < 0) x0 = 0;
    if (x1 > TERM_COLS) x1 = TERM_COLS;
    if (x0 >= x1) return;
    for (int x = x0; x < x1; x++) {
        grid[y * TERM_COLS + x].c = ' ';
        grid[y * TERM_COLS + x].attr = cur_attr;
        render_cell(x, y);
    }
    notify(term_x_px + x0 * 8, term_y_px + y * 16, (x1 - x0) * 8, 16);
}

/* Scroll rows [top, bot] by n lines (n > 0: content moves up). */
void gfx_term_scroll_region(int top, int bot, int n) {
    if (top < 0) top = 0;
    if (bot >= TERM_ROWS) bot = TERM_ROWS - 1;
    if (top > bot || n == 0) return;
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n < -h) n = -h;
    if (n > 0) {
        for (int y = top; y <= bot - n; y++)
            memcpy(&grid[y * TERM_COLS], &grid[(y + n) * TERM_COLS], TERM_COLS * sizeof(cell_t));
        for (int y = bot - n + 1; y <= bot; y++)
            for (int x = 0; x < TERM_COLS; x++) { grid[y * TERM_COLS + x].c = ' '; grid[y * TERM_COLS + x].attr = cur_attr; }
    } else {
        n = -n;
        for (int y = bot; y >= top + n; y--)
            memcpy(&grid[y * TERM_COLS], &grid[(y - n) * TERM_COLS], TERM_COLS * sizeof(cell_t));
        for (int y = top; y < top + n; y++)
            for (int x = 0; x < TERM_COLS; x++) { grid[y * TERM_COLS + x].c = ' '; grid[y * TERM_COLS + x].attr = cur_attr; }
    }
    for (int y = top; y <= bot; y++)
        for (int x = 0; x < TERM_COLS; x++) render_cell(x, y);
    notify(term_x_px, term_y_px + top * 16, TERM_COLS * 8, h * 16);
}

/* On row y from column x: n > 0 inserts n blanks, n < 0 deletes -n cells. */
void gfx_term_shift_chars(int x, int y, int n) {
    if (y < 0 || y >= TERM_ROWS || x < 0 || x >= TERM_COLS || n == 0) return;
    cell_t* row = &grid[y * TERM_COLS];
    int span = TERM_COLS - x;
    int k = n > 0 ? n : -n;
    if (k > span) k = span;
    if (n > 0) {
        memmove(row + x + k, row + x, (uint32_t)(span - k) * sizeof(cell_t));
        for (int i = 0; i < k; i++) { row[x + i].c = ' '; row[x + i].attr = cur_attr; }
    } else {
        memmove(row + x, row + x + k, (uint32_t)(span - k) * sizeof(cell_t));
        for (int i = TERM_COLS - k; i < TERM_COLS; i++) { row[i].c = ' '; row[i].attr = cur_attr; }
    }
    for (int i = x; i < TERM_COLS; i++) render_cell(i, y);
    notify(term_x_px + x * 8, term_y_px + y * 16, span * 8, 16);
}
