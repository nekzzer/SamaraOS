#include "gfx/gfx_term.h"
#include "gfx/gfx.h"
#include "gfx/termfont.h"
#include "core/heap.h"
#include "core/string.h"

/* Character-cell terminal drawn into the framebuffer: the desktop Terminal
   and the full-screen graphical console. Cells hold Unicode code points,
   24-bit colours and attributes (bold, underline, ...); glyphs come from
   gfx/termfont (several cell sizes). The grid is resizable up to
   TERM_MAX_COLS x TERM_MAX_ROWS; lines that scroll off the top go to a
   scrollback ring the view can be moved into (mouse wheel). Full-screen
   programs get an alternate screen (?1049h) that keeps the shell's. */

static int term_x_px, term_y_px;
static int cols = TERM_COLS, rows = TERM_ROWS;
static int cx, cy;
static uint32_t cur_fg, cur_bg;        /* RGB: palette or any 24-bit colour (SGR 38;2) */
static uint8_t  cur_at;                /* TF_BOLD | TF_UNDERLINE | ... */

typedef struct { uint32_t cp; uint32_t fg, bg; uint8_t at; } cell_t;
static cell_t grid[TERM_MAX_COLS * TERM_MAX_ROWS];     /* row stride = TERM_MAX_COLS */
#define CELL(x, y) grid[(y) * TERM_MAX_COLS + (x)]
#define CW termfont_cw()
#define CH termfont_ch()

/* Scrollback ring: line i (0 = oldest) lives at sb[(sb_head - sb_count + i) mod N]. */
static cell_t* sb;
static int sb_head, sb_count;
static int view_off;                   /* lines scrolled back, 0 = live */

/* text cursor drawn by the terminal itself (the graphical console; the
   desktop draws its own caret) */
static bool cursor_mode, cursor_hidden;
static int  drawn_cx = -1, drawn_cy = -1;

/* alternate screen */
static cell_t* alt_saved;
static int  alt_rows, alt_cx, alt_cy;
static bool alt_on;

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

int gfx_term_cols(void) { return cols; }
int gfx_term_rows(void) { return rows; }
int gfx_term_cell_w(void) { return CW; }
int gfx_term_cell_h(void) { return CH; }

static void blank(cell_t* c) { c->cp = ' '; c->fg = cur_fg; c->bg = cur_bg; c->at = 0; }

/* ---------------- scrollback ---------------- */

static cell_t* sb_line(int i) {            /* 0 = oldest */
    int idx = (sb_head - sb_count + i + TERM_SB_LINES) % TERM_SB_LINES;
    return sb + (size_t)idx * TERM_MAX_COLS;
}

static void sb_push(const cell_t* row) {
    if (alt_on) return;                    /* full-screen programs leave no history */
    if (!sb) {
        sb = (cell_t*)kmalloc((size_t)TERM_SB_LINES * TERM_MAX_COLS * sizeof(cell_t));
        if (!sb) return;
    }
    cell_t* dst = sb + (size_t)sb_head * TERM_MAX_COLS;
    memcpy(dst, row, (size_t)cols * sizeof(cell_t));
    for (int x = cols; x < TERM_MAX_COLS; x++) { dst[x].cp = ' '; dst[x].fg = color16[7]; dst[x].bg = color16[0]; dst[x].at = 0; }
    sb_head = (sb_head + 1) % TERM_SB_LINES;
    if (sb_count < TERM_SB_LINES) sb_count++;
}

/* The cell shown at screen row y, honouring the scrollback view. */
static const cell_t* shown(int x, int y) {
    int line = y - view_off;                 /* < 0: from scrollback */
    if (line >= 0) return &CELL(x, line);
    return &sb_line(sb_count + line)[x];
}

/* ---------------- rendering ---------------- */

static bool cursor_at(int x, int y) {
    if (!cursor_mode || cursor_hidden || view_off) return false;
    int px = cx >= cols ? cols - 1 : cx;
    return x == px && y == cy;
}

