/* Extra desktop terminals. The first Terminal window is the kernel shell
   (one gfx_term, one console tty); every next one is this: its own cell
   grid and a small VT parser, with /bin/sh -i running on a pty behind it,
   the same way the ssh and telnet sessions work. */

#include "apps/pterm.h"
#include "gui/wm.h"
#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "gfx/gfx_term.h"
#include "gfx/termfont.h"
#include "core/string.h"
#include "core/heap.h"
#include "core/task.h"
#include "drivers/keyboard.h"
#include "proc/proc.h"
#include "proc/pty.h"
#include "drivers/mouse.h"
#include "gui/uwin.h"

#define PT_MAX 8
#define MAXC 200
#define MAXR 80
#define CW 8
#define CH 16
#define PADX 4

enum { AT_BOLD = 1, AT_UL = 2, AT_INV = 4 };

typedef struct { uint8_t ch, at; uint32_t fg, bg; } pcell_t;

typedef struct {
    bool used;
    window_t* win;
    int pty, pid;
    int cols, rows;
    pcell_t* scr[2];              /* main, alternate */
    int alt;
    int cx, cy, scx, scy;         /* cursor, saved cursor */
    bool wrap, cur_on;
    int top, bot;                 /* scroll region */
    uint32_t fg, bg;
    uint8_t at;
    int st, np, par[16];
    bool priv;
    uint32_t u8, u8n;             /* utf-8 decoder */
    bool dirty;
    uint32_t rowh[MAXR];          /* what each row looked like when last drawn */
    int lcx, lcy;
    int sa, sb;                   /* selection, cell indexes y*MAXC+x */
    bool sel, drag;
    uint8_t pbtn;
} pt_t;

static pt_t pts[PT_MAX];

#define C(t, x, y) ((t)->scr[(t)->alt][(y) * MAXC + (x)])

static void scat_(char* d, const char* s) { while (*d) d++; while ((*d++ = *s++)) ; }
static void scpy_(char* d, const char* s) { *d = 0; scat_(d, s); }

static uint32_t dfg(void) { return gfx_term_color(7); }
static uint32_t dbg(void) { return gfx_term_color(0); }

static void clear_cells(pt_t* t, int x0, int y, int x1) {
    for (int x = x0; x < x1 && x < MAXC; x++) {
        pcell_t* c = &C(t, x, y);
        c->ch = ' '; c->at = 0; c->fg = t->fg; c->bg = t->bg;
    }
}

static void scroll_up(pt_t* t, int top, int bot, int n) {
    for (int k = 0; k < n; k++) {
        for (int y = top; y < bot; y++)
            memcpy(&C(t, 0, y), &C(t, 0, y + 1), sizeof(pcell_t) * MAXC);
        clear_cells(t, 0, bot, t->cols);
    }
}
static void scroll_down(pt_t* t, int top, int bot, int n) {
    for (int k = 0; k < n; k++) {
        for (int y = bot; y > top; y--)
            memcpy(&C(t, 0, y), &C(t, 0, y - 1), sizeof(pcell_t) * MAXC);
        clear_cells(t, 0, top, t->cols);
    }
}

