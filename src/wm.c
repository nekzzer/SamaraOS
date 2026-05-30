#include "wm.h"
#include "gfx.h"
#include "gfx_term.h"
#include "vga.h"
#include "mouse.h"
#include "keyboard.h"
#include "string.h"
#include "heap.h"
#include "pit.h"
#include "io.h"
#include "mediaplayer.h"
#include "paint.h"
#include "clock.h"
#include "browser.h"
#include "net.h"

static bool db_on = false;

/* Forward decls into shell.c (terminal handlers) */
extern void wm_terminal_init(window_t* w);
extern void wm_terminal_handle_key(window_t* w, char c);
extern const char* wm_sysinfo_text(void);

/* ---- colours ---- */
#define BAR_BG     RGB(0x10, 0x14, 0x28)
#define BAR_HI     RGB(0x6A, 0x82, 0xFB)
#define WIN_BG     RGB(0xE8, 0xE8, 0xEC)
#define WIN_FG     RGB(0x10, 0x10, 0x18)
#define WIN_BORDER RGB(0x40, 0x44, 0x58)
#define TITLE_BG   RGB(0x42, 0x53, 0xD4)
#define TITLE_INACT RGB(0x60, 0x64, 0x70)
#define WHITE      RGB(0xFF, 0xFF, 0xFF)
#define BLACK      RGB(0x00, 0x00, 0x00)
#define GREY       RGB(0x80, 0x80, 0x90)
#define RED        RGB(0xE0, 0x40, 0x40)
#define MENU_BG    RGB(0x20, 0x28, 0x50)
#define MENU_HOVER RGB(0x40, 0x55, 0xA0)
#define BTN_FACE   RGB(0x30, 0x40, 0x80)
#define BTN_FOCUS  RGB(0x40, 0x50, 0x90)

/* ---- state ---- */
static window_t windows[WM_MAX_WINDOWS];
static int next_z = 1;
static int focused_idx = -1;
static int terminal_idx = -1;        /* singleton terminal slot */
static bool start_menu_open = false;
static bool exit_requested = false;
static bool need_redraw = true;

/* Taller taskbar to feel right on 1080p; legacy modes still fit since
   gfx_h() is always honoured for positioning. */
static int taskbar_h = 40;
static int start_btn_x = 0, start_btn_y = 0, start_btn_w = 80, start_btn_h = 24;
static int menu_x = 0, menu_y = 0, menu_w = 220, menu_h = 0;
static int hover_item = -1;

static int prev_mx = -1, prev_my = -1;
static bool prev_btn = false;
static int  press_target_idx = -1;     /* window that received the press; fires on_release later */

/* ---- mouse cursor sprite ---- */
#define CUR_W 12
#define CUR_H 19
static const char* cursor_pix[CUR_H] = {
    "#           ","##          ","#*#         ","#**#        ",
    "#***#       ","#****#      ","#*****#     ","#******#    ",
    "#*******#   ","#********#  ","#*********# ","#****#####  ",
    "#***# #*#   ","#**#  #*#   ","#*#    ##   ","##          ",
    "            ","            ","            "
};
static uint32_t cursor_under[CUR_W * CUR_H];
static int cur_drawn_x = -1, cur_drawn_y = -1;

static void hide_cursor(void) {
    if (cur_drawn_x >= 0) {
        gfx_restore_rect(cur_drawn_x, cur_drawn_y, CUR_W, CUR_H, cursor_under);
        cur_drawn_x = cur_drawn_y = -1;
    }
}
static void draw_cursor(int x, int y) {
    gfx_save_rect(x, y, CUR_W, CUR_H, cursor_under);
    for (int r = 0; r < CUR_H; r++)
        for (int c = 0; c < CUR_W; c++) {
            char p = cursor_pix[r][c];
            if (p == '#') gfx_pixel(x + c, y + r, BLACK);
            else if (p == '*') gfx_pixel(x + c, y + r, WHITE);
        }
    cur_drawn_x = x;
    cur_drawn_y = y;
}

/* Composite cursor sprite directly into whatever the current draw target is.
   Used to stamp the cursor onto the back buffer right before gfx_present(),
   so the user never sees a "cursorless" frame during compositing. */
