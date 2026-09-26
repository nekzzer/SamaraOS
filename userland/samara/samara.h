/* samara.h — desktop windows for SamaraOS user programs (header-only).
 *
 * Works with TCC inside SamaraOS (`tcc prog.c -o prog`) and with
 * i686-linux-musl-gcc -static on the host. Everything goes through one
 * syscall, 500 ("samara"); see src/gui/uwin.h in the kernel tree.
 *
 * The pixel buffer (XRGB, 0x00RRGGBB) lives in the program: draw into
 * win->pix with the sm_* helpers (or by hand), then sm_present(). The WM
 * shows it, optionally pixel-scaled, and queues input events for sm_event().
 *
 *     SmWin *w = sm_open(320, 200, 2, "Hello");
 *     SmEvent e;
 *     for (;;) {
 *         while (sm_event(w, &e, 0) > 0) if (e.type == SM_EV_CLOSE) return 0;
 *         sm_clear(w, SM_BLACK);
 *         sm_text(w, 10, 10, "Привет!", SM_WHITE, SM_FONT_BIG);
 *         if (sm_present(w) < 0) break;         // window closed
 *         sm_sleep_ms(16);
 *     }
 *     sm_close(w);
 */
#ifndef SAMARA_H
#define SAMARA_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_SAMARA 500

enum {
    SM_OP_OPEN = 1, SM_OP_PRESENT, SM_OP_EVENT, SM_OP_CLOSE, SM_OP_TEXT,
    SM_OP_KEYS, SM_OP_MOUSE, SM_OP_TITLE, SM_OP_FONT_H
};

/* ---- events ---- */
enum { SM_EV_NONE, SM_EV_KEY, SM_EV_MOUSE_DOWN, SM_EV_MOUSE_UP, SM_EV_MOUSE_MOVE, SM_EV_CLOSE,
       SM_EV_WHEEL };

typedef struct { int32_t type, a, b, c; } SmEvent;
/* SM_EV_KEY:        a = character (CP866; specials below)
   SM_EV_MOUSE_*:    a = x, b = y (window pixels, scale already divided), c = button
   SM_EV_CLOSE:      the user clicked the window's close button
   SM_EV_WHEEL:      a = notches (> 0 = scrolled down / towards the user),
                     b, c = pointer x, y in window pixels */

/* Characters delivered by SM_EV_KEY (kernel drivers/keyboard.h) */
#define SM_CH_UP     0x81
#define SM_CH_DOWN   0x82
#define SM_CH_LEFT   0x83
#define SM_CH_RIGHT  0x84
#define SM_CH_HOME   0x85
#define SM_CH_END    0x86
#define SM_CH_DEL    0x87
#define SM_CH_PGUP   0x88
#define SM_CH_PGDN   0x89
#define SM_CH_F1     0x90             /* F1..F10 = SM_CH_F1 + 0..9 */
#define SM_CH_ESC    0x1B
#define SM_CH_ENTER  '\n'
#define SM_CH_BACK   '\b'

/* Held-key bitmap (sm_keys / sm_key_down): Linux key codes */
#define SMK_ESC    1
#define SMK_1      2
#define SMK_2      3
#define SMK_3      4
#define SMK_4      5
#define SMK_5      6
#define SMK_6      7
#define SMK_7      8
#define SMK_8      9
#define SMK_9      10
#define SMK_0      11
#define SMK_BACKSPACE 14
#define SMK_TAB    15
#define SMK_Q      16
#define SMK_W      17
#define SMK_E      18
#define SMK_R      19
#define SMK_T      20
#define SMK_Y      21
#define SMK_U      22
#define SMK_I      23
#define SMK_O      24
#define SMK_P      25
#define SMK_ENTER  28
#define SMK_LCTRL  29
#define SMK_A      30
#define SMK_S      31
#define SMK_D      32
#define SMK_F      33
#define SMK_G      34
#define SMK_H      35
#define SMK_J      36
#define SMK_K      37
#define SMK_L      38
#define SMK_LSHIFT 42
#define SMK_Z      44
#define SMK_X      45
#define SMK_C      46
#define SMK_V      47
#define SMK_B      48
#define SMK_N      49
#define SMK_M      50
#define SMK_RSHIFT 54
#define SMK_LALT   56
#define SMK_SPACE  57
#define SMK_UP     103
#define SMK_PGUP   104
#define SMK_LEFT   105
#define SMK_RIGHT  106
#define SMK_DOWN   108
#define SMK_PGDN   109