static void lf(pt_t* t) {
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void put(pt_t* t, uint8_t ch) {
    if (t->wrap) { t->wrap = false; t->cx = 0; lf(t); }
    pcell_t* c = &C(t, t->cx, t->cy);
    c->ch = ch; c->at = t->at; c->fg = t->fg; c->bg = t->bg;
    if (t->cx == t->cols - 1) t->wrap = true;
    else t->cx++;
}

static uint32_t ansi(int i, bool bright);
static uint32_t c256(int n) {
    if (n < 16) return ansi(n & 7, n >= 8);
    if (n >= 232) { int v = 8 + (n - 232) * 10; return RGB(v, v, v); }
    n -= 16;
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    return RGB(lv[n / 36], lv[(n / 6) % 6], lv[n % 6]);
}
/* ANSI colour index -> the VGA-ordered palette gfx_term uses */
static uint32_t ansi(int i, bool bright) { return gfx_term_color((bright ? 8 : 0) + (int)"\0\4\2\6\1\5\3\7"[i & 7]); }

static void sgr(pt_t* t) {
    if (!t->np) t->par[t->np++] = 0;
    for (int i = 0; i < t->np; i++) {
        int p = t->par[i];
        if (p == 0) { t->at = 0; t->fg = dfg(); t->bg = dbg(); }
        else if (p == 1) t->at |= AT_BOLD;
        else if (p == 4) t->at |= AT_UL;
        else if (p == 7) t->at |= AT_INV;
        else if (p == 22) t->at &= ~AT_BOLD;
        else if (p == 24) t->at &= ~AT_UL;
        else if (p == 27) t->at &= ~AT_INV;
        else if (p >= 30 && p <= 37) t->fg = ansi(p - 30, t->at & AT_BOLD);
        else if (p == 39) t->fg = dfg();
        else if (p >= 40 && p <= 47) t->bg = ansi(p - 40, false);
        else if (p == 49) t->bg = dbg();
        else if (p >= 90 && p <= 97) t->fg = ansi(p - 90, true);
        else if (p >= 100 && p <= 107) t->bg = ansi(p - 100, true);
        else if ((p == 38 || p == 48) && i + 2 < t->np && t->par[i + 1] == 5) {
            uint32_t c = c256(t->par[i + 2]);
            if (p == 38) t->fg = c; else t->bg = c;
            i += 2;
        } else if ((p == 38 || p == 48) && i + 4 < t->np && t->par[i + 1] == 2) {
            uint32_t c = RGB(t->par[i + 2] & 255, t->par[i + 3] & 255, t->par[i + 4] & 255);
            if (p == 38) t->fg = c; else t->bg = c;
            i += 4;
        }
    }
}

static void reply(pt_t* t, const char* s) { pty_write(t->pty, true, s, (int)strlen(s), true); }

static void csi(pt_t* t, char f) {
    int a = t->np ? t->par[0] : 0, b = t->np > 1 ? t->par[1] : 0;
    int n = a ? a : 1;
    t->wrap = false;
    switch (f) {
    case 'A': t->cy -= n; if (t->cy < 0) t->cy = 0; break;
    case 'B': case 'e': t->cy += n; break;
    case 'C': case 'a': t->cx += n; break;
    case 'D': t->cx -= n; break;
    case 'E': t->cy += n; t->cx = 0; break;
    case 'F': t->cy -= n; t->cx = 0; break;
    case 'G': case '`': t->cx = n - 1; break;
    case 'd': t->cy = n - 1; break;
    case 'H': case 'f': t->cy = (a ? a : 1) - 1; t->cx = (b ? b : 1) - 1; break;
    case 'J':
        if (a == 0) { clear_cells(t, t->cx, t->cy, t->cols); for (int y = t->cy + 1; y < t->rows; y++) clear_cells(t, 0, y, t->cols); }
        else if (a == 1) { clear_cells(t, 0, t->cy, t->cx + 1); for (int y = 0; y < t->cy; y++) clear_cells(t, 0, y, t->cols); }
        else for (int y = 0; y < t->rows; y++) clear_cells(t, 0, y, t->cols);
        break;
    case 'K':
        if (a == 0) clear_cells(t, t->cx, t->cy, t->cols);
        else if (a == 1) clear_cells(t, 0, t->cy, t->cx + 1);
        else clear_cells(t, 0, t->cy, t->cols);
        break;
    case 'X': clear_cells(t, t->cx, t->cy, t->cx + n); break;
    case 'P':
        for (int x = t->cx; x < t->cols; x++) C(t, x, t->cy) = x + n < t->cols ? C(t, x + n, t->cy) : C(t, x, t->cy);
        clear_cells(t, t->cols - n > t->cx ? t->cols - n : t->cx, t->cy, t->cols);
        break;
    case '@':
        for (int x = t->cols - 1; x >= t->cx + n; x--) C(t, x, t->cy) = C(t, x - n, t->cy);
        clear_cells(t, t->cx, t->cy, t->cx + n);
        break;
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) scroll_down(t, t->cy, t->bot, n); break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) scroll_up(t, t->cy, t->bot, n); break;
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'm': sgr(t); break;
    case 'r':
        t->top = (a ? a : 1) - 1;
        t->bot = (b ? b : t->rows) - 1;
        if (t->bot >= t->rows) t->bot = t->rows - 1;
        if (t->top >= t->bot) { t->top = 0; t->bot = t->rows - 1; }
        t->cx = t->cy = 0;
        break;
    case 's': t->scx = t->cx; t->scy = t->cy; break;
    case 'u': t->cx = t->scx; t->cy = t->scy; break;
    case 'n':
        if (a == 6) {
            char r[24] = "\x1b[";
            char num[8];
            itoa(t->cy + 1, num, 10); scat_(r, num);
            scat_(r, ";");
            itoa(t->cx + 1, num, 10); scat_(r, num);
            scat_(r, "R");
            reply(t, r);
        } else if (a == 5) reply(t, "\x1b[0n");
        break;
    case 'c': if (!t->priv) reply(t, "\x1b[?6c"); break;
    case 'h': case 'l': {
        bool on = f == 'h';
        for (int i = 0; i < t->np; i++) {
            int p = t->par[i];
            if (!t->priv) continue;
            if (p == 25) t->cur_on = on;
            else if (p == 1049 || p == 47 || p == 1047) {
                if (on == (t->alt == 1)) continue;
                if (on) { t->scx = t->cx; t->scy = t->cy; }
                t->alt = on;
                if (on) for (int y = 0; y < t->rows; y++) clear_cells(t, 0, y, t->cols);
                else { t->cx = t->scx; t->cy = t->scy; }
            }
        }
        break;
    }
    }
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