static void stamp_cursor_pixels(int x, int y) {
    for (int r = 0; r < CUR_H; r++)
        for (int c = 0; c < CUR_W; c++) {
            char p = cursor_pix[r][c];
            if (p == '#') gfx_pixel(x + c, y + r, BLACK);
            else if (p == '*') gfx_pixel(x + c, y + r, WHITE);
        }
}

/* ---- helpers ---- */

static int z_sorted(int* out) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (windows[i].open) out[n++] = i;
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - 1 - i; j++)
            if (windows[out[j]].z > windows[out[j+1]].z) {
                int t = out[j]; out[j] = out[j+1]; out[j+1] = t;
            }
    return n;
}

static int alloc_slot(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!windows[i].open) return i;
    return -1;
}

static void focus_idx(int idx) {
    if (idx < 0) { focused_idx = -1; return; }
    windows[idx].z = ++next_z;
    focused_idx = idx;
    /* gfx_term_repos happens inside draw_window() during back-buffer redraw */
}

void wm_request_redraw(void) { need_redraw = true; }
void wm_request_exit(void)   { exit_requested = true; }
window_t* wm_focused(void)   { return focused_idx >= 0 ? &windows[focused_idx] : NULL; }

void wm_client_rect(window_t* w, int* x, int* y, int* cw, int* ch) {
    *x  = w->x + 4;
    *y  = w->y + WM_TITLE_H + 2;
    *cw = w->w - 8;
    *ch = w->h - WM_TITLE_H - 6;
}

window_t* wm_open_info(int x, int y, int w, int h, const char* title, const char* content) {
    int i = alloc_slot();
    if (i < 0) return NULL;
    window_t* win = &windows[i];
    memset(win, 0, sizeof(*win));
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title, 63); win->title[63] = 0;
    win->open = true;
    win->type = WIN_INFO;
    win->content = content;
    win->z = ++next_z;
    focused_idx = i;
    need_redraw = true;
    return win;
}

window_t* wm_open_terminal(int x, int y) {
    if (terminal_idx >= 0 && windows[terminal_idx].open) {
        focus_idx(terminal_idx);
        need_redraw = true;
        return &windows[terminal_idx];
    }
    int i = alloc_slot();
    if (i < 0) return NULL;
    window_t* win = &windows[i];
    memset(win, 0, sizeof(*win));
    win->x = x; win->y = y;
    win->w = 80 * 8 + 8;
    win->h = 25 * 16 + WM_TITLE_H + 6;
    strncpy(win->title, "Terminal - samara-sh", 63); win->title[63] = 0;
    win->open = true;
    win->type = WIN_TERMINAL;
    win->z = ++next_z;
    win->term_x = x + 4;
    win->term_y = y + WM_TITLE_H + 2;
    focused_idx = i;
    terminal_idx = i;
    wm_terminal_init(win);
    need_redraw = true;
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
    int i = alloc_slot();
    if (i < 0) return NULL;
    window_t* win = &windows[i];
    memset(win, 0, sizeof(*win));
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title, 63); win->title[63] = 0;
    win->open = true;
    win->type = WIN_APP;
    win->on_paint = on_paint;
    win->on_key = on_key;
    win->on_click = on_click;
    win->on_tick = on_tick;
    win->animate = animate;
    win->needs_repaint = true;
    win->user = user;
    win->z = ++next_z;
    focused_idx = i;
    need_redraw = true;
    return win;
}

void wm_close(window_t* w) {
    if (!w) return;
    if (w->on_close) w->on_close(w);
    w->open = false;
    if (w == &windows[focused_idx]) focused_idx = -1;
    if (w->type == WIN_TERMINAL) terminal_idx = -1;
    need_redraw = true;
}

/* ---- drawing ---- */

