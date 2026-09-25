#include "gui/wm.h"
#include "drivers/fbdev.h"
#include "apps/browser.h"
#include "apps/clock.h"
#include "apps/mediaplayer.h"
#include "apps/paint.h"
#include "boot/pit.h"
#include "core/heap.h"
#include "core/io.h"
#include "core/string.h"
#include "core/task.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "gfx/font.h"
#include "gfx/gfx.h"
#include "gfx/gfx_term.h"
#include "gfx/uifont.h"
#include "net/net.h"
#include "shell/shell.h"

#include "gui/theme.h"

/* ========================================================================
   Geometry. The client-rect contract (4px sides, WM_TITLE_H+2 top, 4px
   bottom) is relied on by every app, so only the look changes inside it.
   ======================================================================== */

#define TASKBAR_H  40
#define WIN_R      6
#define MENU_R     8
#define SH_SIZE    16
#define SH_DY      3
#define CUR_W      12
#define CUR_H      19
#define MENU_W     248
#define MENU_HEAD  38
#define MENU_ITEM  30
#define MENU_SEP   13
#define MENU_PAD   6

typedef struct { int x0, y0, x1, y1; } rect_t;

static rect_t R(int x, int y, int w, int h) { rect_t r = {x, y, x + w, y + h}; return r; }
static bool r_hit(rect_t r, int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }
static bool r_overlap(rect_t a, rect_t b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
static rect_t r_union(rect_t a, rect_t b) {
    rect_t r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                 a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}
static rect_t r_isect(rect_t a, rect_t b) {
    rect_t r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
                 a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    return r;
}
static int r_area(rect_t r) { return (r.x1 - r.x0) * (r.y1 - r.y0); }
static void clip_to(rect_t r) { gfx_set_clip(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0); }

/* ========================================================================
   State
   ======================================================================== */

static window_t windows[WM_MAX_WINDOWS];
static uint32_t open_seq[WM_MAX_WINDOWS];      /* taskbar order = open order */
static char     drawn_title[WM_MAX_WINDOWS][64];
static uint32_t seq_counter;
static int  next_z;
static int  focused_idx = -1;
static int  terminal_idx = -1;
static int  press_target_idx = -1;
static bool menu_open;
static bool exit_requested;
static bool db_on;
static bool composing;
static bool first_frame_done;
static bool layout_changed;

static uint32_t* bg_cache;
static int bg_w, bg_h;

/* ---- damage ---- */
#define MAX_DMG 32
static rect_t dmg[MAX_DMG];
static int    n_dmg;

static void damage_rect(rect_t r) {
    r = r_isect(r, R(0, 0, gfx_w(), gfx_h()));
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    if (n_dmg == MAX_DMG) {
        for (int i = 1; i < n_dmg; i++) dmg[0] = r_union(dmg[0], dmg[i]);
        dmg[0] = r_union(dmg[0], r);
        n_dmg = 1;
        return;
    }
    dmg[n_dmg++] = r;
}
static void damage(int x, int y, int w, int h) { damage_rect(R(x, y, w, h)); }
static void damage_all(void) { n_dmg = 0; damage(0, 0, gfx_w(), gfx_h()); }
static void damage_taskbar(void) { damage(0, gfx_h() - TASKBAR_H, gfx_w(), TASKBAR_H); }

void wm_request_redraw(void) { damage_all(); }
void wm_request_exit(void)   { exit_requested = true; }

/* ---- window geometry ---- */

static bool visible(int i) { return windows[i].open && !windows[i].minimized; }

static rect_t win_bounds(const window_t* w) {
    rect_t r = { w->x - SH_SIZE, w->y - SH_SIZE + SH_DY,
                 w->x + w->w + SH_SIZE, w->y + w->h + SH_SIZE + SH_DY };
    return r;
}
static rect_t client_of(window_t* w) {
    int x, y, cw, ch;
    wm_client_rect(w, &x, &y, &cw, &ch);
    return R(x, y, cw, ch);
}
static rect_t close_btn(const window_t* w) { return R(w->x + w->w - 26, w->y + 2, 22, 18); }
static rect_t min_btn(const window_t* w)   { return R(w->x + w->w - 50, w->y + 2, 22, 18); }
static rect_t title_bar(const window_t* w) { return R(w->x, w->y, w->w, WM_TITLE_H + 1); }

void wm_client_rect(window_t* w, int* x, int* y, int* cw, int* ch) {
    *x  = w->x + 4;
    *y  = w->y + WM_TITLE_H + 2;
    *cw = w->w - 8;
    *ch = w->h - WM_TITLE_H - 6;
}

window_t* wm_focused(void) { return focused_idx >= 0 ? &windows[focused_idx] : NULL; }

static int z_sorted(int* out) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (visible(i)) out[n++] = i;
    for (int i = 1; i < n; i++) {
        int v = out[i], j = i - 1;
        while (j >= 0 && windows[out[j]].z > windows[v].z) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

static int hit_window(int mx, int my) {
    int order[WM_MAX_WINDOWS];
    int n = z_sorted(order);
    for (int i = n - 1; i >= 0; i--) {
        window_t* w = &windows[order[i]];
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h)
            return order[i];
    }
    return -1;
}

/* ========================================================================
   Focus / lifecycle
   ======================================================================== */

static void set_focus(int idx) {
    if (idx >= 0 && idx == focused_idx && windows[idx].z == next_z) return;
    if (focused_idx >= 0 && visible(focused_idx)) damage_rect(win_bounds(&windows[focused_idx]));
    focused_idx = idx;
    if (idx >= 0) {
        windows[idx].z = ++next_z;
        damage_rect(win_bounds(&windows[idx]));
    }
    damage_taskbar();
    layout_changed = true;
}

static void focus_topmost(void) {
    int order[WM_MAX_WINDOWS];
    int n = z_sorted(order);
    focused_idx = -1;
    set_focus(n ? order[n - 1] : -1);
}

static void minimize(int idx) {
    window_t* w = &windows[idx];
    damage_rect(win_bounds(w));
    w->minimized = true;
    w->dragging = false;
    if (idx == focused_idx) focus_topmost();
    damage_taskbar();
    layout_changed = true;
}

static void restore(int idx) {
    windows[idx].minimized = false;
    set_focus(idx);
}

static int alloc_slot(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!windows[i].open) return i;
    return -1;
}

