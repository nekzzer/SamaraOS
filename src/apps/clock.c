/* Analog + digital clock widget. */

#include "apps/clock.h"
#include "gui/wm.h"
#include "gfx/gfx.h"
#include "boot/pit.h"
#include "core/io.h"
#include "core/string.h"
#include "drivers/keyboard.h"

static window_t* g_clock_win = NULL;

/* Bhaskara I sine approximation. Returns sin(a_deg) * 1024 (Q10). */
static int sin_q10(int a) {
    a %= 360;
    if (a < 0) a += 360;
    int sign = 1;
    if (a >= 180) { a -= 180; sign = -1; }
    int x = a * (180 - a);
    int num = 4 * x;
    int den = 40500 - x;
    if (den == 0) den = 1;
    return sign * (num * 1024 / den);
}
static int cos_q10(int a) { return sin_q10(a + 90); }

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xF)); }

static void read_rtc(int* h, int* m, int* s) {
    do { outb(0x70, 0x0A); } while (inb(0x71) & 0x80);
    outb(0x70, 0x00); uint8_t ss = inb(0x71);
    outb(0x70, 0x02); uint8_t mm = inb(0x71);
    outb(0x70, 0x04); uint8_t hh = inb(0x71);
    outb(0x70, 0x0B); uint8_t b  = inb(0x71);
    if (!(b & 0x04)) { ss = bcd2bin(ss); mm = bcd2bin(mm); hh = bcd2bin(hh); }
    *h = hh; *m = mm; *s = ss;
}

#define BG  RGB(0x14, 0x18, 0x28)
#define FACE RGB(0xF4, 0xF4, 0xF8)
#define FRAME RGB(0x6E, 0xA8, 0xFE)
#define MARK RGB(0x20, 0x22, 0x32)
#define HOUR_C RGB(0x10, 0x14, 0x28)
#define MIN_C  RGB(0x2A, 0x36, 0x60)
#define SEC_C  RGB(0xE0, 0x50, 0x50)
#define CENTRE RGB(0x10, 0x14, 0x28)
#define FG     RGB(0xE6, 0xE8, 0xF2)
#define DIM    RGB(0x88, 0x90, 0xB0)

static void draw_hand(int cx, int cy, int len, int deg, int thick, uint32_t col) {
    int s = sin_q10(deg);   /* Q10 */
    int c = -cos_q10(deg);  /* y axis points down → invert */
    int ex = cx + (len * s) / 1024;
    int ey = cy + (len * c) / 1024;
    /* perpendicular offset for thickness */
    int ps = -c;
    int pc =  s;
    int half = thick / 2;
    for (int k = -half; k <= half; k++) {
        int ox = (k * pc) / 1024;
        int oy = (k * ps) / 1024;
        gfx_line(cx + ox, cy + oy, ex + ox, ey + oy, col);
    }
}

static void cl_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_rect_fill(cx, cy, cw, ch, BG);

    /* Clock face */
    int side = (cw < ch ? cw : ch) - 60;
    if (side < 100) side = 100;
    int r   = side / 2;
    int ccx = cx + cw / 2;
    int ccy = cy + (ch - 50) / 2 + 4;

    /* face */
    gfx_disc(ccx, ccy, r, FACE);
    /* bezel */
    for (int i = 0; i < 3; i++) gfx_circle(ccx, ccy, r + i, FRAME);

    /* tick marks */
    for (int i = 0; i < 60; i++) {
        int deg = i * 6;
        int s = sin_q10(deg);
        int c = -cos_q10(deg);
        int x0 = ccx + ((r - 4)  * s) / 1024;
        int y0 = ccy + ((r - 4)  * c) / 1024;
        int x1 = ccx + ((r - ((i % 5 == 0) ? 16 : 8)) * s) / 1024;
        int y1 = ccy + ((r - ((i % 5 == 0) ? 16 : 8)) * c) / 1024;
        gfx_line(x0, y0, x1, y1, MARK);
    }

    /* hour numerals (1..12) */
    for (int i = 1; i <= 12; i++) {
        int deg = i * 30;
        int s = sin_q10(deg), c = -cos_q10(deg);
        int x = ccx + ((r - 30) * s) / 1024 - (i < 10 ? 4 : 8);
        int y = ccy + ((r - 30) * c) / 1024 - 8;
        char buf[4];
        itoa(i, buf, 10);
        gfx_string(x, y, buf, MARK, FACE, false);
    }

    int hh, mm, ss;
    read_rtc(&hh, &mm, &ss);
    int hour_deg = (hh % 12) * 30 + mm / 2;
    int min_deg  = mm * 6 + ss / 10;
    int sec_deg  = ss * 6;

    draw_hand(ccx, ccy, r - 50, hour_deg, 6, HOUR_C);
    draw_hand(ccx, ccy, r - 24, min_deg,  4, MIN_C);
    draw_hand(ccx, ccy, r - 14, sec_deg,  2, SEC_C);

    /* central hub */
    gfx_disc(ccx, ccy, 6, CENTRE);
    gfx_disc(ccx, ccy, 3, SEC_C);

    /* digital readout below face */
    char d[16]; int p = 0; char t[6];
    if (hh < 10) d[p++] = '0';
    itoa(hh, t, 10); for (int i = 0; t[i]; i++) d[p++] = t[i];
    d[p++] = ':';
    if (mm < 10) d[p++] = '0';
    itoa(mm, t, 10); for (int i = 0; t[i]; i++) d[p++] = t[i];
    d[p++] = ':';
    if (ss < 10) d[p++] = '0';
    itoa(ss, t, 10); for (int i = 0; t[i]; i++) d[p++] = t[i];
    d[p] = 0;

    int dw = (int)strlen(d) * 8;
    gfx_string(cx + (cw - dw) / 2, cy + ch - 24, d, FG, BG, false);
    const char* hint = "Esc closes window";
    gfx_string(cx + (cw - (int)strlen(hint) * 8) / 2, cy + ch - 10, hint, DIM, BG, false);
}

static void cl_tick(window_t* w, uint32_t now) {
    (void)w; (void)now;
    /* repaint forced every frame (animate=true) — keeps second-hand smooth */
}

static void cl_key(window_t* w, char c) {
    if (c == 0x1B) wm_close(w);
}

int clock_open(void) {
    if (!gfx_ready()) return -1;
    if (g_clock_win && g_clock_win->open) return 0;
    int W = gfx_w(), H = gfx_h();
    int ww = 360, wh = 400;
    if (ww > W - 40) ww = W - 40;
    if (wh > H - 80) wh = H - 80;
    int wx = (W - ww) / 2 + 220;
    int wy = (H - wh) / 2 - 20;
    if (wx + ww > W) wx = W - ww - 8;
    if (wy < 30) wy = 30;
    g_clock_win = wm_open_app_ex(wx, wy, ww, wh, "Clock - SamaraOS",
                                  cl_paint, cl_key, NULL, cl_tick,
                                  true, NULL);
    return g_clock_win ? 0 : -1;
}