enum { S_N, S_ESC, S_CSI, S_OSC, S_OSCE, S_SKIP };

static void feed(pt_t* t, uint8_t c) {
    switch (t->st) {
    case S_ESC:
        t->st = S_N;
        switch (c) {
        case '[': t->st = S_CSI; t->np = 0; t->priv = false; memset(t->par, 0, sizeof t->par); return;
        case ']': t->st = S_OSC; return;
        case '(': case ')': case '*': case '+': case '#': t->st = S_SKIP; return;
        case '7': t->scx = t->cx; t->scy = t->cy; return;
        case '8': t->cx = t->scx; t->cy = t->scy; return;
        case 'D': lf(t); return;
        case 'E': t->cx = 0; lf(t); return;
        case 'M': if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1); else if (t->cy > 0) t->cy--; return;
        case 'c':
            t->fg = dfg(); t->bg = dbg(); t->at = 0; t->alt = 0;
            t->top = 0; t->bot = t->rows - 1; t->cx = t->cy = 0;
            for (int y = 0; y < t->rows; y++) clear_cells(t, 0, y, t->cols);
            return;
        }
        return;
    case S_SKIP: t->st = S_N; return;
    case S_OSC:                                   /* window title etc: ignored */
        if (c == 7) t->st = S_N;
        else if (c == 0x1b) t->st = S_OSCE;
        return;
    case S_OSCE: t->st = c == '\\' ? S_N : S_OSC; return;
    case S_CSI:
        if (c >= '0' && c <= '9') {
            if (!t->np) t->np = 1;
            t->par[t->np - 1] = t->par[t->np - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';' || c == ':') { if (!t->np) t->np = 1; if (t->np < 16) t->np++; return; }
        if (c == '?' || c == '>' || c == '=') { t->priv = true; return; }
        if (c >= 0x40 && c <= 0x7E) { t->st = S_N; csi(t, (char)c); }
        return;
    }
    /* normal */
    if (t->u8n) {
        if ((c & 0xC0) == 0x80) {
            t->u8 = (t->u8 << 6) | (c & 0x3F);
            if (--t->u8n == 0) {
                int k = uni_to_cp866(t->u8);
                put(t, (uint8_t)(k < 0 ? '?' : k));
            }
            return;
        }
        t->u8n = 0;
    }
    if (c >= 0xC0 && c < 0xF8) {
        t->u8n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        t->u8 = c & (c >= 0xF0 ? 7 : c >= 0xE0 ? 15 : 31);
        return;
    }
    switch (c) {
    case 0x1b: t->st = S_ESC; return;
    case '\r': t->cx = 0; t->wrap = false; return;
    case '\n': case 0x0B: case 0x0C: t->wrap = false; lf(t); return;
    case '\b': if (t->cx > 0) t->cx--; t->wrap = false; return;
    case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) t->cx = t->cols - 1; return;
    case 7: case 0x0E: case 0x0F: case 0: return;
    }
    if (c < 0x20 || c == 0x7F) return;
    put(t, c);
}