static window_t* new_window(int x, int y, int w, int h, const char* title, window_type_t type) {
    int i = alloc_slot();
    if (i < 0) return NULL;
    window_t* win = &windows[i];
    memset(win, 0, sizeof(*win));
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title, 63);
    win->title[63] = 0;
    memcpy(drawn_title[i], win->title, 64);
    win->type = type;
    win->open = true;
    open_seq[i] = ++seq_counter;
    set_focus(i);
    return win;
}

window_t* wm_open_info(int x, int y, int w, int h, const char* title, const char* content) {
    window_t* win = new_window(x, y, w, h, title, WIN_INFO);
    if (win) win->content = content;
    return win;
}

window_t* wm_open_terminal(int x, int y) {
    if (terminal_idx >= 0 && windows[terminal_idx].open) {
        restore(terminal_idx);
        return &windows[terminal_idx];
    }
    window_t* win = new_window(x, y, TERM_COLS * 8 + 8, TERM_ROWS * 16 + WM_TITLE_H + 6,
                               "Terminal", WIN_TERMINAL);
    if (!win) return NULL;
    win->term_x = x + 4;
    win->term_y = y + WM_TITLE_H + 2;
    terminal_idx = (int)(win - windows);
    wm_terminal_init(win);
    return win;
}

window_t* wm_open_app(int x, int y, int w, int h, const char* title,
                      void (*on_paint)(window_t*),
                      void (*on_key)(window_t*, char),
                      void* user) {
    return wm_open_app_ex(x, y, w, h, title, on_paint, on_key, NULL, NULL, true, user);
}

window_t* wm_open_app_ex(int x, int y, int w, int h, const char* title,
                         void (*on_paint)(window_t*),
                         void (*on_key)(window_t*, char),
                         void (*on_click)(window_t*, int, int),
                         void (*on_tick)(window_t*, uint32_t),
                         bool animate,
                         void* user) {
    window_t* win = new_window(x, y, w, h, title, WIN_APP);
    if (!win) return NULL;
    win->on_paint = on_paint;
    win->on_key = on_key;
    win->on_click = on_click;
    win->on_tick = on_tick;
    win->animate = animate;
    win->needs_repaint = true;
    win->user = user;
    return win;
}

void wm_close(window_t* w) {
    if (!w || !w->open) return;
    int idx = (int)(w - windows);
    if (w->on_close) w->on_close(w);
    damage_rect(win_bounds(w));
    damage_taskbar();
    w->open = false;
    w->dragging = false;
    if (idx == terminal_idx) { terminal_idx = -1; wm_terminal_closed(); }
    if (idx == press_target_idx) press_target_idx = -1;
    if (idx == focused_idx) focus_topmost();
    layout_changed = true;
}

/* ========================================================================
   Text helpers — anti-aliased Golos Text (gfx/uifont). `mid` is the
   vertical centre of the capitals, so text sits right in any bar/button.
   ======================================================================== */

static int text(int x, int mid, uif_t f, const char* s, uint32_t c) {
    return uif_draw_mid(x, mid, f, s, c);
}

static void text_fit(int x, int mid, uif_t f, const char* s, int max_px, uint32_t c) {
    if (max_px > 0) uif_draw_fit(x, uif_top_for_mid(f, mid), f, s, max_px, c);
}

static int text_w(uif_t f, const char* s) { return uif_width(f, s); }

/* ========================================================================
   Desktop background — rendered once into a cache: dithered vertical
   gradient, faint dot grid, low-contrast wordmark.
   ======================================================================== */

static const uint8_t bayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static void build_background(int W, int H) {
    if (bg_cache && bg_w == W && bg_h == H) return;
    if (bg_cache) { kfree(bg_cache); bg_cache = NULL; }
    bg_cache = (uint32_t*)kmalloc((size_t)W * (size_t)H * 4);
    if (!bg_cache) { bg_w = bg_h = 0; return; }
    bg_w = W; bg_h = H;

    int tr = (C_DESK_TOP >> 16) & 0xFF, tg = (C_DESK_TOP >> 8) & 0xFF, tb = C_DESK_TOP & 0xFF;
    int br = (C_DESK_BOT >> 16) & 0xFF, bgc = (C_DESK_BOT >> 8) & 0xFF, bb = C_DESK_BOT & 0xFF;
    for (int y = 0; y < H; y++) {
        int r16 = tr * 16 + (br - tr) * 16 * y / H;
        int g16 = tg * 16 + (bgc - tg) * 16 * y / H;
        int b16 = tb * 16 + (bb - tb) * 16 * y / H;
        uint32_t* row = bg_cache + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int t = bayer4[(y & 3) * 4 + (x & 3)];
            row[x] = RGB((r16 + t) >> 4, (g16 + t) >> 4, (b16 + t) >> 4);
        }
    }

    for (int y = 28; y + 1 < H; y += 32)
        for (int x = 28; x + 1 < W; x += 32)
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    uint32_t* p = bg_cache + (size_t)(y + dy) * W + x + dx;
                    *p += 0x070707;
                }

    /* Wordmark in Golos ExtraBold, blended straight into the cache. */
    const char* mark = "samara";
    const uif_face_t* fc = uif_face(UIF_HUGE);
    int mx = W - uif_width(UIF_HUGE, mark) - 56;
    int my = H - fc->ascent - 36;
    int pen = mx << 6;
    int wr = (C_WORDMARK >> 16) & 0xFF, wg = (C_WORDMARK >> 8) & 0xFF, wb = C_WORDMARK & 0xFF;
    for (int i = 0; mark[i]; i++) {
        const uif_glyph_t* g = &fc->gl[(uint8_t)mark[i]];
        int gx = ((pen + 32) >> 6) + g->x, gy = my + g->y;
        for (int r = 0; r < g->h; r++)
            for (int c = 0; c < g->w; c++) {
                int a = fc->px[g->off + r * g->w + c];
                int px = gx + c, py = gy + r;
                if (!a || px < 0 || py < 0 || px >= W || py >= H) continue;
                uint32_t* p = bg_cache + (size_t)py * W + px;
                int dr = (*p >> 16) & 0xFF, dg = (*p >> 8) & 0xFF, db = *p & 0xFF;
                *p = RGB(dr + (wr - dr) * a / 255, dg + (wg - dg) * a / 255, db + (wb - db) * a / 255);
            }
        pen += g->adv;
    }
}

