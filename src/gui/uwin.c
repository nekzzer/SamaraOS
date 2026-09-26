/* User-program windows (syscall 500). Concurrency model: syscalls run with
   interrupts off, the WM task is preemptible. So a syscall never touches WM
   structures directly — it flips a slot's state and the WM performs the
   actual wm_open/wm_close in uwin_wm_frame() on its own task. Pixel copies
   in both directions only race visually (a torn frame), never structurally. */

#include "gui/uwin.h"
#include "gui/wm.h"
#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "proc/proc.h"
#include "boot/pit.h"

#define UWIN_MAX   8
#define EVQ        128
#define MAX_PIXELS (1600 * 1000)

enum { U_FREE, U_PENDING, U_OPEN, U_CLOSING, U_FAILED };

/* Text the program drew into its window buffer, kept as text: the WM draws
   it at screen resolution over the scaled image (see u_paint), so a window
   shown at 3x does not get 3x-pixel letters. One list per frame. */
#define OVL_MAX 64
#define OVL_STR 64
typedef struct {
    int16_t  x, y;
    int16_t  font;
    uint32_t color;
    char     s[OVL_STR];
} ovl_t;

typedef struct {
    volatile int state;
    int pid;
    int w, h, scale;
    char title[64];
    volatile bool title_dirty;
    uint32_t* pix;
    window_t* win;
    volatile bool gone;              /* window closed (by user or WM exit) */
    sm_event_t q[EVQ];
    volatile int qh, qt;
    bool pressed;
    int hover_x, hover_y;
    uint32_t* row;                   /* scaled-row scratch */
    int row_cap;                     /* pixels in row */
    uint32_t  buf;                   /* user address of the pixels it presents */
    ovl_t     ovl_next[OVL_MAX];     /* text of the frame being drawn */
    int       n_next;
    ovl_t     ovl[2][OVL_MAX];       /* presented frames (double-buffered) */
    int       n_ovl[2];
    volatile int ovl_cur;
} uwin_t;

static uwin_t U[UWIN_MAX];
static volatile bool wm_up;

#define EFAULT 14
#define EINVAL 22
#define ENODEV 19
#define ENOMEM 12
#define EBADF   9
#define EPIPE  32
#define EINTR   4
#define EAGAIN 11

static bool uok(uint32_t a, uint32_t len) {
    return a >= USER_BASE && a < USER_TOP && len <= USER_TOP - a;
}

/* NUL-terminated user string into a kernel buffer; UTF-8 Cyrillic is
   folded to CP866 (the font's encoding), raw CP866 bytes pass through. */