static void pt_paint(window_t* w) {
    pt_t* t = (pt_t*)w->user;
    int x0, y0, cw, ch;
    wm_client_rect(w, &x0, &y0, &cw, &ch);
    gfx_rect_fill(x0 > 0 ? x0 : 0, y0, cw, ch, dbg());
    int ox = x0 + PADX, oy = y0 + 2;
    /* only the cells under the clip: the WM repaints small damaged pieces */
    int kx, ky, kw, kh;
    gfx_get_clip(&kx, &ky, &kw, &kh);
    int ya = (ky - oy) / CH, yb = (ky + kh - oy) / CH + 1, xa = (kx - ox) / CW, xb = (kx + kw - ox) / CW + 1;
    if (ya < 0) ya = 0;
    if (xa < 0) xa = 0;
    if (yb > t->rows) yb = t->rows;
    if (xb > t->cols) xb = t->cols;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            pcell_t* c = &C(t, x, y);
            uint32_t fg = c->fg, bg = c->bg;
            if (c->at & AT_INV) { uint32_t s = fg; fg = bg; bg = s; }
            if (t->sel) {
                int lo = t->sa < t->sb ? t->sa : t->sb, hi = t->sa < t->sb ? t->sb : t->sa, ix = y * MAXC + x;
                if (ix >= lo && ix <= hi) { uint32_t s = fg; fg = bg; bg = s; if (fg == bg) fg = dbg(); }
            }
            bool cur = t->cur_on && x == t->cx && y == t->cy && wm_focused() == w;
            if (cur) { uint32_t s = fg; fg = bg == dbg() ? dbg() : bg; bg = s == dbg() ? dfg() : s; if (fg == bg) fg = dbg(); }
            int px = ox + x * CW, py = oy + y * CH;
            if (bg != dbg() || cur) gfx_rect_fill(px, py, CW, CH, bg);
            if (c->ch != ' ') uif_mono_char(px, py, c->ch, fg, 0, false);
            if (c->at & AT_UL) gfx_rect_fill(px, py + CH - 2, CW, 1, fg);
        }
}

static void sel_changed(pt_t* t) {
    int x0, y0, cw, ch;
    wm_client_rect(t->win, &x0, &y0, &cw, &ch);
    wm_damage(t->win, x0, y0, cw, ch);
}

static void sel_copy(pt_t* t) {
    static char out[MAXC * MAXR * 3];
    int lo = t->sa < t->sb ? t->sa : t->sb, hi = t->sa < t->sb ? t->sb : t->sa, n = 0;
    for (int y = lo / MAXC; y <= hi / MAXC && y < t->rows; y++) {
        int a = y == lo / MAXC ? lo % MAXC : 0, b = y == hi / MAXC ? hi % MAXC : t->cols - 1;
        if (b >= t->cols) b = t->cols - 1;
        while (b >= a && C(t, b, y).ch == ' ') b--;      // trailing blanks
        for (int x = a; x <= b; x++) {
            uint8_t c = C(t, x, y).ch;
            if (c < 0x80) out[n++] = c ? c : ' ';
            else { uint32_t cp = cp866_to_uni(c); out[n++] = 0xC0 | (cp >> 6); out[n++] = 0x80 | (cp & 0x3F); }
        }
        if (y < hi / MAXC) out[n++] = '\n';
    }
    if (n) clip_set(out, n);
}