/* ========================================================================
   Clock (CMOS RTC, polled once a second)
   ======================================================================== */

static char clock_time[8] = "--:--";
static char clock_date[12] = "";
static uint32_t next_rtc_ms;

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static int from_bcd(int v) { return (v >> 4) * 10 + (v & 0x0F); }

static void two_digits(char* p, int v) { p[0] = (char)('0' + v / 10 % 10); p[1] = (char)('0' + v % 10); }

/* Returns true if the displayed strings changed. */
static bool rtc_poll(void) {
    while (cmos(0x0A) & 0x80) {}
    int mi = cmos(0x02), h = cmos(0x04), d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    int regb = cmos(0x0B);
    bool pm = (h & 0x80) != 0;
    h &= 0x7F;
    if (!(regb & 0x04)) { mi = from_bcd(mi); h = from_bcd(h); d = from_bcd(d); mo = from_bcd(mo); y = from_bcd(y); }
    if (!(regb & 0x02)) { h %= 12; if (pm) h += 12; }

    char t[8], dt[12];
    two_digits(t, h); t[2] = ':'; two_digits(t + 3, mi); t[5] = 0;
    two_digits(dt, d); dt[2] = '.'; two_digits(dt + 3, mo); dt[5] = '.';
    dt[6] = '2'; dt[7] = '0'; two_digits(dt + 8, y); dt[10] = 0;
    bool changed = strcmp(t, clock_time) != 0 || strcmp(dt, clock_date) != 0;
    memcpy(clock_time, t, sizeof(t));
    memcpy(clock_date, dt, sizeof(dt));
    return changed;
}

static uint32_t net_key;          /* 0 = offline, else IP */

static void ip_string(uint32_t ip, char* out) {
    int p = 0;
    char tmp[8];
    for (int i = 0; i < 4; i++) {
        itoa((int)((ip >> (24 - i * 8)) & 0xFF), tmp, 10);
        for (int j = 0; tmp[j]; j++) out[p++] = tmp[j];
        if (i < 3) out[p++] = '.';
    }
    out[p] = 0;
}

/* ========================================================================
   Hover tracking
   ======================================================================== */

enum { HOV_NONE, HOV_CLOSE, HOV_MIN, HOV_TASK, HOV_START, HOV_LANG, HOV_MENU };
static int hov_kind = HOV_NONE, hov_idx = -1;

static bool hovered(int kind, int idx) { return hov_kind == kind && hov_idx == idx; }

/* ========================================================================
   Taskbar layout (shared by drawing and hit testing)
   ======================================================================== */

static rect_t tb_start, tb_lang, tb_net, tb_clock;
static rect_t tb_btn[WM_MAX_WINDOWS];
static int    tb_btn_win[WM_MAX_WINDOWS];
static int    n_tb;

static void layout_taskbar(void) {
    int W = gfx_w(), top = gfx_h() - TASKBAR_H;
    tb_start = R(8, top + 6, 32 + text_w(UIF_BIG, "samara") + 14, 28);
    tb_clock = R(W - 12 - 88, top + 4, 88, 32);
    tb_lang  = R(tb_clock.x0 - 14 - 34, top + 9, 34, 22);
    tb_net   = R(tb_lang.x0 - 14 - 136, top + 6, 136, 28);

    int order[WM_MAX_WINDOWS], n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (windows[i].open) order[n++] = i;
    for (int i = 1; i < n; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && open_seq[order[j]] > open_seq[v]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }

    int x = tb_start.x1 + 17, right = tb_net.x0 - 12;
    int bw = 188;
    if (n && n * (bw + 6) > right - x) bw = (right - x) / n - 6;
    n_tb = 0;
    if (bw < 64) return;
    for (int i = 0; i < n; i++) {
        tb_btn[n_tb] = R(x, top + 6, bw, 28);
        tb_btn_win[n_tb] = order[i];
        n_tb++;
        x += bw + 6;
    }
}

/* ========================================================================
   Start menu
   ======================================================================== */

typedef struct { const char* label; const char* hint; int action; } menu_entry_t;

static const menu_entry_t menu_entries[] = {
    { "Terminal",     NULL,  0 },
    { "Web Browser",  NULL,  1 },
    { "Music Player", NULL,  2 },
    { "Paint",        NULL,  3 },
    { "Clock",        NULL,  4 },
    { NULL,           NULL, -1 },
    { "About",        NULL,  5 },
    { "Welcome",      NULL,  6 },
    { "System Info",  NULL,  7 },
    { NULL,           NULL, -1 },
    { "Reboot",       NULL,  8 },
    { "Shutdown",     NULL,  9 },
    { "Exit Desktop", "Esc", 10 },
};
#define N_MENU_ENTRIES ((int)(sizeof(menu_entries) / sizeof(menu_entries[0])))

static rect_t menu_r;
static rect_t menu_item_r[N_MENU_ENTRIES];
static int    menu_item_entry[N_MENU_ENTRIES];
static int    n_menu_items;
static int    menu_sep_y[N_MENU_ENTRIES];
static int    n_menu_seps;

static void layout_menu(void) {
    int h = MENU_PAD + MENU_HEAD + MENU_PAD;
    for (int i = 0; i < N_MENU_ENTRIES; i++) h += menu_entries[i].label ? MENU_ITEM : MENU_SEP;
    menu_r = R(8, gfx_h() - TASKBAR_H - 8 - h, MENU_W, h);

    int y = menu_r.y0 + MENU_PAD + MENU_HEAD;
    n_menu_items = n_menu_seps = 0;
    for (int i = 0; i < N_MENU_ENTRIES; i++) {
        if (!menu_entries[i].label) {
            menu_sep_y[n_menu_seps++] = y + MENU_SEP / 2;
            y += MENU_SEP;
            continue;
        }
        menu_item_r[n_menu_items] = R(menu_r.x0 + MENU_PAD, y, MENU_W - 2 * MENU_PAD, MENU_ITEM);
        menu_item_entry[n_menu_items] = i;
        n_menu_items++;
        y += MENU_ITEM;
    }
}