static void render_cell(int x, int y) {
    const cell_t* p = shown(x, y);
    termfont_cell(term_x_px + x * CW, term_y_px + y * CH, p->cp, p->fg, p->bg,
                  (uint8_t)(p->at | (cursor_at(x, y) ? TF_CURSOR : 0)));
}

static void render_all(void) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            render_cell(x, y);
    drawn_cx = cx >= cols ? cols - 1 : cx;
    drawn_cy = cy;
}

static void notify_all(void) { notify(term_x_px, term_y_px, cols * CW, rows * CH); }
static void notify_cell(int x, int y) { notify(term_x_px + x * CW, term_y_px + y * CH, CW, CH); }

/* Move the drawn cursor to where the cursor is now. */
static void cursor_sync(void) {
    if (!cursor_mode) return;
    int px = cx >= cols ? cols - 1 : cx;
    if (px == drawn_cx && cy == drawn_cy) return;
    int ox = drawn_cx, oy = drawn_cy;
    drawn_cx = px; drawn_cy = cy;
    if (ox >= 0 && oy >= 0 && ox < cols && oy < rows) { render_cell(ox, oy); notify_cell(ox, oy); }
    if (cy < rows) { render_cell(px, cy); notify_cell(px, cy); }
}

void gfx_term_show_cursor(bool on) {        /* the terminal draws its own cursor */
    cursor_mode = on;
    drawn_cx = drawn_cy = -1;
    if (cy < rows) { int px = cx >= cols ? cols - 1 : cx; render_cell(px, cy); notify_cell(px, cy); }
    cursor_sync();
}

void gfx_term_cursor_visible(bool on) {     /* DECTCEM (?25h / ?25l) */
    if (cursor_hidden == !on) return;
    cursor_hidden = !on;
    if (cursor_mode && cy < rows) {
        int px = cx >= cols ? cols - 1 : cx;
        render_cell(px, cy); notify_cell(px, cy);
    }
}

/* New output: leave the scrollback view first. */
static void snap(void) {
    if (!view_off) return;
    view_off = 0;
    render_all();
    notify_all();
}

bool gfx_term_scrolled(void) { return view_off != 0; }

void gfx_term_view_scroll(int lines) {
    int v = view_off + lines;
    if (v < 0) v = 0;
    if (v > sb_count) v = sb_count;
    if (alt_on) v = 0;
    if (v == view_off) return;
    view_off = v;
    render_all();
    notify_all();
}

static void blank_row(int y) {
    for (int x = 0; x < TERM_MAX_COLS; x++) blank(&CELL(x, y));
}

void gfx_term_init(int x_pixel, int y_pixel) {
    term_x_px = x_pixel;
    term_y_px = y_pixel;
    cx = 0; cy = 0;
    cur_fg = color16[7];
    cur_bg = color16[0];
    cur_at = 0;
    for (int y = 0; y < TERM_MAX_ROWS; y++) blank_row(y);
    sb_count = sb_head = 0;
    view_off = 0;
    cursor_hidden = false;
    drawn_cx = drawn_cy = -1;
    if (alt_saved) { kfree(alt_saved); alt_saved = NULL; }
    alt_on = false;
    gfx_rect_fill(term_x_px, term_y_px, cols * CW, rows * CH, color16[0]);
}

void gfx_term_repos(int x_pixel, int y_pixel) {
    term_x_px = x_pixel;
    term_y_px = y_pixel;
    render_all();
}

/* Change the grid size. Content stays anchored top-left; if rows shrink
   below the cursor, the top lines move into the scrollback so the cursor
   line stays on screen. Returns how many lines the content moved up. */