static void pt_paste(pt_t* t) {
    static char b[4096];
    int n = clip_get(b, sizeof b);
    for (int i = 0; i < n; i++) if (b[i] == '\n') b[i] = '\r';
    if (n > 0) pty_write(t->pty, true, b, n, true);
}

static void send(pt_t* t, const char* s, int n) { pty_write(t->pty, true, s, n, true); }

static void pt_key(window_t* w, char ch) {
    pt_t* t = (pt_t*)w->user;
    uint8_t c = (uint8_t)ch;
    if (c == 0x16 && kbd_shift_held()) { pt_paste(t); return; }          // ctrl+shift+v
    if (c == 3 && kbd_shift_held() && t->sel) { sel_copy(t); return; }   // ctrl+shift+c
    if (t->sel) { t->sel = false; sel_changed(t); }
    bool ru = kbd_is_ru() && ((c >= 0x80 && c <= 0xAF) || (c >= 0xE0 && c <= 0xF1));
    const char* seq = NULL;
    if (!ru) switch (c) {
        case K_UP: seq = "\x1b[A"; break;
        case K_DOWN: seq = "\x1b[B"; break;
        case K_RIGHT: seq = "\x1b[C"; break;
        case K_LEFT: seq = "\x1b[D"; break;
        case K_HOME: seq = "\x1b[1~"; break;
        case K_END: seq = "\x1b[4~"; break;
        case K_DEL: seq = "\x1b[3~"; break;
        case K_PGUP: seq = "\x1b[5~"; break;
        case K_PGDN: seq = "\x1b[6~"; break;
    }
    if (seq) { send(t, seq, (int)strlen(seq)); return; }
    if (!ru && c >= K_F1 && c < K_F1 + 10) return;
    if (c == '\b') c = 0x7F;
    if (c == '\n') c = '\r';
    if (c >= 0x80) {                              /* keyboard gives cp866, programs want utf-8 */
        uint32_t cp = cp866_to_uni(c);
        char u[3];
        if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); send(t, u, 2); }
        else { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); send(t, u, 3); }
        return;
    }
    char b = (char)c;
    send(t, &b, 1);
}

static void pt_fit(pt_t* t) {
    int x0, y0, cw, ch;
    wm_client_rect(t->win, &x0, &y0, &cw, &ch);
    int nc = (cw - 2 * PADX) / CW, nr = (ch - 4) / CH;
    if (nc > MAXC) nc = MAXC;
    if (nr > MAXR) nr = MAXR;
    if (nc < 10) nc = 10;
    if (nr < 3) nr = 3;
    if (nc == t->cols && nr == t->rows) return;
    for (int s = 0; s < 2; s++) {                 /* blank what the old size never touched */
        int oa = t->alt;
        t->alt = s;
        for (int y = 0; y < nr; y++) clear_cells(t, y < t->rows ? t->cols : 0, y, nc);
        t->alt = oa;
    }
    if (t->cy >= nr) {                            /* keep the cursor line: scroll the rest up */
        int d = t->cy - nr + 1;
        for (int y = 0; y + d < t->rows; y++) memcpy(&C(t, 0, y), &C(t, 0, y + d), sizeof(pcell_t) * MAXC);
        t->cy -= d;
    }
    t->cols = nc; t->rows = nr;
    t->top = 0; t->bot = nr - 1;
    if (t->cx >= nc) t->cx = nc - 1;
    pty_set_size(t->pty, nr, nc);
    memset(t->rowh, 0, sizeof t->rowh);
    t->win->needs_repaint = true;
}