static void put_clock(int x, int y, int w, int h) {
    char buf[16]; int p = 0;
    uint32_t s = pit_uptime_ms() / 1000;
    int hh = (s / 3600) % 100;
    int mm = (s / 60) % 60;
    int ss = s % 60;
    char tmp[8];
    if (hh < 10) buf[p++] = '0';
    itoa(hh, tmp, 10); for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    if (mm < 10) buf[p++] = '0';
    itoa(mm, tmp, 10); for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    if (ss < 10) buf[p++] = '0';
    itoa(ss, tmp, 10); for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p] = 0;
    gfx_rect_fill(x, y, w, h, BAR_BG);
    gfx_string(x + 6, y + (h - 16) / 2, buf, WHITE, BAR_BG, false);
}

static void put_netinfo(int x, int y, int w, int h) {
    gfx_rect_fill(x, y, w, h, BAR_BG);
    if (!net_ready()) {
        gfx_string(x + 6, y + (h - 16) / 2, "net: down", GREY, BAR_BG, false);
        return;
    }
    uint32_t ip = net_ip();
    char buf[24]; int p = 0; char tmp[8];
    buf[p++] = 'I'; buf[p++] = 'P'; buf[p++] = ':'; buf[p++] = ' ';
    for (int i = 0; i < 4; i++) {
        itoa((ip >> (24 - i*8)) & 0xFF, tmp, 10);
        for (int j = 0; tmp[j]; j++) buf[p++] = tmp[j];
        if (i < 3) buf[p++] = '.';
    }
    buf[p] = 0;
    gfx_string(x + 6, y + (h - 16) / 2, buf, WHITE, BAR_BG, false);
}

static void draw_taskbar(void) {
    int W = gfx_w(), H = gfx_h();
    gfx_rect_fill(0, H - taskbar_h, W, taskbar_h, BAR_BG);
    gfx_rect_fill(0, H - taskbar_h, W, 2, BAR_HI);

    start_btn_x = 6;
    start_btn_y = H - taskbar_h + 4;
    start_btn_w = 96;
    start_btn_h = taskbar_h - 8;
    int label_y = start_btn_y + (start_btn_h - 16) / 2;

    uint32_t bbg = start_menu_open ? BAR_HI : BTN_FACE;
    gfx_rect_fill(start_btn_x, start_btn_y, start_btn_w, start_btn_h, bbg);
    gfx_rect(start_btn_x, start_btn_y, start_btn_w, start_btn_h, WHITE);
    /* a tiny windows-like 4-square logo */
    int lx = start_btn_x + 8, ly = start_btn_y + (start_btn_h - 14) / 2;
    gfx_rect_fill(lx,     ly,     6, 6, WHITE);
    gfx_rect_fill(lx + 8, ly,     6, 6, WHITE);
    gfx_rect_fill(lx,     ly + 8, 6, 6, WHITE);
    gfx_rect_fill(lx + 8, ly + 8, 6, 6, WHITE);
    gfx_string(start_btn_x + 28, label_y, "Start", WHITE, bbg, false);

    /* right-side widgets first so we know where window-button strip ends */
    int clock_w = 80, net_w = 130, lang_w = 36, gap = 6;
    int clock_x = W - clock_w - 4;
    int lang_x  = clock_x - lang_w - gap;
    int net_x   = lang_x - net_w - gap;
    put_netinfo(net_x, start_btn_y, net_w, start_btn_h);
    /* layout indicator: F11 toggles RU/EN */
    gfx_rect_fill(lang_x, start_btn_y, lang_w, start_btn_h, kbd_is_ru() ? BAR_HI : BTN_FACE);
    gfx_rect(lang_x, start_btn_y, lang_w, start_btn_h, WHITE);
    gfx_string(lang_x + 6, start_btn_y + (start_btn_h - 16) / 2,
               kbd_is_ru() ? "RU" : "EN",
               WHITE, kbd_is_ru() ? BAR_HI : BTN_FACE, false);
    put_clock(clock_x, start_btn_y, clock_w, start_btn_h);

    /* window list buttons — fill space between Start and the right-side widgets */
    int idxs[WM_MAX_WINDOWS];
    int n = z_sorted(idxs);
    int wx = start_btn_x + start_btn_w + 8;
    int strip_right = net_x - gap;
    (void)lang_x;
    for (int i = 0; i < n; i++) {
        window_t* w = &windows[idxs[i]];
        int bw = 200;
        if (wx + bw > strip_right) break;
        bool fc = (idxs[i] == focused_idx);
        uint32_t bbg2 = fc ? BTN_FOCUS : MENU_BG;
        gfx_rect_fill(wx, start_btn_y, bw, start_btn_h, bbg2);
        gfx_rect(wx, start_btn_y, bw, start_btn_h, GREY);
        char st[26]; int j = 0;
        for (; w->title[j] && j < 24; j++) st[j] = w->title[j];
        st[j] = 0;
        gfx_string(wx + 8, label_y, st, WHITE, bbg2, false);
        wx += bw + 4;
    }
}