static rect_t menu_bounds(void) {
    rect_t r = { menu_r.x0 - SH_SIZE, menu_r.y0 - SH_SIZE + SH_DY,
                 menu_r.x1 + SH_SIZE, menu_r.y1 + SH_SIZE + SH_DY };
    return r;
}

static void open_menu(void) {
    layout_menu();
    menu_open = true;
    damage_rect(menu_bounds());
    damage_rect(tb_start);
    layout_changed = true;
}

static void close_menu(void) {
    if (!menu_open) return;
    damage_rect(menu_bounds());
    damage_rect(tb_start);
    menu_open = false;
    if (hov_kind == HOV_MENU) { hov_kind = HOV_NONE; hov_idx = -1; }
    layout_changed = true;
}

static const char welcome_text[] =
    "Welcome to SamaraOS.\n"
    "\n"
    "Drag a window by its title bar.\n"
    "The buttons on the right of the title\n"
    "minimize and close it; the taskbar\n"
    "brings minimized windows back.\n"
    "\n"
    "Esc returns to the text shell.\n";

static const char about_text[] =
    "SamaraOS 0.5\n"
    "\n"
    "Hobby OS in C for x86 / Multiboot.\n"
    "\n"
    "Preemptive multitasking, ramfs + FAT,\n"
    "PS/2 input, SB16 audio, RTL8139 + TCP,\n"
    "a compositing window manager, DOOM.\n";

static void menu_run(int action) {
    close_menu();
    int W = gfx_w(), H = gfx_h();
    switch (action) {
        case 0:  wm_open_terminal(80, 80); break;
        case 1:  browser_open(NULL); break;
        case 2:  mediaplayer_open(); break;
        case 3:  paint_open(); break;
        case 4:  clock_open(); break;
        case 5:  wm_open_info(W / 2 - 180, H / 2 - 120, 360, 190, "About", about_text); break;
        case 6:  wm_open_info(W / 2 - 200, H / 2 - 120, 400, 200, "Welcome", welcome_text); break;
        case 7:  wm_open_info(W / 2 - 200, H / 2 - 120, 400, 210, "System Info", wm_sysinfo_text()); break;
        case 8:  while (inb(0x64) & 0x02) {} outb(0x64, 0xFE); break;
        case 9:  outw(0x604, 0x2000); outw(0xB004, 0x2000); break;
        case 10: exit_requested = true; break;
    }
}

/* ========================================================================
   Drawing
   ======================================================================== */

static void draw_close_glyph(rect_t b, uint32_t c) {
    int x = b.x0 + 7, y = b.y0 + 5;
    for (int i = 0; i < 8; i++) {
        gfx_rect_fill(x + i, y + i, 2, 1, c);
        gfx_rect_fill(x + 6 - i, y + i, 2, 1, c);
    }
}

static void draw_min_glyph(rect_t b, uint32_t c) {
    gfx_rect_fill(b.x0 + 7, b.y0 + 11, 9, 2, c);
}

/* ---- terminal caret ---- */
static int  caret_x = -1, caret_y = -1;
static bool caret_on;
static uint32_t last_key_ms;

static void draw_window(int idx, rect_t region) {
    window_t* w = &windows[idx];
    if (!r_overlap(win_bounds(w), region)) return;
    bool foc = (idx == focused_idx);

    gfx_shadow(w->x, w->y + SH_DY, w->w, w->h, WIN_R, SH_SIZE, foc ? 120 : 60);
    gfx_rrect_fill(w->x, w->y, w->w, w->h, WIN_R, GFX_CORNERS_ALL, C_OUTLINE);
    gfx_rrect_fill(w->x + 1, w->y + 1, w->w - 2, w->h - 2, WIN_R - 1, GFX_CORNERS_ALL, C_SURFACE);
    gfx_rect_fill(w->x + 1, w->y + WM_TITLE_H, w->w - 2, 1, C_RULE);

    gfx_rrect_fill(w->x + 10, w->y + 7, 8, 8, 2, GFX_CORNERS_ALL, foc ? C_ACCENT : C_RULE);
    text_fit(w->x + 26, w->y + WM_TITLE_H / 2 + 1, UIF_MED, w->title, w->w - 26 - 58,
             foc ? C_INK : C_INK_DIM);

    rect_t mb = min_btn(w), cb = close_btn(w);
    uint32_t glyph = foc ? C_GLYPH : C_GLYPH_DIM;
    if (hovered(HOV_MIN, idx)) {
        gfx_rrect_fill(mb.x0, mb.y0, mb.x1 - mb.x0, mb.y1 - mb.y0, 4, GFX_CORNERS_ALL, C_BTN_HOVER);
        draw_min_glyph(mb, C_INK);
    } else {
        draw_min_glyph(mb, glyph);
    }
    if (hovered(HOV_CLOSE, idx)) {
        gfx_rrect_fill(cb.x0, cb.y0, cb.x1 - cb.x0, cb.y1 - cb.y0, 4, GFX_CORNERS_ALL, C_DANGER);
        draw_close_glyph(cb, C_WHITE);
    } else {
        draw_close_glyph(cb, glyph);
    }

    rect_t cr = client_of(w);
    rect_t inner = r_isect(cr, region);
    if (inner.x0 >= inner.x1 || inner.y0 >= inner.y1) return;
    clip_to(inner);

    if (w->type == WIN_INFO && w->content) {
        /* First line is the heading, the rest a wrapped paragraph. */
        const char* p = w->content;
        char head[64];
        int n = 0;
        while (p[n] && p[n] != '\n' && n < 63) { head[n] = p[n]; n++; }
        head[n] = 0;
        int tx = cr.x0 + 16, ty = cr.y0 + 12;
        uif_draw_fit(tx, ty, UIF_BIG, head, cr.x1 - cr.x0 - 32, C_INK);
        ty += uif_height(UIF_BIG) + 2;
        p += n;
        if (*p == '\n') p++;
        if (*p == '\n') { p++; ty += 4; }
        uif_draw_wrap(tx, ty, cr.x1 - cr.x0 - 32, 21, UIF_REG, p, C_INK);
    } else if (w->type == WIN_TERMINAL) {
        w->term_x = cr.x0;
        w->term_y = cr.y0;
        gfx_term_repos(cr.x0, cr.y0);
        if (caret_on && idx == terminal_idx)
            gfx_rect_fill(caret_x, caret_y + 13, FONT_W, 2, C_ACCENT);
    } else if (w->type == WIN_APP && w->on_paint) {
        w->on_paint(w);
    }
    clip_to(region);
}