static void pt_tick(window_t* w, uint32_t now) {
    pt_t* t = (pt_t*)w->user;
    (void)now;
    pt_fit(t);
    if (wm_focused() == w) {
        int mx, my, x0, y0, cw, ch;
        uint8_t b;
        mouse_get(&mx, &my, &b);
        wm_client_rect(w, &x0, &y0, &cw, &ch);
        int cx = (mx - x0 - PADX) / CW, cy = (my - y0 - 2) / CH;
        if (cx < 0) cx = 0;
        if (cx >= t->cols) cx = t->cols - 1;
        if (cy < 0) cy = 0;
        if (cy >= t->rows) cy = t->rows - 1;
        bool in = mx >= x0 && mx < x0 + cw && my >= y0 && my < y0 + ch;
        if ((b & 1) && !(t->pbtn & 1) && in) { t->drag = true; t->sa = t->sb = cy * MAXC + cx; if (t->sel) { t->sel = false; sel_changed(t); } }
        else if ((b & 1) && t->drag && cy * MAXC + cx != t->sb) {
            t->sb = cy * MAXC + cx;
            t->sel = t->sa != t->sb;
            sel_changed(t);
        }
        if (!(b & 1) && (t->pbtn & 1) && t->drag) {
            t->drag = false;
            if (t->sel) sel_copy(t);
        }
        if ((b & 2) && !(t->pbtn & 2) && in) pt_paste(t);
        t->pbtn = b;
    }
    static char buf[2048];
    for (int k = 0; k < 16; k++) {
        int n = pty_read(t->pty, true, buf, sizeof buf, true);
        if (n == -5 || (n <= 0 && t->pid > 0 && !proc_alive(t->pid) && !pty_pending(t->pty, true))) {
            wm_close(w);                              /* the shell exited */
            return;
        }
        if (n <= 0) break;
        for (int i = 0; i < n; i++) feed(t, (uint8_t)buf[i]);
        t->dirty = true;
    }
    if (!t->dirty) return;
    t->dirty = false;
    /* damage only rows that changed (and where the cursor was / is) */
    int x0, y0, cw, ch;
    wm_client_rect(w, &x0, &y0, &cw, &ch);
    for (int y = 0; y < t->rows; y++) {
        uint32_t hsh = 2166136261u;
        for (int x = 0; x < t->cols; x++) {
            pcell_t* c = &C(t, x, y);
            hsh = (hsh ^ c->ch) * 16777619u;
            hsh = (hsh ^ c->at) * 16777619u;
            hsh = (hsh ^ c->fg) * 16777619u;
            hsh = (hsh ^ c->bg) * 16777619u;
        }
        bool cur = (y == t->cy || y == t->lcy) && (t->cx != t->lcx || t->cy != t->lcy);
        if (hsh != t->rowh[y] || cur) {
            t->rowh[y] = hsh;
            int y1 = y;
            while (y1 + 1 < t->rows) {                  /* merge runs of dirty rows */
                uint32_t h2 = 2166136261u;
                for (int x = 0; x < t->cols; x++) {
                    pcell_t* c = &C(t, x, y1 + 1);
                    h2 = (h2 ^ c->ch) * 16777619u; h2 = (h2 ^ c->at) * 16777619u;
                    h2 = (h2 ^ c->fg) * 16777619u; h2 = (h2 ^ c->bg) * 16777619u;
                }
                bool cur2 = (y1 + 1 == t->cy || y1 + 1 == t->lcy) && (t->cx != t->lcx || t->cy != t->lcy);
                if (h2 == t->rowh[y1 + 1] && !cur2) break;
                t->rowh[++y1] = h2;
            }
            wm_damage(w, x0, y0 + 2 + y * CH, cw, (y1 - y + 1) * CH);
            y = y1;
        }
    }
    t->lcx = t->cx; t->lcy = t->cy;
}

static void pt_close(window_t* w) {
    pt_t* t = (pt_t*)w->user;
    if (!t || !t->used) return;
    if (t->pid > 0) {
        proc_t* p = proc_by_pid(t->pid);
        if (p) proc_kill_session(p->sid);
    }
    pty_master_close(t->pty);
    for (int s = 0; s < 2; s++) if (t->scr[s]) kfree(t->scr[s]);
    memset(t, 0, sizeof *t);
}