static const char* start_items[] = {
    "Terminal",
    "Web Browser",
    "Music Player",
    "Paint",
    "Clock",
    "About",
    "Welcome",
    "System Info",
    "Reboot",
    "Shutdown",
    "Exit Desktop",
    NULL
};

static int n_start_items(void) {
    int n = 0; while (start_items[n]) n++; return n;
}

static void draw_start_menu(void) {
    if (!start_menu_open) return;
    int H = gfx_h();
    int n = n_start_items();
    int item_h = 28;
    menu_w = 220;
    menu_h = n * item_h + 10;
    menu_x = start_btn_x;
    menu_y = H - taskbar_h - menu_h;

    gfx_rect_fill(menu_x + 4, menu_y + 4, menu_w, menu_h, BLACK);
    gfx_rect_fill(menu_x, menu_y, menu_w, menu_h, MENU_BG);
    gfx_rect(menu_x, menu_y, menu_w, menu_h, WHITE);

    /* header strip on the left */
    gfx_rect_fill(menu_x, menu_y, 4, menu_h, BAR_HI);

    for (int i = 0; i < n; i++) {
        int iy = menu_y + 5 + i * item_h;
        bool hovered = (i == hover_item);
        if (hovered) gfx_rect_fill(menu_x + 6, iy, menu_w - 8, item_h, MENU_HOVER);
        gfx_string(menu_x + 16, iy + 6, start_items[i], WHITE,
                   hovered ? MENU_HOVER : MENU_BG, false);
    }
}

static void draw_window(window_t* w) {
    bool fc = (focused_idx >= 0 && &windows[focused_idx] == w);
    uint32_t tbg = fc ? TITLE_BG : TITLE_INACT;

    gfx_rect_fill(w->x + 4, w->y + 4, w->w, w->h, BLACK);   /* shadow */
    gfx_rect_fill(w->x, w->y, w->w, w->h, WIN_BG);          /* body */
    gfx_rect_fill(w->x, w->y, w->w, WM_TITLE_H, tbg);       /* title */
    gfx_string(w->x + 8, w->y + 3, w->title, WHITE, tbg, false);

    /* close button */
    int cbx = w->x + w->w - 22, cby = w->y + 3;
    gfx_rect_fill(cbx, cby, 16, 16, RED);
    gfx_string(cbx + 4, cby, "x", WHITE, RED, false);

    gfx_rect(w->x, w->y, w->w, w->h, WIN_BORDER);

    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);

    if (w->type == WIN_INFO && w->content) {
        const char* p = w->content;
        int tx = cx, ty = cy;
        while (*p && ty + 16 <= cy + ch) {
            if (*p == '\n') { ty += 16; tx = cx; p++; }
            else {
                gfx_glyph(tx, ty, *p, WIN_FG, WIN_BG, true);
                tx += 8; p++;
                if (tx + 8 > cx + cw) { tx = cx; ty += 16; }
            }
        }
    } else if (w->type == WIN_TERMINAL) {
        w->term_x = cx;
        w->term_y = cy;
        gfx_term_repos(cx, cy);
    } else if (w->type == WIN_APP) {
        if (w->on_paint) w->on_paint(w);
    }
}

static void draw_background(void) {
    int W = gfx_w(), H = gfx_h() - taskbar_h;
    for (int y = 0; y < H; y++) {
        uint8_t t = (uint8_t)((y * 64) / H);
        uint32_t col = RGB(0x14 + t/3, 0x18 + t/3, 0x2E + t/2);
        gfx_rect_fill(0, y, W, 1, col);
    }
}

