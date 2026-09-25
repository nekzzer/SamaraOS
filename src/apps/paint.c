/* SamaraOS Paint — drawing app in the desktop's paper-and-amber style.

   Tools: brush, eraser, line, rectangle, ellipse, fill, colour picker.
   One-level undo/redo (two canvases swapped), BMP export to /mnt (or home).
   The canvas survives closing the window. */

#include "apps/paint.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "core/heap.h"
#include "core/string.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "boot/pit.h"
#include "fs/fs.h"

/* ---- layout ---- */
#define TB_H     52      /* toolbar */
#define SB_H     26      /* status bar */
#define BTN      36      /* square tool button */
#define GAP      4
#define PAD      12
#define SW       16      /* palette swatch */
#define SW_GAP   3
#define MAX_BRUSH 40

#define PAPER    RGB(0xFF, 0xFF, 0xFF)

typedef enum { T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICK, T_COUNT } tool_t;

static const char* const tool_name[T_COUNT] = {
    "Brush", "Eraser", "Line", "Rectangle", "Ellipse", "Fill", "Picker"
};
static const char tool_key[T_COUNT] = { 'b', 'e', 'l', 'r', 'o', 'f', 'i' };

#define N_COLORS 16
static const uint32_t palette[N_COLORS] = {
    RGB(0x1D, 0x1E, 0x21), RGB(0x8C, 0x88, 0x80), RGB(0xD4, 0xCF, 0xC6), RGB(0xFF, 0xFF, 0xFF),
    RGB(0xCF, 0x4A, 0x3E), RGB(0xE3, 0x70, 0x2F), RGB(0xE3, 0x9B, 0x32), RGB(0xF2, 0xD0, 0x4A),
    RGB(0x74, 0xB0, 0x5E), RGB(0x2F, 0x7D, 0x4F), RGB(0x2F, 0xA3, 0xA0), RGB(0x3B, 0x7D, 0xD8),
    RGB(0x25, 0x3A, 0x73), RGB(0x7A, 0x4F, 0xB5), RGB(0xD8, 0x67, 0x9E), RGB(0x8A, 0x5A, 0x3B),
};

typedef struct { int x, y, w, h; } box_t;

enum { A_UNDO, A_CLEAR, A_SAVE, A_COUNT };
static const char* const action_label[A_COUNT] = { "Undo", "Clear", "Save" };

static struct {
    uint32_t* canvas;
    uint32_t* undo;              /* previous state; swapped for redo */
    int cw, ch;                  /* canvas size */
    bool can_undo;

    tool_t tool;
    uint32_t color;
    int brush;                   /* radius in px */

    bool pressing;               /* mouse held since a press in our client */
    bool stroking;               /* ... and that press started on the canvas */
    int sx, sy, lx, ly;          /* stroke start / last point (canvas coords) */

    int hover_x, hover_y;        /* canvas coords under the mouse, -1 = outside */
    char msg[64];
    uint32_t msg_until;

    /* hit boxes, relative to the client origin (filled by paint) */
    box_t tool_box[T_COUNT], minus, plus, cur_swatch, swatch[N_COLORS], action[A_COUNT];
    box_t canvas_box;
} S;

static window_t* g_win;

/* ------------------------------------------------------------------------
   Canvas operations
   ------------------------------------------------------------------------ */

static void snapshot(void) {
    memcpy(S.undo, S.canvas, (size_t)S.cw * S.ch * 4);
    S.can_undo = true;
}

static void undo_swap(void) {
    if (!S.can_undo) return;
    uint32_t* t = S.canvas; S.canvas = S.undo; S.undo = t;
}

static void canvas_fill(uint32_t c) {
    for (int i = 0; i < S.cw * S.ch; i++) S.canvas[i] = c;
}

static inline void put(int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)S.cw && (unsigned)y < (unsigned)S.ch) S.canvas[y * S.cw + x] = c;
}