window_t* pterm_open(int x, int y) {
    pt_t* t = NULL;
    for (int i = 0; i < PT_MAX; i++) if (!pts[i].used) { t = &pts[i]; break; }
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    t->scr[0] = kmalloc_big(sizeof(pcell_t) * MAXC * MAXR);
    t->scr[1] = kmalloc_big(sizeof(pcell_t) * MAXC * MAXR);
    if (!t->scr[0] || !t->scr[1]) goto fail;
    t->pty = pty_alloc();
    if (t->pty < 0) goto fail;
    pty_unlock(t->pty);
    t->used = true;
    t->fg = dfg(); t->bg = dbg();
    t->cols = 80; t->rows = 25;
    t->top = 0; t->bot = t->rows - 1;
    t->cur_on = true;
    for (int s = 0; s < 2; s++) { t->alt = s; for (int yy = 0; yy < MAXR; yy++) clear_cells(t, 0, yy, MAXC); }
    t->alt = 0;
    pty_set_size(t->pty, t->rows, t->cols);

    /* sh on the slave: redirecting to /dev/pts/N makes it the controlling tty,
       the spawned process is a session leader without one */
    static char cmd[96];
    char num[8];
    itoa(t->pty, num, 10);
    scpy_(cmd, "exec /bin/sh -i </dev/pts/");
    scat_(cmd, num); scat_(cmd, " >/dev/pts/"); scat_(cmd, num); scat_(cmd, " 2>&1");
    char* argv[] = { (char*)"sh", (char*)"-c", cmd, NULL };
    char* envp[] = { (char*)"PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/games:/usr/local/bin:/opt/gcc/bin", (char*)"HOME=/root",
                     (char*)"USER=root", (char*)"LOGNAME=root", (char*)"SHELL=/bin/sh", (char*)"TERM=linux",
                     (char*)"COLORTERM=truecolor", (char*)"ENV=/etc/shrc", (char*)"PS1=\\u@\\h:\\w\\$ ", (char*)"LANG=C.UTF-8",
                     (char*)"XDG_RUNTIME_DIR=/run/user/0", (char*)"WAYLAND_DISPLAY=wayland-0", (char*)"DISPLAY=:0",
                     (char*)"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus",
                     (char*)"SDL_VIDEODRIVER=wayland,x11", (char*)"MESA_VK_WSI_DEBUG=sw", NULL };   // sdl over xwayland segfaults (neverball), TODO
    t->pid = proc_spawn_detached("/bin/sh", argv, envp);
    if (t->pid < 0) { pty_master_close(t->pty); goto fail; }

    int w = t->cols * CW + 2 * PADX + 8, h = t->rows * CH + 4 + WM_TITLE_H + 6;
    t->win = wm_open_app_ex(x, y, w, h, "Terminal", pt_paint, pt_key, NULL, pt_tick, true, t);
    if (!t->win) { proc_t* p = proc_by_pid(t->pid); if (p) proc_kill_session(p->sid); pty_master_close(t->pty); goto fail; }
    t->win->on_close = pt_close;
    t->win->opaque = true;
    t->win->group = WG_TERMINAL;
    t->win->min_w = 20 * CW + 2 * PADX + 8;
    t->win->min_h = 5 * CH + WM_TITLE_H + 10;
    return t->win;
fail:
    for (int s = 0; s < 2; s++) if (t->scr[s]) kfree(t->scr[s]);
    memset(t, 0, sizeof *t);
    return NULL;
}

bool pterm_drop(window_t* w, const char* s, int n) {
    for (int i = 0; i < PT_MAX; i++) {
        if (!pts[i].used || pts[i].win != w) continue;
        char o[4200];
        int k = 0;
        // one path per line -> 'path' 'path'
        o[k++] = '\'';
        for (int j = 0; j < n && k < 4190; j++) {
            if (s[j] == '\n') { o[k++] = '\''; o[k++] = ' '; o[k++] = '\''; }
            else if (s[j] == '\'') { o[k++] = '\''; o[k++] = '\\'; o[k++] = '\''; o[k++] = '\''; }
            else o[k++] = s[j];
        }
        o[k++] = '\'';
        o[k++] = ' ';
        pty_write(pts[i].pty, true, o, k, true);
        return true;
    }
    return false;
}