static void redraw_full(void) {
    cur_drawn_x = cur_drawn_y = -1;     /* cursor will be re-drawn */
    draw_background();
    int idxs[WM_MAX_WINDOWS]; int n = z_sorted(idxs);
    for (int i = 0; i < n; i++) draw_window(&windows[idxs[i]]);
    draw_taskbar();
    if (start_menu_open) draw_start_menu();
    need_redraw = false;
}

/* ---- mouse hit testing ---- */

static int hit_window_idx(int mx, int my) {
    int idxs[WM_MAX_WINDOWS]; int n = z_sorted(idxs);
    for (int i = n - 1; i >= 0; i--) {
        window_t* w = &windows[idxs[i]];
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h)
            return idxs[i];
    }
    return -1;
}

static bool hit_close_button(window_t* w, int mx, int my) {
    int cbx = w->x + w->w - 22, cby = w->y + 3;
    return mx >= cbx && mx < cbx + 16 && my >= cby && my < cby + 16;
}
static bool hit_title_bar(window_t* w, int mx, int my) {
    return mx >= w->x && mx < w->x + w->w - 26 && my >= w->y && my < w->y + WM_TITLE_H;
}

/* ---- start menu actions ---- */

static const char welcome_text[] =
    "Welcome to SamaraOS Desktop!\n"
    "\n"
    "* Drag any window by its title bar.\n"
    "* Click X (red) to close a window.\n"
    "* Use Start menu to open more.\n"
    "* Esc returns to text shell.\n";

static const char about_text[] =
    "SamaraOS 0.5\n"
    "\n"
    "Hobby OS in C, x86 / Multiboot.\n"
    "\n"
    "* Preemptive multitasking\n"
    "* In-memory file system\n"
    "* PS/2 keyboard + mouse\n"
    "* VBE/13h linear framebuffer\n"
    "* Window manager with Start menu\n"
    "* Foundation for app porting (eg DOOM)\n";

static void start_menu_action(int idx) {
    start_menu_open = false;
    need_redraw = true;
    int W = gfx_w(), H = gfx_h();
    switch (idx) {
        case 0:  wm_open_terminal(40, 40); break;
        case 1:  browser_open(NULL); break;
        case 2:  mediaplayer_open(); break;
        case 3:  paint_open(); break;
        case 4:  clock_open(); break;
        case 5:  wm_open_info(W/2 - 180, H/2 - 110, 360, 180, "About",  about_text);  break;
        case 6:  wm_open_info(W/2 - 220, H/2 - 110, 440, 180, "Welcome", welcome_text); break;
        case 7:  wm_open_info(W/2 - 200, H/2 - 110, 400, 200, "System Info", wm_sysinfo_text()); break;
        case 8:  while (inb(0x64) & 0x02) {} outb(0x64, 0xFE); break;
        case 9:  outw(0x604, 0x2000); outw(0xB004, 0x2000); break;
        case 10: exit_requested = true; break;
    }
}

/* ---- mouse event handling ---- */