static void dot(int cx, int cy, int r, uint32_t c) {
    if (r <= 0) { put(cx, cy, c); return; }
    int r2 = r * r + r;              /* +r rounds the disc nicely at small radii */
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if ((unsigned)y >= (unsigned)S.ch) continue;
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r2) put(cx + dx, y, c);
    }
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void line(int x0, int y0, int x1, int y1, int r, uint32_t c) {
    int dx = iabs(x1 - x0), dy = -iabs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        dot(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void rect_outline(int x0, int y0, int x1, int y1, int r, uint32_t c) {
    line(x0, y0, x1, y0, r, c);
    line(x1, y0, x1, y1, r, c);
    line(x1, y1, x0, y1, r, c);
    line(x0, y1, x0, y0, r, c);
}

static int isqrt(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* Ellipse inscribed in the box, sampled along both axes so it has no gaps. */
static void ellipse(int x0, int y0, int x1, int y1, int r, uint32_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int a = (x1 - x0) / 2, b = (y1 - y0) / 2;
    int cx = x0 + a, cy = y0 + b;
    if (a == 0 || b == 0) { line(x0, y0, x1, y1, r, c); return; }
    for (int dx = -a; dx <= a; dx++) {
        int dy = (int)((long long)b * isqrt(a * a - dx * dx) / a);
        dot(cx + dx, cy - dy, r, c);
        dot(cx + dx, cy + dy, r, c);
    }
    for (int dy = -b; dy <= b; dy++) {
        int dx = (int)((long long)a * isqrt(b * b - dy * dy) / b);
        dot(cx - dx, cy + dy, r, c);
        dot(cx + dx, cy + dy, r, c);
    }
}

/* Scanline flood fill with an explicit seed stack. */
static void flood(int x, int y, uint32_t c) {
    if ((unsigned)x >= (unsigned)S.cw || (unsigned)y >= (unsigned)S.ch) return;
    uint32_t from = S.canvas[y * S.cw + x];
    if (from == c) return;
    int cap = 16384;
    int* st = (int*)kmalloc((size_t)cap * 2 * sizeof(int));
    if (!st) return;
    int n = 0;
    st[n++] = x; st[n++] = y;
    while (n) {
        int py = st[--n], px = st[--n];
        uint32_t* row = S.canvas + py * S.cw;
        if (row[px] != from) continue;
        int l = px, r = px;
        while (l > 0 && row[l - 1] == from) l--;
        while (r < S.cw - 1 && row[r + 1] == from) r++;
        for (int i = l; i <= r; i++) row[i] = c;
        for (int k = -1; k <= 1; k += 2) {
            int ny = py + k;
            if ((unsigned)ny >= (unsigned)S.ch) continue;
            uint32_t* nr = S.canvas + ny * S.cw;
            for (int i = l; i <= r; i++) {
                if (nr[i] != from || (i > l && nr[i - 1] == from)) continue;
                if (n + 2 > cap * 2) continue;          /* stack full: skip seed */
                st[n++] = i; st[n++] = ny;
            }
        }
    }
    kfree(st);
}

/* ------------------------------------------------------------------------
   BMP export
   ------------------------------------------------------------------------ */

static void le16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t* p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

static void flash(const char* m) {
    strncpy(S.msg, m, sizeof(S.msg) - 1);
    S.msg[sizeof(S.msg) - 1] = 0;
    S.msg_until = pit_uptime_ms() + 4000;
}

static void save_bmp(void) {
    fs_node_t* mnt = fs_resolve(fs_root(), "/mnt");
    const char* dir = (mnt && mnt->mount_id) ? "/mnt" : "/home/user";
    char path[64];
    for (int i = 1; i < 1000; i++) {
        char num[8];
        itoa(i, num, 10);
        strcpy(path, dir);
        strcat(path, "/paint");
        strcat(path, num);
        strcat(path, ".bmp");
        if (!fs_resolve(fs_root(), path)) break;
    }
    int stride = (S.cw * 3 + 3) & ~3;
    uint32_t size = 54 + (uint32_t)stride * S.ch;
    uint8_t* b = (uint8_t*)kmalloc(size);
    if (!b) { flash("Save failed: out of memory"); return; }
    memset(b, 0, 54);
    b[0] = 'B'; b[1] = 'M';
    le32(b + 2, size); le32(b + 10, 54); le32(b + 14, 40);
    le32(b + 18, (uint32_t)S.cw); le32(b + 22, (uint32_t)S.ch);
    le16(b + 26, 1); le16(b + 28, 24); le32(b + 34, (uint32_t)stride * S.ch);
    le32(b + 38, 2835); le32(b + 42, 2835);
    for (int y = 0; y < S.ch; y++) {
        uint8_t* row = b + 54 + (size_t)(S.ch - 1 - y) * stride;
        memset(row, 0, stride);
        const uint32_t* src = S.canvas + y * S.cw;
        for (int x = 0; x < S.cw; x++) {
            row[x * 3 + 0] = (uint8_t)src[x];
            row[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            row[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
    }
    fs_node_t* f = fs_create(fs_root(), path, FS_FILE);
    if (f && fs_write(f, (const char*)b, size) >= 0) {
        char m[64] = "Saved ";
        strcat(m, path);
        flash(m);
    } else {
        flash("Save failed");
    }
    kfree(b);
}

/* ------------------------------------------------------------------------
   Drawing the UI
   ------------------------------------------------------------------------ */

static bool in(box_t b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void thick_line(int x0, int y0, int x1, int y1, uint32_t c) {
    gfx_line(x0, y0, x1, y1, c);
    gfx_line(x0 + 1, y0, x1 + 1, y1, c);
    gfx_line(x0, y0 + 1, x1, y1 + 1, c);
}

static void draw_icon(tool_t t, int x, int y, uint32_t c) {
    /* 18x18 icon box at (x, y) */
    switch (t) {
    case T_BRUSH:
        thick_line(x + 15, y + 2, x + 7, y + 10, c);
        gfx_disc(x + 5, y + 13, 3, c);
        gfx_rect_fill(x + 2, y + 15, 3, 2, c);
        break;
    case T_ERASER:
        gfx_rrect_fill(x + 1, y + 5, 16, 9, 2, GFX_CORNERS_ALL, c);
        gfx_rect_fill(x + 8, y + 7, 7, 5, C_SURFACE);
        gfx_rect_fill(x + 1, y + 16, 16, 1, c);
        break;
    case T_LINE:
        thick_line(x + 2, y + 15, x + 15, y + 2, c);
        break;
    case T_RECT:
        gfx_rect(x + 1, y + 3, 16, 12, c);
        gfx_rect(x + 2, y + 4, 14, 10, c);
        break;
    case T_ELLIPSE:
        gfx_circle(x + 9, y + 9, 8, c);
        gfx_circle(x + 9, y + 9, 7, c);
        break;
    case T_FILL:
        for (int i = 0; i < 8; i++) {                    /* tipped bucket */
            gfx_rect_fill(x + 3 + i, y + 2 + i, 2, 1, c);
            gfx_rect_fill(x + 3 - i + 7, y + 9 + i, 2, 1, c);
        }
        thick_line(x + 3, y + 9, x + 10, y + 16, c);
        gfx_disc(x + 15, y + 13, 2, c);
        gfx_rect_fill(x + 14, y + 9, 3, 3, c);
        break;
    case T_PICK:
        thick_line(x + 3, y + 15, x + 11, y + 7, c);
        gfx_disc(x + 13, y + 5, 3, c);
        gfx_rect_fill(x + 2, y + 15, 2, 2, c);
        break;
    default: break;
    }
}

static void button_bg(int x, int y, int w, int h, bool active) {
    if (active) {
        gfx_rrect_fill(x, y, w, h, 5, GFX_CORNERS_ALL, C_PRESSED);
        gfx_rect_fill(x + 8, y + h - 3, w - 16, 2, C_ACCENT);
    }
}

static void separator(int x, int cy) { gfx_rect_fill(x, cy + 10, 1, TB_H - 20, C_RULE); }

static void pt_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);

    /* --- toolbar --- */
    gfx_rect_fill(cx, cy, cw, TB_H, C_SURFACE);
    gfx_rect_fill(cx, cy + TB_H - 1, cw, 1, C_RULE);
    int x = PAD, by = (TB_H - BTN) / 2;
    for (int t = 0; t < T_COUNT; t++) {
        S.tool_box[t] = (box_t){ x, by, BTN, BTN };
        bool act = (S.tool == (tool_t)t);
        button_bg(cx + x, cy + by, BTN, BTN, act);
        draw_icon((tool_t)t, cx + x + 9, cy + by + 8, act ? C_INK : C_GLYPH);
        x += BTN + GAP;
    }
    x += 8; separator(cx + x, cy); x += 9;

    /* brush size: [-] preview [+] */
    S.minus = (box_t){ x, by + 6, 24, 24 };
    gfx_rrect_fill(cx + x, cy + by + 6, 24, 24, 5, GFX_CORNERS_ALL, C_PRESSED);
    gfx_rect_fill(cx + x + 7, cy + by + 17, 10, 2, C_INK);
    x += 28;
    int pv = cx + x + BTN / 2, pvy = cy + TB_H / 2;
    int pr = S.brush > 15 ? 15 : S.brush;
    gfx_rrect_fill(cx + x, cy + by, BTN, BTN, 5, GFX_CORNERS_ALL, PAPER);
    gfx_disc(pv, pvy, pr < 1 ? 1 : pr, S.tool == T_ERASER ? C_RULE : S.color);
    x += BTN + 4;
    S.plus = (box_t){ x, by + 6, 24, 24 };
    gfx_rrect_fill(cx + x, cy + by + 6, 24, 24, 5, GFX_CORNERS_ALL, C_PRESSED);
    gfx_rect_fill(cx + x + 7, cy + by + 17, 10, 2, C_INK);
    gfx_rect_fill(cx + x + 11, cy + by + 13, 2, 10, C_INK);
    x += 30;
    char sz[12];
    itoa(S.brush * 2 + 1, sz, 10);
    strcat(sz, " px");
    uif_draw_mid(cx + x, cy + TB_H / 2, UIF_SMALL, sz, C_INK_DIM);
    x += uif_width(UIF_SMALL, "00 px") + 10;
    separator(cx + x, cy); x += 13;

    /* colours: current + 2x8 grid */
    S.cur_swatch = (box_t){ x, by, BTN, BTN };
    gfx_rrect_fill(cx + x, cy + by, BTN, BTN, 6, GFX_CORNERS_ALL, C_OUTLINE);
    gfx_rrect_fill(cx + x + 2, cy + by + 2, BTN - 4, BTN - 4, 4, GFX_CORNERS_ALL, S.color);
    x += BTN + 10;
    int gy = (TB_H - (2 * SW + SW_GAP)) / 2;
    for (int i = 0; i < N_COLORS; i++) {
        int sx = x + (i % 8) * (SW + SW_GAP), sy = gy + (i / 8) * (SW + SW_GAP);
        S.swatch[i] = (box_t){ sx, sy, SW, SW };
        bool sel = palette[i] == S.color;
        gfx_rrect_fill(cx + sx, cy + sy, SW, SW, 3, GFX_CORNERS_ALL, sel ? C_ACCENT : C_RULE);
        gfx_rrect_fill(cx + sx + (sel ? 2 : 1), cy + sy + (sel ? 2 : 1),
                       SW - (sel ? 4 : 2), SW - (sel ? 4 : 2), 2, GFX_CORNERS_ALL, palette[i]);
    }

    /* actions, right-aligned */
    int ax = cw - PAD;
    for (int a = A_COUNT - 1; a >= 0; a--) {
        int bw = uif_width(UIF_MED, action_label[a]) + 24;
        ax -= bw;
        S.action[a] = (box_t){ ax, by + 4, bw, BTN - 8 };
        bool primary = (a == A_SAVE);
        bool dim = (a == A_UNDO && !S.can_undo);
        gfx_rrect_fill(cx + ax, cy + by + 4, bw, BTN - 8, 5, GFX_CORNERS_ALL,
                       primary ? C_ACCENT : C_RULE);
        if (!primary)
            gfx_rrect_fill(cx + ax + 1, cy + by + 5, bw - 2, BTN - 10, 4, GFX_CORNERS_ALL, C_SURFACE);
        uif_draw_center(cx + ax, cy + by + 4, bw, BTN - 8, UIF_MED, action_label[a],
                        dim ? C_GLYPH_DIM : C_INK);
        ax -= 6;
    }

    /* --- canvas well --- */
    int wy = TB_H, wh = ch - TB_H - SB_H;
    gfx_rect_fill(cx, cy + wy, cw, wh, C_WELL);
    int ox = (cw - S.cw) / 2, oy = wy + (wh - S.ch) / 2;
    S.canvas_box = (box_t){ ox, oy, S.cw, S.ch };
    gfx_shadow(cx + ox, cy + oy + 2, S.cw, S.ch, 2, 10, 70);
    gfx_rect(cx + ox - 1, cy + oy - 1, S.cw + 2, S.ch + 2, C_RULE);
    gfx_blit_argb(cx + ox, cy + oy, S.cw, S.ch, S.canvas);

    /* --- status bar --- */
    int sy = cy + ch - SB_H;
    gfx_rect_fill(cx, sy, cw, SB_H, C_SURFACE);
    gfx_rect_fill(cx, sy, cw, 1, C_RULE);
    int mid = sy + SB_H / 2 + 1;
    int tx = uif_draw_mid(cx + PAD, mid, UIF_MED, tool_name[S.tool], C_INK);
    char info[48] = "   ";
    itoa(S.brush * 2 + 1, info + 3, 10);
    strcat(info, " px");
    tx = uif_draw_mid(tx, mid, UIF_SMALL, info, C_INK_DIM);
    if (S.hover_x >= 0) {
        char pos[24], n[8];
        itoa(S.hover_x, n, 10); strcpy(pos, "      "); strcat(pos, n); strcat(pos, ", ");
        itoa(S.hover_y, n, 10); strcat(pos, n);
        uif_draw_mid(tx, mid, UIF_SMALL, pos, C_INK_DIM);
    }
    char dims[24], n[8];
    itoa(S.cw, n, 10); strcpy(dims, n); strcat(dims, " x ");
    itoa(S.ch, n, 10); strcat(dims, n);
    int dx = cx + cw - PAD - uif_width(UIF_SMALL, dims);
    uif_draw_mid(dx, mid, UIF_SMALL, dims, C_INK_DIM);
    if (S.msg[0]) {
        int mw = uif_width(UIF_MED, S.msg);
        int mx = dx - 24 - mw;
        gfx_rrect_fill(mx - 6, mid - 5, 6, 6, 3, GFX_CORNERS_ALL, C_ONLINE);
        uif_draw_mid(mx + 6, mid, UIF_MED, S.msg, C_INK);
    }
}

/* ------------------------------------------------------------------------
   Input
   ------------------------------------------------------------------------ */

static uint32_t draw_color(void) { return S.tool == T_ERASER ? PAPER : S.color; }

static void shape_preview(int x, int y) {
    memcpy(S.canvas, S.undo, (size_t)S.cw * S.ch * 4);
    if (S.tool == T_LINE)    line(S.sx, S.sy, x, y, S.brush, S.color);
    if (S.tool == T_RECT)    rect_outline(S.sx, S.sy, x, y, S.brush, S.color);
    if (S.tool == T_ELLIPSE) ellipse(S.sx, S.sy, x, y, S.brush, S.color);
}

static void pick(int x, int y) {
    if ((unsigned)x < (unsigned)S.cw && (unsigned)y < (unsigned)S.ch)
        S.color = S.canvas[y * S.cw + x];
}

static void toolbar_press(int rx, int ry) {
    for (int t = 0; t < T_COUNT; t++)
        if (in(S.tool_box[t], rx, ry)) { S.tool = (tool_t)t; return; }
    for (int i = 0; i < N_COLORS; i++)
        if (in(S.swatch[i], rx, ry)) {
            S.color = palette[i];
            if (S.tool == T_ERASER || S.tool == T_PICK) S.tool = T_BRUSH;
            return;
        }
    if (in(S.minus, rx, ry) && S.brush > 0) { S.brush--; return; }
    if (in(S.plus, rx, ry) && S.brush < MAX_BRUSH) { S.brush++; return; }
    if (in(S.action[A_UNDO], rx, ry))  { undo_swap(); return; }
    if (in(S.action[A_CLEAR], rx, ry)) { snapshot(); canvas_fill(PAPER); return; }
    if (in(S.action[A_SAVE], rx, ry))  { save_bmp(); return; }
}

static void pt_click(window_t* w, int rx, int ry) {
    (void)w;
    int x = rx - S.canvas_box.x, y = ry - S.canvas_box.y;
    if (!S.pressing) {                           /* fresh press */
        S.pressing = true;
        if (!in(S.canvas_box, rx, ry)) { toolbar_press(rx, ry); return; }
        S.stroking = true;
        S.sx = S.lx = x; S.sy = S.ly = y;
        if (S.tool == T_PICK) { pick(x, y); return; }
        snapshot();
        if (S.tool == T_FILL)                         flood(x, y, S.color);
        else if (S.tool == T_BRUSH || S.tool == T_ERASER) dot(x, y, S.brush, draw_color());
        return;
    }
    if (!S.stroking) return;
    switch (S.tool) {
    case T_BRUSH: case T_ERASER: line(S.lx, S.ly, x, y, S.brush, draw_color()); break;
    case T_LINE: case T_RECT: case T_ELLIPSE: shape_preview(x, y); break;
    case T_PICK: pick(x, y); break;
    default: break;
    }
    S.lx = x; S.ly = y;
}

static void pt_release(window_t* w) {
    (void)w;
    S.pressing = S.stroking = false;
}

static void pt_tick(window_t* w, uint32_t now) {
    int cx, cy, cw, ch, mx, my;
    uint8_t b;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    mouse_get(&mx, &my, &b);
    int hx = mx - cx - S.canvas_box.x, hy = my - cy - S.canvas_box.y;
    bool inside = w == wm_focused() && hx >= 0 && hy >= 0 && hx < S.cw && hy < S.ch;
    if (!inside) hx = hy = -1;
    if (hx != S.hover_x || hy != S.hover_y) {
        S.hover_x = hx; S.hover_y = hy;
        w->needs_repaint = true;
    }
    if (S.msg[0] && (int32_t)(now - S.msg_until) >= 0) { S.msg[0] = 0; w->needs_repaint = true; }
}

static void pt_key(window_t* w, char c) {
    uint8_t k = (uint8_t)c;
    if (k == 0x1B) { wm_close(w); return; }
    if (k == 0x1A) { undo_swap(); return; }              /* ^Z */
    if (k == 0x13) { save_bmp(); return; }               /* ^S */
    if (k == (uint8_t)K_DEL) { snapshot(); canvas_fill(PAPER); return; }
    if (c == '+' || c == '=' || c == ']') { if (S.brush < MAX_BRUSH) S.brush++; return; }
    if (c == '-' || c == '_' || c == '[') { if (S.brush > 0) S.brush--; return; }
    if (c >= '1' && c <= '9') { S.color = palette[c - '1']; return; }
    if (c == '0') { S.color = palette[9]; return; }
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (int t = 0; t < T_COUNT; t++)
        if (c == tool_key[t]) { S.tool = (tool_t)t; return; }
}

static void pt_close(window_t* w) { (void)w; g_win = NULL; S.pressing = S.stroking = false; }

int paint_open(void) {
    if (!gfx_ready()) return -1;
    if (g_win && g_win->open) return 0;

    int W = gfx_w(), H = gfx_h();
    int ww = W - 60 < 1040 ? W - 60 : 1040;
    int wh = H - 110 < 700 ? H - 110 : 700;
    int ccw = ww - 8, cch = wh - WM_TITLE_H - 6;
    int want_w = ccw - 2 * 20, want_h = cch - TB_H - SB_H - 2 * 16;

    if (!S.canvas) {
        S.cw = want_w; S.ch = want_h;
        S.canvas = (uint32_t*)kmalloc((size_t)S.cw * S.ch * 4);
        S.undo = (uint32_t*)kmalloc((size_t)S.cw * S.ch * 4);
        if (!S.canvas || !S.undo) {
            if (S.canvas) kfree(S.canvas);
            if (S.undo) kfree(S.undo);
            S.canvas = S.undo = NULL;
            return -1;
        }
        canvas_fill(PAPER);
        S.can_undo = false;
        S.tool = T_BRUSH;
        S.color = palette[0];
        S.brush = 2;
    }
    S.pressing = S.stroking = false;
    S.hover_x = S.hover_y = -1;
    S.msg[0] = 0;

    int wx = (W - ww) / 2, wy = (H - 40 - wh) / 2;
    if (wy < 10) wy = 10;
    g_win = wm_open_app_ex(wx, wy, ww, wh, "Paint",
                           pt_paint, pt_key, pt_click, pt_tick, false, &S);
    if (!g_win) return -1;
    g_win->on_release = pt_release;
    g_win->on_close = pt_close;
    return 0;
}