int gfx_term_resize(int ncols, int nrows) {
    if (ncols < 8) ncols = 8;
    if (nrows < 2) nrows = 2;
    if (ncols > TERM_MAX_COLS) ncols = TERM_MAX_COLS;
    if (nrows > TERM_MAX_ROWS) nrows = TERM_MAX_ROWS;
    if (ncols == cols && nrows == rows) return 0;
    view_off = 0;
    int shift = cy - (nrows - 1);
    if (shift < 0) shift = 0;
    for (int i = 0; i < shift; i++) sb_push(&CELL(0, i));
    if (shift) {
        for (int y = 0; y + shift < TERM_MAX_ROWS; y++)
            memcpy(&CELL(0, y), &CELL(0, y + shift), TERM_MAX_COLS * sizeof(cell_t));
        for (int y = TERM_MAX_ROWS - shift; y < TERM_MAX_ROWS; y++) blank_row(y);
    }
    /* Whatever falls outside the new size is dropped, so growing again
       later shows blanks rather than stale text. */
    for (int y = 0; y < TERM_MAX_ROWS; y++)
        for (int x = (y < nrows ? ncols : 0); x < TERM_MAX_COLS; x++) blank(&CELL(x, y));
    cols = ncols;
    rows = nrows;
    cy -= shift;
    if (cx > cols) cx = cols;
    if (cy >= rows) cy = rows - 1;
    drawn_cx = drawn_cy = -1;
    return shift;
}

static void scroll(void) {
    sb_push(&CELL(0, 0));
    for (int y = 0; y < rows - 1; y++)
        memcpy(&CELL(0, y), &CELL(0, y + 1), (size_t)cols * sizeof(cell_t));
    for (int x = 0; x < cols; x++) blank(&CELL(x, rows - 1));
    cy = rows - 1;
    render_all();
    notify_all();
}

void gfx_term_putu(uint32_t c) {
    snap();
    if (c == '\n') {
        cx = 0; cy++;
    } else if (c == '\r') {
        cx = 0;
    } else if (c == '\b') {
        if (cx > 0) {
            cx--;
            blank(&CELL(cx, cy));
            render_cell(cx, cy);
            notify_cell(cx, cy);
        }
    } else if (c == '\t') {
        cx = (cx + 8) & ~7;
        if (cx >= cols) { cx = 0; cy++; }
    } else if (c >= 0x20 && c != 0x7F) {
        if (cx >= cols) { cx = 0; cy++; }
        if (cy >= rows) scroll();
        cell_t* p = &CELL(cx, cy);
        p->cp = c; p->fg = cur_fg; p->bg = cur_bg; p->at = cur_at;
        cx++;
        render_cell(cx - 1, cy);
        notify_cell(cx - 1, cy);
    }
    if (cy >= rows) scroll();
    cursor_sync();
}

/* 8-bit console text is CP866 (the keyboard and the SamaraOS shell use it) */
void gfx_term_putc(char c) {
    uint8_t u = (uint8_t)c;
    gfx_term_putu(u < 0x80 ? u : cp866_to_uni(u));
}

void gfx_term_puts(const char* s) { while (*s) gfx_term_putc(*s++); }

void gfx_term_clear(void) {
    view_off = 0;
    for (int y = 0; y < TERM_MAX_ROWS; y++) blank_row(y);
    cx = 0; cy = 0;
    gfx_rect_fill(term_x_px, term_y_px, cols * CW, rows * CH, cur_bg);
    drawn_cx = drawn_cy = -1;
    cursor_sync();
    notify_all();
}

void gfx_term_set_color(uint8_t fg, uint8_t bg) {
    cur_fg = color16[fg & 0x0F];
    cur_bg = color16[bg & 0x0F];
}

/* Any 24-bit colours (0xRRGGBB), for SGR 38;2 / 48;2 and 256-colour codes. */
void gfx_term_set_rgb(uint32_t fg, uint32_t bg) {
    cur_fg = fg & 0xFFFFFF;
    cur_bg = bg & 0xFFFFFF;
}

void gfx_term_set_attr(uint8_t at) { cur_at = at & ~TF_CURSOR; }