static void draw_taskbar(void) {
    int W = gfx_w(), top = gfx_h() - TASKBAR_H;
    gfx_rect_fill(0, top, W, TASKBAR_H, C_BAR);
    gfx_rect_fill(0, top, W, 1, C_BAR_RULE);

    /* start */
    uint32_t sbg = menu_open ? C_BAR_ACTIVE : hovered(HOV_START, 0) ? C_BAR_HOVER : C_BAR;
    if (sbg != C_BAR)
        gfx_rrect_fill(tb_start.x0, tb_start.y0, tb_start.x1 - tb_start.x0,
                       tb_start.y1 - tb_start.y0, 4, GFX_CORNERS_ALL, sbg);
    int lx = tb_start.x0 + 12, ly = tb_start.y0 + 9;
    gfx_rrect_fill(lx, ly, 11, 11, 2, GFX_CORNERS_ALL, C_ACCENT);
    gfx_rect_fill(lx + 6, ly + 6, 5, 5, sbg);
    text(tb_start.x0 + 32, tb_start.y0 + 13, UIF_BIG, "samara", C_BAR_TEXT);
    gfx_rect_fill(tb_start.x1 + 8, top + 12, 1, 16, C_BAR_RULE);

    /* window buttons */
    for (int i = 0; i < n_tb; i++) {
        int idx = tb_btn_win[i];
        window_t* w = &windows[idx];
        rect_t b = tb_btn[i];
        bool foc = (idx == focused_idx && !w->minimized);
        bool hv = hovered(HOV_TASK, idx);
        if (foc || hv)
            gfx_rrect_fill(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, 4, GFX_CORNERS_ALL,
                           foc ? C_BAR_ACTIVE : C_BAR_HOVER);
        uint32_t tc = foc || hv ? C_BAR_TEXT : w->minimized ? C_BAR_FAINT : C_BAR_DIM;
        text_fit(b.x0 + 12, b.y0 + 14, UIF_REG, w->title, b.x1 - b.x0 - 24, tc);
        if (foc) gfx_rect_fill(b.x0 + 10, b.y1 - 2, b.x1 - b.x0 - 20, 2, C_ACCENT);
    }

    /* network */
    char ip[20];
    const char* label = "offline";
    if (net_key) { ip_string(net_key, ip); label = ip; }
    int tw = text_w(UIF_REG, label);
    int nx = tb_net.x1 - tw;
    int ncy = tb_net.y0 + 14;
    gfx_rrect_fill(nx - 14, ncy - 3, 6, 6, 3, GFX_CORNERS_ALL, net_key ? C_ONLINE : C_BAR_FAINT);
    text(nx, ncy, UIF_REG, label, net_key ? C_BAR_DIM : C_BAR_FAINT);

    /* keyboard layout pill (click toggles) */
    bool ru = kbd_is_ru();
    gfx_rrect_fill(tb_lang.x0, tb_lang.y0, tb_lang.x1 - tb_lang.x0, tb_lang.y1 - tb_lang.y0, 4,
                   GFX_CORNERS_ALL, hovered(HOV_LANG, 0) ? C_BAR_PILL_HV : C_BAR_ACTIVE);
    uif_draw_center(tb_lang.x0, tb_lang.y0, tb_lang.x1 - tb_lang.x0, tb_lang.y1 - tb_lang.y0,
                    UIF_SMALL, ru ? "RU" : "EN", ru ? C_ACCENT : C_BAR_TEXT);

    /* clock */
    text(tb_clock.x1 - text_w(UIF_MED, clock_time), tb_clock.y0 + 9, UIF_MED, clock_time, C_BAR_TEXT);
    text(tb_clock.x1 - text_w(UIF_SMALL, clock_date), tb_clock.y0 + 25, UIF_SMALL, clock_date, C_BAR_DIM);
}

static void draw_menu(void) {
    rect_t m = menu_r;
    int w = m.x1 - m.x0, h = m.y1 - m.y0;
    gfx_shadow(m.x0, m.y0 + SH_DY, w, h, MENU_R, SH_SIZE, 150);
    gfx_rrect_fill(m.x0, m.y0, w, h, MENU_R, GFX_CORNERS_ALL, C_MENU_EDGE);
    gfx_rrect_fill(m.x0 + 1, m.y0 + 1, w - 2, h - 2, MENU_R - 1, GFX_CORNERS_ALL, C_MENU);

    int hx = m.x0 + 18, hy = m.y0 + MENU_PAD + 10;
    gfx_rrect_fill(hx, hy + 2, 11, 11, 2, GFX_CORNERS_ALL, C_ACCENT);
    gfx_rect_fill(hx + 6, hy + 8, 5, 5, C_MENU);
    int ex = text(hx + 20, hy + 8, UIF_BIG, "SamaraOS", C_BAR_TEXT);
    text(ex + 8, hy + 8, UIF_SMALL, "0.5", C_BAR_FAINT);
    gfx_rect_fill(m.x0 + 12, m.y0 + MENU_PAD + MENU_HEAD - 1, w - 24, 1, C_MENU_EDGE);

    for (int i = 0; i < n_menu_seps; i++)
        gfx_rect_fill(m.x0 + 12, menu_sep_y[i], w - 24, 1, C_MENU_EDGE);

    for (int k = 0; k < n_menu_items; k++) {
        rect_t r = menu_item_r[k];
        const menu_entry_t* e = &menu_entries[menu_item_entry[k]];
        bool hv = hovered(HOV_MENU, k);
        if (hv) {
            gfx_rrect_fill(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 4, GFX_CORNERS_ALL, C_MENU_HOVER);
            gfx_rect_fill(r.x0, r.y0 + 7, 2, 16, C_ACCENT);
        }
        text(r.x0 + 14, r.y0 + MENU_ITEM / 2, UIF_REG, e->label, hv ? C_WHITE : C_BAR_TEXT);
        if (e->hint)
            text(r.x1 - 12 - text_w(UIF_SMALL, e->hint), r.y0 + MENU_ITEM / 2, UIF_SMALL, e->hint, C_BAR_FAINT);
    }
}