static void on_press(int mx, int my) {
    /* start menu hovering over an item -> activate */
    if (start_menu_open) {
        if (mx >= menu_x && mx < menu_x + menu_w &&
            my >= menu_y && my < menu_y + menu_h) {
            int item = (my - menu_y - 5) / 28;
            if (item >= 0 && item < n_start_items()) { start_menu_action(item); return; }
        }
        start_menu_open = false; need_redraw = true; return;
    }
    /* start button */
    if (mx >= start_btn_x && mx < start_btn_x + start_btn_w &&
        my >= start_btn_y && my < start_btn_y + start_btn_h) {
        start_menu_open = !start_menu_open; need_redraw = true; return;
    }
    /* taskbar window button (skip start) -> focus that window */
    int H = gfx_h();
    if (my >= H - taskbar_h && my < H) {
        int idxs[WM_MAX_WINDOWS]; int n = z_sorted(idxs);
        int wx = start_btn_x + start_btn_w + 8;
        int W = gfx_w();
        for (int i = 0; i < n; i++) {
            int bw = 160;
            if (wx + bw > W - 90) break;
            if (mx >= wx && mx < wx + bw) { focus_idx(idxs[i]); need_redraw = true; return; }
            wx += bw + 4;
        }
        return;
    }
    /* window hit */
    int wi = hit_window_idx(mx, my);
    if (wi >= 0) {
        window_t* w = &windows[wi];
        focus_idx(wi);
        press_target_idx = wi;
        if (hit_close_button(w, mx, my)) {
            wm_close(w);
            press_target_idx = -1;
        } else if (hit_title_bar(w, mx, my)) {
            w->dragging = true;
            w->drag_off_x = mx - w->x;
            w->drag_off_y = my - w->y;
        } else if (w->type == WIN_APP && w->on_click) {
            int cx, cy, cw, ch;
            wm_client_rect(w, &cx, &cy, &cw, &ch);
            if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) {
                w->on_click(w, mx - cx, my - cy);
                w->needs_repaint = true;
            }
        }
        need_redraw = true;
    }
}

static void on_drag(int mx, int my) {
    bool moved_window = false;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t* w = &windows[i];
        if (!w->open || !w->dragging) continue;
        int W = gfx_w(), H = gfx_h() - taskbar_h;
        int nx = mx - w->drag_off_x;
        int ny = my - w->drag_off_y;
        if (nx < -w->w + 60) nx = -w->w + 60;
        if (ny < 0) ny = 0;
        if (nx > W - 60) nx = W - 60;
        if (ny > H - 22) ny = H - 22;
        if (nx != w->x || ny != w->y) {
            w->x = nx; w->y = ny;
            need_redraw = true;
        }
        moved_window = true;
    }
    /* If no title-bar drag, relay drag to focused app window's client area */
    if (!moved_window && focused_idx >= 0) {
        window_t* w = &windows[focused_idx];
        if (w->open && w->type == WIN_APP && w->on_click) {
            int cx, cy, cw, ch;
            wm_client_rect(w, &cx, &cy, &cw, &ch);
            if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) {
                w->on_click(w, mx - cx, my - cy);
                w->needs_repaint = true;
                need_redraw = true;
            }
        }
    }
}

static void on_release(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (windows[i].open && windows[i].dragging) {
            windows[i].dragging = false;
            need_redraw = true;
        }
    /* Fire on_release on the window that originally caught the press, even if
       focus has since drifted — apps use this to finish drags or debounce. */
    if (press_target_idx >= 0) {
        window_t* w = &windows[press_target_idx];
        if (w->open && w->type == WIN_APP && w->on_release) {
            w->on_release(w);
            w->needs_repaint = true;
            need_redraw = true;
        }
        press_target_idx = -1;
    }
}

/* ---- main loop ---- */

void wm_init(void) {
    memset(windows, 0, sizeof(windows));
    next_z = 0;
    focused_idx = -1;
    terminal_idx = -1;
    start_menu_open = false;
    exit_requested = false;
    need_redraw = true;
    cur_drawn_x = cur_drawn_y = -1;
    prev_btn = false;
    prev_mx = prev_my = -1;
    hover_item = -1;
    db_on = gfx_enable_double_buffer();
}

static int      last_clock_sec   = -1;
static uint32_t last_layout_epoch = 0;

/* PIT runs at 100 Hz (10 ms ticks). Targeting 30 ms = ~33 fps fits 3 ticks
   exactly and keeps the hlt-pacer dead-stable without bumping IRQ0 rate. */
#define FRAME_MS 30