static bool ustr(uint32_t p, char* out, int cap) {
    if (!uok(p, 1)) return false;
    const uint8_t* s = (const uint8_t*)p;
    int n = 0;
    for (uint32_t i = 0; n < cap - 1; i++) {
        if (!uok(p + i, 1)) break;
        uint8_t c = s[i];
        if (!c) break;
        if ((c == 0xD0 || c == 0xD1) && uok(p + i + 1, 1) && (s[i + 1] & 0xC0) == 0x80) {
            uint32_t u = ((uint32_t)(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            i++;
            if (u >= 0x410 && u <= 0x43F) c = (uint8_t)(0x80 + (u - 0x410));
            else if (u >= 0x440 && u <= 0x44F) c = (uint8_t)(0xE0 + (u - 0x440));
            else if (u == 0x401) c = 0xF0;
            else if (u == 0x451) c = 0xF1;
            else c = '?';
        } else if (c == 0xC2 && uok(p + i + 1, 1) && (s[i + 1] & 0xC0) == 0x80) {
            uint8_t d = s[++i];
            c = d == 0xB0 ? 0xF8 : d == 0xB7 ? 0xFA : d == 0xA0 ? ' ' : '?';
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return true;
}

static uwin_t* by_handle(uint32_t h) {
    proc_t* p = proc_current();
    if (h >= UWIN_MAX || !p) return NULL;
    uwin_t* u = &U[h];
    if (u->state == U_FREE || u->pid != p->pid) return NULL;
    return u;
}

static void push(uwin_t* u, int type, int a, int b, int c) {
    int next = (u->qt + 1) % EVQ;
    if (next == u->qh) return;                      /* full: drop */
    u->q[u->qt] = (sm_event_t){ type, a, b, c };
    u->qt = next;
}

static uwin_t* of(window_t* w) { return (uwin_t*)w->user; }

/* ---------------- WM callbacks (WM task) ---------------- */

/* Where the image sits in the window: the largest integer scale that fits
   the client area (the window may have been resized or maximized),
   centred. Screen coordinates. */
static void u_geom(uwin_t* u, window_t* w, int* ox, int* oy, int* sc) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    int s = cw / u->w, t = ch / u->h;
    if (t < s) s = t;
    if (s > 8) s = 8;
    if (s < 1) s = 1;
    *sc = s;
    *ox = cx + (cw - u->w * s) / 2;
    *oy = cy + (ch - u->h * s) / 2;
    if (*ox < cx) *ox = cx;
    if (*oy < cy) *oy = cy;
}

/* Screen point -> image pixel (clamped to the image). */
static void u_map(uwin_t* u, window_t* w, int mx, int my, int* x, int* y) {
    int ox, oy, s;
    u_geom(u, w, &ox, &oy, &s);
    int px = mx - ox, py = my - oy;
    *x = (px < 0 ? 0 : px / s);
    *y = (py < 0 ? 0 : py / s);
    if (*x >= u->w) *x = u->w - 1;
    if (*y >= u->h) *y = u->h - 1;
}

static void u_paint(window_t* w) {
    uwin_t* u = of(w);
    int cx, cy, cw, ch, ox, oy, s;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    if (!u->pix) return;
    u_geom(u, w, &ox, &oy, &s);
    int iw = u->w * s, ih = u->h * s;
    /* letterbox bands */
    const uint32_t bar = 0x000000;
    if (oy > cy)                gfx_rect_fill(cx, cy, cw, oy - cy, bar);
    if (oy + ih < cy + ch)      gfx_rect_fill(cx, oy + ih, cw, cy + ch - oy - ih, bar);
    if (ox > cx)                gfx_rect_fill(cx, oy, ox - cx, ih, bar);
    if (ox + iw < cx + cw)      gfx_rect_fill(ox + iw, oy, cx + cw - ox - iw, ih, bar);
    if (s == 1) {
        gfx_blit_argb(ox, oy, u->w, u->h, u->pix);
    } else {
        if (iw > u->row_cap) return;
        for (int y = 0; y < u->h; y++) {
            const uint32_t* src = u->pix + y * u->w;
            for (int x = 0; x < u->w; x++)
                for (int k = 0; k < s; k++) u->row[x * s + k] = src[x];
            for (int k = 0; k < s; k++) gfx_blit_argb(ox, oy + y * s + k, iw, 1, u->row);
        }
    }
    /* the frame's text, crisp at this scale, clipped to the image */
    int cur = u->ovl_cur, n = u->n_ovl[cur];
    if (!n) return;
    int kx, ky, kw, kh;
    gfx_get_clip(&kx, &ky, &kw, &kh);
    int x0 = ox > kx ? ox : kx, y0 = oy > ky ? oy : ky;
    int x1 = ox + iw < kx + kw ? ox + iw : kx + kw, y1 = oy + ih < ky + kh ? oy + ih : ky + kh;
    if (x1 <= x0 || y1 <= y0) return;
    gfx_set_clip(x0, y0, x1 - x0, y1 - y0);
    for (int i = 0; i < n; i++) {
        const ovl_t* o = &u->ovl[cur][i];
        uif_draw_scaled(ox + o->x * s, oy + o->y * s, o->font, o->s, o->color, s);
    }
    gfx_set_clip(kx, ky, kw, kh);
}

/* SM_OP_TEXT aimed at a window's own pixel buffer (the one it presents):
   record it instead of rasterizing at window resolution. */
static uwin_t* text_target(uint32_t buf, int bw, int bh) {
    proc_t* p = proc_current();
    if (!p || !buf) return NULL;
    for (int i = 0; i < UWIN_MAX; i++) {
        uwin_t* u = &U[i];
        if (u->state == U_OPEN && u->pid == p->pid && u->buf == buf && u->w == bw && u->h == bh)
            return u;
    }
    return NULL;
}

static void u_key(window_t* w, char c) { push(of(w), SM_EV_KEY, (uint8_t)c, 0, 0); }

static void u_click(window_t* w, int rx, int ry) {
    uwin_t* u = of(w);
    int cx, cy, cw, ch, x, y;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    u_map(u, w, cx + rx, cy + ry, &x, &y);
    if (!u->pressed) { u->pressed = true; push(u, SM_EV_MOUSE_DOWN, x, y, 1); }
    else if (x != u->hover_x || y != u->hover_y) push(u, SM_EV_MOUSE_MOVE, x, y, 1);
    u->hover_x = x; u->hover_y = y;
}

static void u_release(window_t* w) {
    uwin_t* u = of(w);
    u->pressed = false;
    push(u, SM_EV_MOUSE_UP, u->hover_x, u->hover_y, 0);
}

static void u_tick(window_t* w, uint32_t now) {
    (void)now;
    uwin_t* u = of(w);
    if (u->title_dirty) {
        strncpy(w->title, u->title, 63);
        w->title[63] = 0;
        u->title_dirty = false;
    }
    if (u->pressed || wm_focused() != w) return;
    int cx, cy, cw, ch, mx, my;
    uint8_t b;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    mouse_get(&mx, &my, &b);
    if (mx < cx || my < cy || mx >= cx + cw || my >= cy + ch) return;
    int x, y;
    u_map(u, w, mx, my, &x, &y);
    if (x != u->hover_x || y != u->hover_y) {
        u->hover_x = x; u->hover_y = y;
        push(u, SM_EV_MOUSE_MOVE, x, y, 0);
    }
}

static void u_scroll(window_t* w, int dz) {
    uwin_t* u = of(w);
    push(u, SM_EV_WHEEL, dz, u->hover_x, u->hover_y);
}

static void u_close(window_t* w) {
    uwin_t* u = of(w);
    u->win = NULL;
    u->gone = true;
    push(u, SM_EV_CLOSE, 0, 0, 0);
}

static void release_slot(uwin_t* u) {
    if (u->pix) kfree(u->pix);
    if (u->row) kfree(u->row);
    u->pix = u->row = NULL;
    u->win = NULL;
    u->state = U_FREE;
}

static int cascade;

void uwin_wm_frame(void) {
    wm_up = true;
    for (int i = 0; i < UWIN_MAX; i++) {
        uwin_t* u = &U[i];
        if (u->state == U_PENDING) {
            int ww = u->w * u->scale + 8, wh = u->h * u->scale + WM_TITLE_H + 6;
            int x = (gfx_w() - ww) / 2 + cascade * 24, y = (gfx_h() - 40 - wh) / 2 + cascade * 24;
            cascade = (cascade + 1) % 6;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            window_t* w = wm_open_app_ex(x, y, ww, wh, u->title, u_paint, u_key, u_click,
                                         u_tick, false, u);
            if (!w) { u->state = U_FAILED; continue; }
            w->on_release = u_release;
            w->on_close = u_close;
            w->on_scroll = u_scroll;
            w->opaque = true;                    /* u_paint fills image + letterbox */
            w->min_w = u->w + 8;                 /* never below 1:1 */
            w->min_h = u->h + WM_TITLE_H + 6;
            u->win = w;
            u->state = U_OPEN;
        } else if (u->state == U_CLOSING) {
            if (u->win) {
                window_t* w = u->win;
                w->on_close = NULL;           /* nobody is listening any more */
                wm_close(w);
            }
            release_slot(u);
        }
    }
}

void uwin_wm_exit(void) {
    wm_up = false;
    for (int i = 0; i < UWIN_MAX; i++) {
        uwin_t* u = &U[i];
        if (u->state == U_OPEN && u->win) { u->win = NULL; u->gone = true; push(u, SM_EV_CLOSE, 0, 0, 0); }
        if (u->state == U_PENDING) u->state = U_FAILED;
        if (u->state == U_CLOSING) release_slot(u);
    }
}

bool uwin_wm_running(void) { return wm_up; }

bool uwin_pid_has_window(int pid) {
    for (int i = 0; i < UWIN_MAX; i++)
        if (U[i].state == U_OPEN && U[i].pid == pid) return true;
    return false;
}

void uwin_proc_exit(int pid) {
    for (int i = 0; i < UWIN_MAX; i++) {
        uwin_t* u = &U[i];
        if (u->state == U_FREE || u->pid != pid) continue;
        if (wm_up) u->state = U_CLOSING;      /* WM closes + frees next frame */
        else       release_slot(u);
    }
}

/* ---------------- syscalls (process task, IRQs off) ---------------- */

static int32_t op_open(uint32_t a) {
    if (!uok(a, sizeof(sm_open_t))) return -EFAULT;
    if (!wm_up) return -ENODEV;
    proc_t* p = proc_current();
    if (!p) return -EINVAL;
    sm_open_t o = *(const sm_open_t*)a;
    if (o.scale < 1) o.scale = 1;
    if (o.scale > 8) o.scale = 8;
    if (o.w < 16 || o.h < 16 || o.w * o.h > MAX_PIXELS) return -EINVAL;
    if (o.w * o.scale + 8 > gfx_w() || o.h * o.scale + WM_TITLE_H + 6 > gfx_h() - 40) return -EINVAL;
    int slot = -1;
    for (int i = 0; i < UWIN_MAX; i++) if (U[i].state == U_FREE) { slot = i; break; }
    if (slot < 0) return -ENOMEM;
    uwin_t* u = &U[slot];
    memset(u, 0, sizeof(*u));
    u->pid = p->pid;
    u->w = o.w; u->h = o.h; u->scale = o.scale;
    u->hover_x = u->hover_y = -1;
    if (!o.title || !ustr(o.title, u->title, sizeof(u->title))) strcpy(u->title, p->name);
    u->pix = (uint32_t*)kmalloc((size_t)o.w * o.h * 4);
    /* Wide enough for any scale a maximized window can reach. */
    u->row_cap = gfx_w() > o.w * o.scale ? gfx_w() : o.w * o.scale;
    u->row = (uint32_t*)kmalloc((size_t)u->row_cap * 4);
    if (!u->pix || !u->row) { release_slot(u); return -ENOMEM; }
    memset(u->pix, 0, (size_t)o.w * o.h * 4);
    u->state = U_PENDING;
    /* The WM creates it on its next frame. */
    uint32_t t0 = pit_uptime_ms();
    while (u->state == U_PENDING) {
        if (proc_interrupted() || pit_uptime_ms() - t0 > 3000) { u->state = U_CLOSING; return -EINTR; }
        task_sleep_ms(5);
    }
    if (u->state != U_OPEN) { release_slot(u); return -ENODEV; }
    return slot;
}

static int32_t op_event(uwin_t* u, uint32_t ev, int32_t timeout) {
    if (!uok(ev, sizeof(sm_event_t))) return -EFAULT;
    uint32_t t0 = pit_uptime_ms();
    while (u->qh == u->qt) {
        if (timeout == 0 || (timeout > 0 && (int32_t)(pit_uptime_ms() - t0) >= timeout)) return 0;
        if (proc_interrupted()) return -EINTR;
        task_sleep_ms(4);
    }
    *(sm_event_t*)ev = u->q[u->qh];
    u->qh = (u->qh + 1) % EVQ;
    return 1;
}

int32_t uwin_syscall(uint32_t op, uint32_t a, uint32_t b, uint32_t c) {
    if (op == SM_OP_OPEN) return op_open(a);
    if (op == SM_OP_FONT_H) return uif_height_any((int)a);
    if (op == SM_OP_TEXT) {
        if (!uok(a, sizeof(sm_text_t))) return -EFAULT;
        sm_text_t t = *(const sm_text_t*)a;
        char s[512];
        if (!ustr(t.str, s, sizeof(s))) return -EFAULT;
        if (!t.buf) return uif_width_any(t.font, s);
        uwin_t* tu = text_target(t.buf, t.bw, t.bh);
        if (tu) {
            if (tu->n_next < OVL_MAX) {
                ovl_t* o = &tu->ovl_next[tu->n_next++];
                o->x = (int16_t)t.x; o->y = (int16_t)t.y;
                o->font = (int16_t)t.font; o->color = t.color;
                strncpy(o->s, s, OVL_STR - 1);
                o->s[OVL_STR - 1] = 0;
            }
            return t.x + uif_width_any(t.font, s);
        }
        if (t.bw <= 0 || t.bh <= 0 || t.bw * t.bh > MAX_PIXELS ||
            !uok(t.buf, (uint32_t)t.bw * t.bh * 4)) return -EFAULT;
        return uif_draw_mem((uint32_t*)t.buf, t.bw, t.bh, t.x, t.y, t.font, s, t.color);
    }

    uwin_t* u = by_handle(a);
    if (!u) return -EBADF;
    switch (op) {
    case SM_OP_PRESENT:
        if (!uok(b, (uint32_t)u->w * u->h * 4)) return -EFAULT;
        if (u->gone) return -EPIPE;
        if (u->state != U_OPEN) return -EAGAIN;
        memcpy(u->pix, (const void*)b, (size_t)u->w * u->h * 4);
        if (u->buf == b) {                    /* frame's text goes live with its pixels */
            int nx = 1 - u->ovl_cur;
            memcpy(u->ovl[nx], u->ovl_next, (size_t)u->n_next * sizeof(ovl_t));
            u->n_ovl[nx] = u->n_next;
            u->ovl_cur = nx;
        }
        u->n_next = 0;
        u->buf = b;       /* text drawn into this buffer from now on becomes overlay */
        if (u->win) u->win->needs_repaint = true;
        return 0;
    case SM_OP_EVENT:
        return op_event(u, b, (int32_t)c);
    case SM_OP_CLOSE:
        if (wm_up) u->state = U_CLOSING; else release_slot(u);
        return 0;
    case SM_OP_KEYS: {
        if (!uok(b, 32)) return -EFAULT;
        uint8_t* out = (uint8_t*)b;
        if (u->win && wm_focused() == u->win) kbd_key_bits(out);
        else memset(out, 0, 32);
        return 0;
    }
    case SM_OP_MOUSE: {
        if (!uok(b, 12)) return -EFAULT;
        int32_t* out = (int32_t*)b;
        out[0] = u->hover_x; out[1] = u->hover_y; out[2] = 0;
        if (!u->win) return 0;
        int cx, cy, cw, ch, mx, my;
        uint8_t btn;
        wm_client_rect(u->win, &cx, &cy, &cw, &ch);
        mouse_get(&mx, &my, &btn);
        int ox, oy, sc;
        u_geom(u, u->win, &ox, &oy, &sc);
        bool inside = mx >= ox && my >= oy && mx < ox + u->w * sc && my < oy + u->h * sc;
        out[0] = (mx - ox) / sc;
        out[1] = (my - oy) / sc;
        out[2] = wm_focused() == u->win ? btn : 0;
        return inside ? 1 : 0;
    }
    case SM_OP_TITLE:
        if (!ustr(b, u->title, sizeof(u->title))) return -EFAULT;
        u->title_dirty = true;
        return 0;
    }
    return -EINVAL;
}