/* ========================================================================
   Compositor: redraw only damaged rects, back-to-front, then present them.
   ======================================================================== */

/* on_paint may advance app state, so no app client may straddle two rects. */
static bool paint_conflict(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!visible(i) || windows[i].type != WIN_APP) continue;
        rect_t cr = client_of(&windows[i]);
        int first = -1;
        for (int j = 0; j < n_dmg; j++) {
            if (!r_overlap(dmg[j], cr)) continue;
            if (first < 0) { first = j; continue; }
            dmg[first] = r_union(dmg[first], dmg[j]);
            dmg[j] = dmg[--n_dmg];
            return true;
        }
    }
    return false;
}

static void merge_damage(void) {
    for (;;) {
        bool merged = false;
        for (int i = 0; i < n_dmg && !merged; i++)
            for (int j = i + 1; j < n_dmg && !merged; j++) {
                rect_t u = r_union(dmg[i], dmg[j]);
                if (r_overlap(dmg[i], dmg[j]) ||
                    r_area(u) <= r_area(dmg[i]) + r_area(dmg[j]) + 8192) {
                    dmg[i] = u;
                    dmg[j] = dmg[--n_dmg];
                    merged = true;
                }
            }
        if (!merged) merged = paint_conflict();
        if (!merged) return;
    }
}

static void compose(rect_t region) {
    composing = true;
    if (db_on) gfx_target_back();
    clip_to(region);

    int desk_h = gfx_h() - TASKBAR_H;
    if (region.y0 < desk_h) {
        if (bg_cache) gfx_blit_argb(0, 0, bg_w, bg_h, bg_cache);
        else          gfx_rect_fill(0, 0, gfx_w(), desk_h, C_DESK_TOP);
    }

    int order[WM_MAX_WINDOWS];
    int n = z_sorted(order);
    for (int i = 0; i < n; i++) draw_window(order[i], region);

    if (region.y1 > desk_h) draw_taskbar();
    if (menu_open && r_overlap(menu_bounds(), region)) draw_menu();

    gfx_reset_clip();
    composing = false;
}

/* ========================================================================
   Mouse cursor — kept off the back buffer and stamped onto the front.
   ======================================================================== */

static const char* cursor_pix[CUR_H] = {
    "#           ",
    "##          ",
    "#*#         ",
    "#**#        ",
    "#***#       ",
    "#****#      ",
    "#*****#     ",
    "#******#    ",
    "#*******#   ",
    "#********#  ",
    "#*********# ",
    "#******#####",
    "#***#**#    ",
    "#**##**#    ",
    "#*#  #**#   ",
    "##   #**#   ",
    "#     #**#  ",
    "      #**#  ",
    "       ##   ",
};

static uint32_t cursor_under[CUR_W * CUR_H];
static int cur_x = -1, cur_y = -1;

static void stamp_cursor(int x, int y) {
    for (int r = 0; r < CUR_H; r++)
        for (int c = 0; c < CUR_W; c++) {
            char p = cursor_pix[r][c];
            if (p == '#')      gfx_pixel(x + c, y + r, C_BLACK);
            else if (p == '*') gfx_pixel(x + c, y + r, C_WHITE);
        }
}

static rect_t cursor_rect(int x, int y) { return R(x, y, CUR_W, CUR_H); }

/* Double-buffered: cursor lives only on the front buffer. */
static void front_cursor(int x, int y) {
    gfx_target_front();
    stamp_cursor(x, y);
    gfx_target_back();
    cur_x = x; cur_y = y;
}

/* Single-buffered fallback: classic save-under. */
static void sw_hide_cursor(void) {
    if (cur_x < 0) return;
    gfx_restore_rect(cur_x, cur_y, CUR_W, CUR_H, cursor_under);
    cur_x = cur_y = -1;
}
static void sw_show_cursor(int x, int y) {
    gfx_save_rect(x, y, CUR_W, CUR_H, cursor_under);
    stamp_cursor(x, y);
    cur_x = x; cur_y = y;
}

/* Terminal output from a running command lands in the back buffer between
   frames; put it on screen right away when nothing covers the terminal. */
static void term_drawn(int x, int y, int w, int h) {
    if (composing) return;
    rect_t r = R(x, y, w, h);
    bool on_top = terminal_idx >= 0 && visible(terminal_idx) && focused_idx == terminal_idx &&
                  !(menu_open && r_overlap(menu_bounds(), r));
    if (!db_on || !first_frame_done || !on_top) { damage_rect(r); return; }
    gfx_present_rect(x, y, w, h);
    if (cur_x >= 0 && r_overlap(r, cursor_rect(cur_x, cur_y))) front_cursor(cur_x, cur_y);
}

/* ========================================================================
   Input
   ======================================================================== */

static rect_t hover_rect(int kind, int idx) {
    switch (kind) {
        case HOV_CLOSE: return close_btn(&windows[idx]);
        case HOV_MIN:   return min_btn(&windows[idx]);
        case HOV_START: return tb_start;
        case HOV_LANG:  return tb_lang;
        case HOV_MENU:  return menu_item_r[idx];
        case HOV_TASK:
            for (int i = 0; i < n_tb; i++) if (tb_btn_win[i] == idx) return tb_btn[i];
            break;
    }
    return R(0, 0, 0, 0);
}

static void set_hover(int kind, int idx) {
    if (kind == hov_kind && idx == hov_idx) return;
    if (hov_kind != HOV_NONE) damage_rect(hover_rect(hov_kind, hov_idx));
    hov_kind = kind;
    hov_idx = idx;
    if (kind != HOV_NONE) damage_rect(hover_rect(kind, idx));
}

