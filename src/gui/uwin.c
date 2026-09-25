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

static void u_paint(window_t* w) {
    uwin_t* u = of(w);
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    if (!u->pix) return;
    if (u->scale <= 1) { gfx_blit_argb(cx, cy, u->w, u->h, u->pix); return; }
    int s = u->scale;
    for (int y = 0; y < u->h; y++) {
        const uint32_t* src = u->pix + y * u->w;
        for (int x = 0; x < u->w; x++)
            for (int k = 0; k < s; k++) u->row[x * s + k] = src[x];
        for (int k = 0; k < s; k++) gfx_blit_argb(cx, cy + y * s + k, u->w * s, 1, u->row);
    }
}

static void u_key(window_t* w, char c) { push(of(w), SM_EV_KEY, (uint8_t)c, 0, 0); }

static void u_click(window_t* w, int rx, int ry) {
    uwin_t* u = of(w);
    int x = rx / u->scale, y = ry / u->scale;
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
    int x = (mx - cx) / u->scale, y = (my - cy) / u->scale;
    if (x != u->hover_x || y != u->hover_y) {
        u->hover_x = x; u->hover_y = y;
        push(u, SM_EV_MOUSE_MOVE, x, y, 0);
    }
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
    u->row = (uint32_t*)kmalloc((size_t)o.w * o.scale * 4);
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
        bool inside = mx >= cx && my >= cy && mx < cx + cw && my < cy + ch;
        out[0] = (mx - cx) / u->scale;
        out[1] = (my - cy) / u->scale;
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
