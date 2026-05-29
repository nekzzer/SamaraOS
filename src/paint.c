/* SamaraOS Paint — freehand drawing in a WM window. */

#include "paint.h"
#include "wm.h"
#include "gfx.h"
#include "string.h"
#include "keyboard.h"

#define CANVAS_W 560
#define CANVAS_H 320

static uint32_t canvas[CANVAS_W * CANVAS_H];

static const uint32_t palette[10] = {
    RGB(0x10, 0x14, 0x1E),  /* near-black */
    RGB(0xFF, 0xFF, 0xFF),
    RGB(0xE0, 0x40, 0x40),
    RGB(0xF0, 0x90, 0x30),
    RGB(0xF0, 0xD0, 0x40),
    RGB(0x60, 0xC0, 0x60),
    RGB(0x40, 0xA0, 0xE0),
    RGB(0x70, 0x60, 0xE0),
    RGB(0xC0, 0x60, 0xC0),
    RGB(0x88, 0x60, 0x40),
};

typedef struct {
    int  color_idx;
    int  brush;             /* radius */
    int  last_x, last_y;    /* -1 == no previous */
    /* swatch + button hit rects (filled by paint) */
    int  swatch_x, swatch_y, swatch_sz;
    int  clear_x, clear_y, clear_w, clear_h;
    int  size_x, size_y, size_w, size_h;
    int  canvas_x, canvas_y;
    bool dirty_full;        /* full canvas needs blit (after clear) */
} paint_t;

static paint_t S;
static window_t* g_paint_win = NULL;

#define COL_BG     RGB(0x1A, 0x1D, 0x2C)
#define COL_PANEL  RGB(0x25, 0x29, 0x3C)
#define COL_FG     RGB(0xE0, 0xE6, 0xF0)
#define COL_DIM    RGB(0x88, 0x90, 0xB0)
#define COL_BORDER RGB(0x6E, 0xA8, 0xFE)
#define COL_BTN    RGB(0x33, 0x3C, 0x5C)

static void canvas_clear(void) {
    uint32_t c = RGB(0xF8, 0xF8, 0xF8);
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvas[i] = c;
}

static void paint_dot(int cx, int cy, int r, uint32_t col) {
    int x0 = cx - r, y0 = cy - r;
    int x1 = cx + r, y1 = cy + r;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= CANVAS_W) x1 = CANVAS_W - 1;
    if (y1 >= CANVAS_H) y1 = CANVAS_H - 1;
    int r2 = r * r;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx*dx + dy*dy <= r2)
                canvas[y * CANVAS_W + x] = col;
        }
}