static void update_hover(int mx, int my) {
    if (menu_open && r_hit(menu_r, mx, my)) {
        for (int k = 0; k < n_menu_items; k++)
            if (r_hit(menu_item_r[k], mx, my)) { set_hover(HOV_MENU, k); return; }
        set_hover(HOV_NONE, -1);
        return;
    }
    if (my >= gfx_h() - TASKBAR_H) {
        if (r_hit(tb_start, mx, my)) { set_hover(HOV_START, 0); return; }
        if (r_hit(tb_lang, mx, my))  { set_hover(HOV_LANG, 0); return; }
        for (int i = 0; i < n_tb; i++)
            if (r_hit(tb_btn[i], mx, my)) { set_hover(HOV_TASK, tb_btn_win[i]); return; }
        set_hover(HOV_NONE, -1);
        return;
    }
    int wi = hit_window(mx, my);
    if (wi >= 0 && r_hit(close_btn(&windows[wi]), mx, my)) { set_hover(HOV_CLOSE, wi); return; }
    if (wi >= 0 && r_hit(min_btn(&windows[wi]), mx, my))   { set_hover(HOV_MIN, wi); return; }
    set_hover(HOV_NONE, -1);
}

static void on_press(int mx, int my) {
    layout_changed = true;
    if (menu_open) {
        if (r_hit(menu_r, mx, my)) {
            for (int k = 0; k < n_menu_items; k++)
                if (r_hit(menu_item_r[k], mx, my)) {
                    menu_run(menu_entries[menu_item_entry[k]].action);
                    return;
                }
            return;
        }
        close_menu();
        return;
    }

    if (my >= gfx_h() - TASKBAR_H) {
        if (r_hit(tb_start, mx, my)) { open_menu(); return; }
        if (r_hit(tb_lang, mx, my))  { kbd_set_ru(!kbd_is_ru()); damage_rect(tb_lang); return; }
        for (int i = 0; i < n_tb; i++) {
            if (!r_hit(tb_btn[i], mx, my)) continue;
            int idx = tb_btn_win[i];
            if (windows[idx].minimized)  restore(idx);
            else if (idx == focused_idx) minimize(idx);
            else                         set_focus(idx);
            return;
        }
        return;
    }

    int wi = hit_window(mx, my);
    if (wi < 0) return;
    window_t* w = &windows[wi];
    set_focus(wi);
    press_target_idx = wi;

    if (r_hit(close_btn(w), mx, my)) { wm_close(w); return; }
    if (r_hit(min_btn(w), mx, my))   { press_target_idx = -1; minimize(wi); return; }
    if (r_hit(title_bar(w), mx, my)) {
        w->dragging = true;
        w->drag_off_x = mx - w->x;
        w->drag_off_y = my - w->y;
        return;
    }
    if (w->type == WIN_APP && w->on_click) {
        rect_t cr = client_of(w);
        if (r_hit(cr, mx, my)) {
            w->on_click(w, mx - cr.x0, my - cr.y0);
            w->needs_repaint = true;
        }
    }
}

static void on_drag(int mx, int my) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t* w = &windows[i];
        if (!visible(i) || !w->dragging) continue;
        int W = gfx_w(), H = gfx_h() - TASKBAR_H;
        int nx = mx - w->drag_off_x, ny = my - w->drag_off_y;
        if (nx < -w->w + 80) nx = -w->w + 80;
        if (nx > W - 80) nx = W - 80;
        if (ny < 0) ny = 0;
        if (ny > H - WM_TITLE_H) ny = H - WM_TITLE_H;
        if (nx != w->x || ny != w->y) {
            damage_rect(win_bounds(w));
            if (w->type == WIN_TERMINAL) { w->term_x += nx - w->x; w->term_y += ny - w->y; }
            w->x = nx;
            w->y = ny;
            damage_rect(win_bounds(w));
            layout_changed = true;
        }
        return;
    }
    if (press_target_idx >= 0 && press_target_idx == focused_idx) {
        window_t* w = &windows[press_target_idx];
        if (visible(press_target_idx) && w->type == WIN_APP && w->on_click) {
            rect_t cr = client_of(w);
            if (r_hit(cr, mx, my)) {
                w->on_click(w, mx - cr.x0, my - cr.y0);
                w->needs_repaint = true;
            }
        }
    }
}

static void on_release(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) windows[i].dragging = false;
    if (press_target_idx >= 0) {
        window_t* w = &windows[press_target_idx];
        if (w->open && w->type == WIN_APP && w->on_release) {
            w->on_release(w);
            w->needs_repaint = true;
        }
        press_target_idx = -1;
    }
    layout_changed = true;
}

static void on_key(char c, uint32_t now) {
    if (menu_open) {
        int k = hov_kind == HOV_MENU ? hov_idx : -1;
        if (c == 0x1B) close_menu();
        else if (c == (char)K_DOWN) set_hover(HOV_MENU, (k + 1) % n_menu_items);
        else if (c == (char)K_UP)   set_hover(HOV_MENU, k <= 0 ? n_menu_items - 1 : k - 1);
        else if (c == '\n' && k >= 0) menu_run(menu_entries[menu_item_entry[k]].action);
        return;
    }
    window_t* f = (focused_idx >= 0 && visible(focused_idx)) ? &windows[focused_idx] : NULL;
    /* Esc belongs to apps (DOOM's menu, Bounce's close); elsewhere it leaves. */
    if (c == 0x1B && (!f || f->type == WIN_INFO ||
                      (f->type == WIN_TERMINAL && !wm_terminal_busy()))) {
        exit_requested = true;
        return;
    }
    if (!f) return;
    if (f->type == WIN_TERMINAL) {
        last_key_ms = now;
        wm_terminal_handle_key(f, c);
    } else if (f->type == WIN_APP && f->on_key) {
        f->on_key(f, c);
        f->needs_repaint = true;
    }
}

/* ========================================================================
   Per-frame bookkeeping that turns state changes into damage
   ======================================================================== */

static uint32_t last_layout_epoch;