void wm_run(void) {
    uint32_t next_frame = pit_uptime_ms();

    while (!exit_requested) {
        uint32_t now_ms = pit_uptime_ms();

        int mx, my; uint8_t b;
        mouse_get(&mx, &my, &b);
        bool button = (b & 1) != 0;

        /* update hover for start menu */
        if (start_menu_open) {
            int new_hover = -1;
            if (mx >= menu_x && mx < menu_x + menu_w &&
                my >= menu_y && my < menu_y + menu_h) {
                int item = (my - menu_y - 5) / 28;
                if (item >= 0 && item < n_start_items()) new_hover = item;
            }
            if (new_hover != hover_item) { hover_item = new_hover; need_redraw = true; }
        }

        if (button && !prev_btn) on_press(mx, my);
        else if (!button && prev_btn) on_release();
        else if (button && prev_btn) on_drag(mx, my);
        prev_btn = button;

        /* keys */
        while (kbd_has_key()) {
            char c = kbd_trygetc();
            if (c == 0x1B && !start_menu_open) { exit_requested = true; break; }
            if (c == 0x1B && start_menu_open) { start_menu_open = false; need_redraw = true; continue; }
            if (focused_idx >= 0 && windows[focused_idx].open) {
                window_t* w = &windows[focused_idx];
                if (w->type == WIN_TERMINAL) wm_terminal_handle_key(w, c);
                else if (w->type == WIN_APP && w->on_key) { w->on_key(w, c); w->needs_repaint = true; }
            }
        }

        /* per-frame tick for animated apps */
        bool any_animating = false;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            window_t* w = &windows[i];
            if (!w->open || w->type != WIN_APP) continue;
            if (w->on_tick) w->on_tick(w, now_ms);
            if (w->animate || w->needs_repaint) any_animating = true;
        }
        if (any_animating) need_redraw = true;

        /* clock tick — refresh once per second */
        int s = (int)(now_ms / 1000);
        if (s != last_clock_sec) {
            last_clock_sec = s;
            need_redraw = true;
        }

        /* layout toggle (F11/F12) happens in the keyboard ISR — sample its
           epoch each frame so the taskbar indicator flips instantly. */
        uint32_t le = kbd_layout_epoch();
        if (le != last_layout_epoch) {
            last_layout_epoch = le;
            need_redraw = true;
        }

        bool cursor_moved = (mx != prev_mx || my != prev_my);

        if (need_redraw || cursor_moved) {
            if (db_on) {
                gfx_target_back();
                if (need_redraw) {
                    redraw_full();
                    /* paint app windows on top */
                    int idxs[WM_MAX_WINDOWS]; int n = z_sorted(idxs);
                    for (int i = 0; i < n; i++) {
                        window_t* w = &windows[idxs[i]];
                        if (w->type == WIN_APP && w->on_paint) {
                            w->on_paint(w);
                            w->needs_repaint = false;
                        }
                    }
                }
                /* Stamp cursor into the back buffer LAST, then present so the
                   user never sees a cursorless frame. After present, restore
                   the under-cursor pixels in the back so the back buffer stays
                   "clean" (no cursor) for the next paint cycle. */
                gfx_save_rect(mx, my, CUR_W, CUR_H, cursor_under);
                stamp_cursor_pixels(mx, my);
                gfx_target_front();
                gfx_present();
                gfx_target_back();
                gfx_restore_rect(mx, my, CUR_W, CUR_H, cursor_under);
                gfx_target_front();
                cur_drawn_x = mx; cur_drawn_y = my;
            } else {
                /* No double buffer: classic save/restore on the front buffer */
                hide_cursor();
                if (need_redraw) {
                    redraw_full();
                    int idxs[WM_MAX_WINDOWS]; int n = z_sorted(idxs);
                    for (int i = 0; i < n; i++) {
                        window_t* w = &windows[idxs[i]];
                        if (w->type == WIN_APP && w->on_paint) {
                            w->on_paint(w);
                            w->needs_repaint = false;
                        }
                    }
                }
                draw_cursor(mx, my);
            }
            need_redraw = false;
            prev_mx = mx; prev_my = my;
        }

        /* Frame pacer: hlt until next 16ms slot. PIT IRQ wakes us up. */
        next_frame += FRAME_MS;
        uint32_t cur = pit_uptime_ms();
        if ((int32_t)(next_frame - cur) < -100) next_frame = cur;  /* resync if lagging */
        while ((int32_t)(pit_uptime_ms() - next_frame) < 0) {
            __asm__ volatile ("hlt");
        }
    }

    if (db_on) { gfx_disable_double_buffer(); db_on = false; }
}