/* Bresenham line, drawing brush dot at each point */
static void paint_line(int x0, int y0, int x1, int y1, int r, uint32_t col) {
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int safety = 0;
    while (1) {
        paint_dot(x0, y0, r, col);
        if (x0 == x1 && y0 == y1) break;
        if (++safety > 2000) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void pt_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_rect_fill(cx, cy, cw, ch, COL_BG);

    /* Tool palette (left) */
    int px = cx + 12, py = cy + 14, pw = 80, ph = ch - 28;
    gfx_rect_fill(px, py, pw, ph, COL_PANEL);
    gfx_rect(px, py, pw, ph, COL_BORDER);
    gfx_string(px + 8, py + 8, "Tools", COL_FG, COL_PANEL, false);

    int sz = 28;
    S.swatch_x = px + 14;
    S.swatch_y = py + 32;
    S.swatch_sz = sz;
    for (int i = 0; i < 10; i++) {
        int sx = S.swatch_x;
        int sy = S.swatch_y + i * (sz + 4);
        gfx_rect_fill(sx, sy, sz, sz, palette[i]);
        gfx_rect(sx, sy, sz, sz, i == S.color_idx ? RGB(0xFF, 0xFF, 0xFF) : COL_DIM);
        if (i == S.color_idx) gfx_rect(sx-2, sy-2, sz+4, sz+4, COL_BORDER);
    }

    /* Brush size selector */
    int by = S.swatch_y + 10 * (sz + 4) + 6;
    S.size_x = px + 8; S.size_y = by; S.size_w = pw - 16; S.size_h = 22;
    gfx_rect_fill(S.size_x, S.size_y, S.size_w, S.size_h, COL_BTN);
    gfx_rect(S.size_x, S.size_y, S.size_w, S.size_h, COL_BORDER);
    char sbuf[24] = "Brush: ";
    char tmp[6]; itoa(S.brush, tmp, 10); strcat(sbuf, tmp); strcat(sbuf, "px");
    int tw = (int)strlen(sbuf) * 8;
    gfx_string(S.size_x + (S.size_w - tw) / 2, S.size_y + 3, sbuf, COL_FG, COL_BTN, false);

    /* Clear button */
    S.clear_x = px + 8; S.clear_y = by + 28; S.clear_w = pw - 16; S.clear_h = 22;
    gfx_rect_fill(S.clear_x, S.clear_y, S.clear_w, S.clear_h, COL_BTN);
    gfx_rect(S.clear_x, S.clear_y, S.clear_w, S.clear_h, COL_BORDER);
    gfx_string(S.clear_x + (S.clear_w - 5*8) / 2, S.clear_y + 3, "Clear", COL_FG, COL_BTN, false);

    /* Canvas area */
    int cx0 = px + pw + 14;
    int cy0 = cy + 14;
    int caw = cw - (cx0 - cx) - 14;
    int cah = ch - 28;
    if (caw > CANVAS_W) caw = CANVAS_W;
    if (cah > CANVAS_H) cah = CANVAS_H;
    S.canvas_x = cx0;
    S.canvas_y = cy0;
    gfx_rect(cx0 - 2, cy0 - 2, caw + 4, cah + 4, COL_BORDER);

    /* Blit canvas (fast path on 32bpp) */
    gfx_blit_argb(cx0, cy0, CANVAS_W, CANVAS_H, canvas);
    (void)caw; (void)cah;
    S.dirty_full = false;

    /* Hint */
    const char* hint = "Drag to draw  |  Esc closes window";
    gfx_string(cx0, cy0 + cah + 4, hint, COL_DIM, COL_BG, false);
}

static void pt_click(window_t* w, int rx, int ry) {
    (void)w;
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    int ax = rx + cx, ay = ry + cy;

    /* Palette */
    for (int i = 0; i < 10; i++) {
        int sx = S.swatch_x, sy = S.swatch_y + i * (S.swatch_sz + 4);
        if (ax >= sx && ax < sx + S.swatch_sz && ay >= sy && ay < sy + S.swatch_sz) {
            S.color_idx = i;
            S.last_x = -1;
            return;
        }
    }
    /* Clear */
    if (ax >= S.clear_x && ax < S.clear_x + S.clear_w &&
        ay >= S.clear_y && ay < S.clear_y + S.clear_h) {
        canvas_clear();
        S.last_x = -1;
        S.dirty_full = true;
        return;
    }
    /* Brush size cycle */
    if (ax >= S.size_x && ax < S.size_x + S.size_w &&
        ay >= S.size_y && ay < S.size_y + S.size_h) {
        S.brush = (S.brush % 12) + 1;
        S.last_x = -1;
        return;
    }
    /* Canvas */
    int caw = cw - (S.canvas_x - cx) - 14;
    int cah = ch - 28;
    if (caw > CANVAS_W) caw = CANVAS_W;
    if (cah > CANVAS_H) cah = CANVAS_H;
    if (ax >= S.canvas_x && ax < S.canvas_x + caw &&
        ay >= S.canvas_y && ay < S.canvas_y + cah) {
        int x = ax - S.canvas_x, y = ay - S.canvas_y;
        uint32_t col = palette[S.color_idx];
        if (S.last_x >= 0) paint_line(S.last_x, S.last_y, x, y, S.brush, col);
        else               paint_dot(x, y, S.brush, col);
        S.last_x = x; S.last_y = y;
    } else {
        S.last_x = -1;
    }
}

static void pt_key(window_t* w, char c) {
    if (c == 0x1B) { wm_close(w); return; }
    if (c >= '1' && c <= '9') { S.color_idx = c - '1'; }
    if (c == '0') { S.color_idx = 9; }
    if (c == '+' || c == '=') { if (S.brush < 12) S.brush++; }
    if (c == '-' || c == '_') { if (S.brush > 1)  S.brush--; }
    if (c == 'c' || c == 'C') { canvas_clear(); S.dirty_full = true; }
}

int paint_open(void) {
    if (!gfx_ready()) return -1;
    if (g_paint_win && g_paint_win->open) return 0;

    canvas_clear();
    S.color_idx = 0;
    S.brush = 3;
    S.last_x = -1; S.last_y = -1;
    S.dirty_full = true;

    int W = gfx_w(), H = gfx_h();
    int ww = 720, wh = 400;
    if (ww > W - 40) ww = W - 40;
    if (wh > H - 80) wh = H - 80;
    int wx = (W - ww) / 2;
    int wy = (H - wh) / 2 - 20;
    if (wy < 30) wy = 30;

    g_paint_win = wm_open_app_ex(wx, wy, ww, wh, "Paint - SamaraOS",
                                  pt_paint, pt_key, pt_click, NULL,
                                  false, &S);
    return g_paint_win ? 0 : -1;
}