static void collect_damage(uint32_t now) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t* w = &windows[i];
        if (!w->open) continue;
        if (strcmp(w->title, drawn_title[i]) != 0) {
            memcpy(drawn_title[i], w->title, 64);
            if (!w->minimized) damage_rect(title_bar(w));
            damage_taskbar();
        }
        if (w->type != WIN_APP) continue;
        if (w->on_tick) w->on_tick(w, now);
        if (!w->minimized && (w->animate || w->needs_repaint)) damage_rect(client_of(w));
        w->needs_repaint = false;
    }

    if ((int32_t)(now - next_rtc_ms) >= 0) {
        next_rtc_ms = now + 1000;
        if (rtc_poll()) damage_rect(tb_clock);
        uint32_t nk = net_ready() ? net_ip() : 0;
        if (nk != net_key) { net_key = nk; damage_rect(tb_net); }
    }

    uint32_t le = kbd_layout_epoch();
    if (le != last_layout_epoch) { last_layout_epoch = le; damage_rect(tb_lang); }

    bool on = false;
    int px = caret_x, py = caret_y;
    if (terminal_idx >= 0 && visible(terminal_idx) && focused_idx == terminal_idx) {
        int tx, ty;
        gfx_term_get_cursor(&tx, &ty);
        px = windows[terminal_idx].term_x + tx * FONT_W;
        py = windows[terminal_idx].term_y + ty * FONT_H;
        on = ((now - last_key_ms) % 1060) < 530;
    }
    if (on != caret_on || px != caret_x || py != caret_y) {
        if (caret_on) damage(caret_x, caret_y, FONT_W, FONT_H);
        caret_on = on;
        caret_x = px;
        caret_y = py;
        if (caret_on) damage(caret_x, caret_y, FONT_W, FONT_H);
    }
}

/* ========================================================================
   Main loop
   ======================================================================== */

void wm_init(void) {
    memset(windows, 0, sizeof(windows));
    memset(open_seq, 0, sizeof(open_seq));
    seq_counter = 0;
    next_z = 0;
    focused_idx = terminal_idx = press_target_idx = -1;
    menu_open = exit_requested = composing = first_frame_done = false;
    hov_kind = HOV_NONE; hov_idx = -1;
    cur_x = cur_y = -1;
    caret_on = false; caret_x = caret_y = -1;
    n_dmg = 0;
    net_key = 0;
    next_rtc_ms = 0;
    last_layout_epoch = kbd_layout_epoch();

    db_on = gfx_enable_double_buffer();
    if (db_on) gfx_target_back();
    gfx_reset_clip();
    build_background(gfx_w(), gfx_h() - TASKBAR_H);
    rtc_poll();
    layout_taskbar();
    gfx_term_set_draw_hook(term_drawn);
    damage_all();
}

/* 60 Hz on a 1 kHz PIT: frame n is due at t0 + n*50/3 ms. */
#define FRAME_NUM 50
#define FRAME_DEN 3

void wm_run(void) {
    uint32_t t0 = pit_uptime_ms();
    uint32_t frame = 0;
    bool prev_btn = false;
    bool fb_was_active = false;
    int prev_mx = -1, prev_my = -1;

    while (!exit_requested) {
        uint32_t due = t0 + frame * FRAME_NUM / FRAME_DEN;
        while ((int32_t)(pit_uptime_ms() - due) < 0) task_yield();   /* lets user programs run */
        uint32_t now = pit_uptime_ms();
        frame++;
        if ((int32_t)(now - due) > 100) { t0 = now; frame = 1; }   /* fell behind: resync */

        /* A process owns the screen through /dev/fb0: leave the pixels and
           the input alone, but keep the terminal plumbing (and the foreground
           job's exit detection) running. */
        if (fbdev_active()) {
            fb_was_active = true;
            while (kbd_has_key()) (void)kbd_trygetc();
            if (terminal_idx >= 0) wm_terminal_poll(&windows[terminal_idx]);
            continue;
        }
        if (fb_was_active) {
            fb_was_active = false;
            fbdev_restore();
            cur_x = cur_y = -1;
            prev_btn = false; prev_mx = prev_my = -1;
            damage_all();
        }

        int mx, my; uint8_t b;
        mouse_get(&mx, &my, &b);
        bool button = (b & 1) != 0;
        bool moved = (mx != prev_mx || my != prev_my);

        if (button && !prev_btn)      on_press(mx, my);
        else if (!button && prev_btn) on_release();
        else if (button && moved)     on_drag(mx, my);
        prev_btn = button;

        while (kbd_has_key() && !exit_requested) on_key(kbd_trygetc(), now);
        if (terminal_idx >= 0) wm_terminal_poll(&windows[terminal_idx]);

        if (layout_changed) { layout_taskbar(); if (menu_open) layout_menu(); }
        if (!button && (moved || layout_changed)) update_hover(mx, my);
        layout_changed = false;

        collect_damage(now);
        prev_mx = mx; prev_my = my;

        if (db_on) {
            bool cursor_hit = false;
            if (n_dmg) {
                merge_damage();
                for (int i = 0; i < n_dmg; i++) compose(dmg[i]);
                for (int i = 0; i < n_dmg; i++) {
                    gfx_present_rect(dmg[i].x0, dmg[i].y0,
                                     dmg[i].x1 - dmg[i].x0, dmg[i].y1 - dmg[i].y0);
                    if (cur_x >= 0 && r_overlap(dmg[i], cursor_rect(cur_x, cur_y))) cursor_hit = true;
                }
                n_dmg = 0;
            }
            if (mx != cur_x || my != cur_y) {
                if (cur_x >= 0) gfx_present_rect(cur_x, cur_y, CUR_W, CUR_H);
                front_cursor(mx, my);
            } else if (cursor_hit) {
                front_cursor(mx, my);
            }
        } else {
            bool hide = (mx != cur_x || my != cur_y);
            for (int i = 0; i < n_dmg && !hide; i++)
                if (cur_x >= 0 && r_overlap(dmg[i], cursor_rect(cur_x, cur_y))) hide = true;
            if (hide) sw_hide_cursor();
            if (n_dmg) {
                merge_damage();
                for (int i = 0; i < n_dmg; i++) compose(dmg[i]);
                n_dmg = 0;
            }
            if (hide) sw_show_cursor(mx, my);
        }
        first_frame_done = true;
    }

    if (terminal_idx >= 0) wm_terminal_closed();
    gfx_term_set_draw_hook(NULL);
    if (db_on) { gfx_disable_double_buffer(); db_on = false; }
    gfx_reset_clip();
}