/* ---- fonts for sm_text (kernel gfx/uifont.h) ---- */
#define SM_FONT_REG    0              /* Golos 15px regular  */
#define SM_FONT_MED    1              /* Golos 15px semibold */
#define SM_FONT_SMALL  2              /* Golos 12px medium   */
#define SM_FONT_BIG    3              /* Golos 20px bold     */
#define SM_FONT_HUGE   4              /* Golos 84px (big digits/latin) */
#define SM_FONT_MONO   16             /* 8x16 terminal face  */

/* ---- colours ---- */
#define SM_RGB(r, g, b) ((uint32_t)((((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF)))
#define SM_BLACK   SM_RGB(0, 0, 0)
#define SM_WHITE   SM_RGB(255, 255, 255)
#define SM_GRAY    SM_RGB(128, 128, 128)
#define SM_DARK    SM_RGB(0x1D, 0x1E, 0x21)
#define SM_PAPER   SM_RGB(0xEE, 0xEB, 0xE5)
#define SM_RED     SM_RGB(0xCF, 0x4A, 0x3E)
#define SM_GREEN   SM_RGB(0x74, 0xB0, 0x5E)
#define SM_BLUE    SM_RGB(0x3E, 0x7C, 0xCF)
#define SM_YELLOW  SM_RGB(0xF2, 0xC9, 0x4C)
#define SM_ORANGE  SM_RGB(0xE3, 0x9B, 0x32)
#define SM_CYAN    SM_RGB(0x4C, 0xC2, 0xD2)
#define SM_MAGENTA SM_RGB(0xB0, 0x5E, 0xC8)

/* ---- kernel ABI structs ---- */
typedef struct { int32_t w, h, scale, flags; uint32_t title; } sm_open_t;
typedef struct { uint32_t buf; int32_t bw, bh, x, y, font; uint32_t color, str; } sm_text_t;

typedef struct {
    int w, h, scale;
    int handle;
    uint32_t *pix;                   /* w*h XRGB, owned by the program */
} SmWin;

static inline long sm_call(long op, long a, long b, long c) { return syscall(SYS_SAMARA, op, a, b, c); }

/* ---- time ---- */
static inline uint32_t sm_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

static inline void sm_sleep_ms(int ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

/* ---- window lifecycle ---- */

/* Opens a w x h window shown at `scale`x (1..8). NULL outside the desktop
   (start `desktop` first) or when out of window slots. */
static inline SmWin *sm_open(int w, int h, int scale, const char *title) {
    sm_open_t o;
    SmWin *win;
    long r;
    o.w = w; o.h = h; o.scale = scale; o.flags = 0;
    o.title = (uint32_t)(uintptr_t)title;
    r = sm_call(SM_OP_OPEN, (long)&o, 0, 0);
    if (r < 0) return 0;
    win = (SmWin *)malloc(sizeof(SmWin));
    if (!win) { sm_call(SM_OP_CLOSE, r, 0, 0); return 0; }
    win->w = w; win->h = h; win->scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    win->handle = (int)r;
    win->pix = (uint32_t *)calloc((size_t)w * h, 4);
    if (!win->pix) { sm_call(SM_OP_CLOSE, r, 0, 0); free(win); return 0; }
    return win;
}

/* Copies the buffer to the screen. <0 (-EPIPE) once the window is closed. */
static inline int sm_present(SmWin *w) { return (int)sm_call(SM_OP_PRESENT, w->handle, (long)w->pix, 0); }

/* 1 = event in *e, 0 = timeout, <0 = interrupted. timeout_ms -1 waits. */
static inline int sm_event(SmWin *w, SmEvent *e, int timeout_ms) {
    return (int)sm_call(SM_OP_EVENT, w->handle, (long)e, timeout_ms);
}

static inline void sm_close(SmWin *w) {
    if (!w) return;
    sm_call(SM_OP_CLOSE, w->handle, 0, 0);
    free(w->pix);
    free(w);
}

static inline void sm_title(SmWin *w, const char *t) { sm_call(SM_OP_TITLE, w->handle, (long)t, 0); }

/* 32-byte bitmap of held keys (Linux codes, SMK_*); zero unless focused. */
static inline void sm_keys(SmWin *w, uint8_t bits[32]) { sm_call(SM_OP_KEYS, w->handle, (long)bits, 0); }
static inline int sm_key_down(const uint8_t bits[32], int code) {
    return code > 0 && code < 256 && (bits[code >> 3] >> (code & 7)) & 1;
}

/* Mouse in window pixels; returns 1 if the pointer is inside the window. */
static inline int sm_mouse(SmWin *w, int *x, int *y, int *buttons) {
    int32_t m[3];
    int r = (int)sm_call(SM_OP_MOUSE, w->handle, (long)m, 0);
    if (x) *x = m[0];
    if (y) *y = m[1];
    if (buttons) *buttons = m[2];
    return r;
}

static inline int sm_font_h(int font) { return (int)sm_call(SM_OP_FONT_H, font, 0, 0); }

/* ---- drawing into the buffer ---- */

static inline void sm_pixel(SmWin *w, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)w->w && (unsigned)y < (unsigned)w->h) w->pix[y * w->w + x] = c;
}