void gfx_term_get_cursor(int* x, int* y) { *x = cx; *y = cy; }
void gfx_term_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cols) x = cols - 1;
    if (y >= rows) y = rows - 1;
    cx = x; cy = y;
    cursor_sync();
}

void gfx_term_erase(int x0, int y, int x1) {
    if (y < 0 || y >= rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 > cols) x1 = cols;
    if (x0 >= x1) return;
    snap();
    for (int x = x0; x < x1; x++) {
        blank(&CELL(x, y));
        render_cell(x, y);
    }
    notify(term_x_px + x0 * CW, term_y_px + y * CH, (x1 - x0) * CW, CH);
}

/* Scroll rows [top, bot] by n lines (n > 0: content moves up). */
void gfx_term_scroll_region(int top, int bot, int n) {
    if (top < 0) top = 0;
    if (bot >= rows) bot = rows - 1;
    if (top > bot || n == 0) return;
    snap();
    int h = bot - top + 1;
    if (n > h) n = h;
    if (n < -h) n = -h;
    size_t rb = (size_t)cols * sizeof(cell_t);
    if (n > 0) {
        for (int y = top; y <= bot - n; y++) memcpy(&CELL(0, y), &CELL(0, y + n), rb);
        for (int y = bot - n + 1; y <= bot; y++)
            for (int x = 0; x < cols; x++) blank(&CELL(x, y));
    } else {
        n = -n;
        for (int y = bot; y >= top + n; y--) memcpy(&CELL(0, y), &CELL(0, y - n), rb);
        for (int y = top; y < top + n; y++)
            for (int x = 0; x < cols; x++) blank(&CELL(x, y));
    }
    for (int y = top; y <= bot; y++)
        for (int x = 0; x < cols; x++) render_cell(x, y);
    notify(term_x_px, term_y_px + top * CH, cols * CW, h * CH);
}

/* On row y from column x: n > 0 inserts n blanks, n < 0 deletes -n cells. */
void gfx_term_shift_chars(int x, int y, int n) {
    if (y < 0 || y >= rows || x < 0 || x >= cols || n == 0) return;
    snap();
    cell_t* row = &CELL(0, y);
    int span = cols - x;
    int k = n > 0 ? n : -n;
    if (k > span) k = span;
    if (n > 0) {
        memmove(row + x + k, row + x, (uint32_t)(span - k) * sizeof(cell_t));
        for (int i = 0; i < k; i++) blank(&row[x + i]);
    } else {
        memmove(row + x, row + x + k, (uint32_t)(span - k) * sizeof(cell_t));
        for (int i = cols - k; i < cols; i++) blank(&row[i]);
    }
    for (int i = x; i < cols; i++) render_cell(i, y);
    notify(term_x_px + x * CW, term_y_px + y * CH, span * CW, CH);
}

/* ---------------- alternate screen (?1049h / ?1049l) ---------------- */

void gfx_term_alt(bool on) {
    if (on == alt_on) return;
    if (on) {
        alt_saved = (cell_t*)kmalloc((size_t)rows * TERM_MAX_COLS * sizeof(cell_t));
        if (!alt_saved) return;
        memcpy(alt_saved, grid, (size_t)rows * TERM_MAX_COLS * sizeof(cell_t));
        alt_rows = rows; alt_cx = cx; alt_cy = cy;
        alt_on = true;
        view_off = 0;
        for (int y = 0; y < TERM_MAX_ROWS; y++) blank_row(y);
        cx = cy = 0;
    } else {
        alt_on = false;
        if (alt_saved) {
            int n = alt_rows < rows ? alt_rows : rows;
            for (int y = 0; y < TERM_MAX_ROWS; y++) blank_row(y);
            memcpy(grid, alt_saved, (size_t)n * TERM_MAX_COLS * sizeof(cell_t));
            kfree(alt_saved);
            alt_saved = NULL;
        }
        cx = alt_cx; cy = alt_cy < rows ? alt_cy : rows - 1;
    }
    render_all();
    notify_all();
}