static inline uint32_t sm_get(SmWin *w, int x, int y) {
    if ((unsigned)x < (unsigned)w->w && (unsigned)y < (unsigned)w->h) return w->pix[y * w->w + x];
    return 0;
}

static inline void sm_rect(SmWin *w, int x, int y, int rw, int rh, uint32_t c) {
    int x1 = x + rw, y1 = y + rh, i, j;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > w->w) x1 = w->w;
    if (y1 > w->h) y1 = w->h;
    for (j = y; j < y1; j++) {
        uint32_t *p = w->pix + j * w->w;
        for (i = x; i < x1; i++) p[i] = c;
    }
}

static inline void sm_clear(SmWin *w, uint32_t c) { sm_rect(w, 0, 0, w->w, w->h, c); }
static inline void sm_hline(SmWin *w, int x, int y, int len, uint32_t c) { sm_rect(w, x, y, len, 1, c); }
static inline void sm_vline(SmWin *w, int x, int y, int len, uint32_t c) { sm_rect(w, x, y, 1, len, c); }

static inline void sm_frame(SmWin *w, int x, int y, int rw, int rh, uint32_t c) {
    if (rw <= 0 || rh <= 0) return;
    sm_hline(w, x, y, rw, c);
    sm_hline(w, x, y + rh - 1, rw, c);
    sm_vline(w, x, y, rh, c);
    sm_vline(w, x + rw - 1, y, rh, c);
}

static inline void sm_line(SmWin *w, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        sm_pixel(w, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static inline void sm_circle(SmWin *w, int cx, int cy, int r, uint32_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        sm_pixel(w, cx + x, cy + y, c); sm_pixel(w, cx - x, cy + y, c);
        sm_pixel(w, cx + x, cy - y, c); sm_pixel(w, cx - x, cy - y, c);
        sm_pixel(w, cx + y, cy + x, c); sm_pixel(w, cx - y, cy + x, c);
        sm_pixel(w, cx + y, cy - x, c); sm_pixel(w, cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static inline void sm_disc(SmWin *w, int cx, int cy, int r, uint32_t c) {
    int y, x;
    for (y = -r; y <= r; y++) {
        x = 0;
        while ((x + 1) * (x + 1) + y * y <= r * r) x++;
        sm_hline(w, cx - x, cy + y, 2 * x + 1, c);
    }
}

/* Text is rasterized by the kernel with the desktop fonts (anti-aliased,
   blended over what is already in the buffer). y is the top of the line.
   UTF-8 Cyrillic is accepted. Returns the pen x after the text. */
static inline int sm_text(SmWin *w, int x, int y, const char *s, uint32_t color, int font) {
    sm_text_t t;
    t.buf = (uint32_t)(uintptr_t)w->pix; t.bw = w->w; t.bh = w->h;
    t.x = x; t.y = y; t.font = font; t.color = color;
    t.str = (uint32_t)(uintptr_t)s;
    return (int)sm_call(SM_OP_TEXT, (long)&t, 0, 0);
}

static inline int sm_text_w(const char *s, int font) {
    sm_text_t t;
    memset(&t, 0, sizeof t);
    t.font = font;
    t.str = (uint32_t)(uintptr_t)s;
    return (int)sm_call(SM_OP_TEXT, (long)&t, 0, 0);
}

#endif
