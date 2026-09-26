#include "gui/desktop.h"
#include "gfx/termfont.h"
#include "gfx/gfx_term.h"
#include "drivers/vga.h"
#include "drivers/mouse.h"
#include "core/string.h"
#include "gfx/gfx.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "boot/pit.h"
#include "core/string.h"
#include "core/io.h"
#include "drivers/vga.h"

#define BG       RGB(0x1A, 0x1F, 0x3A)
#define BAR      RGB(0x10, 0x14, 0x28)
#define BAR_HI   RGB(0x6A, 0x82, 0xFB)
#define WIN_BG   RGB(0xF0, 0xF0, 0xF5)
#define WIN_FG   RGB(0x10, 0x10, 0x18)
#define TITLE    RGB(0x42, 0x53, 0xD4)
#define TITLE_HI RGB(0x6E, 0x82, 0xFF)
#define WHITE    RGB(0xFF, 0xFF, 0xFF)
#define BLACK    RGB(0x00, 0x00, 0x00)
#define GREY     RGB(0x90, 0x90, 0xA0)
#define DGREY    RGB(0x40, 0x44, 0x58)
#define RED      RGB(0xE0, 0x40, 0x40)
#define GREEN    RGB(0x40, 0xC0, 0x60)

#define CUR_W 12
#define CUR_H 19

/* Arrow cursor — '#' = black outline, '*' = white fill, ' ' = transparent.
   Each row must be exactly CUR_W (12) characters long. */
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
    "#****#####  ",
    "#***# #*#   ",
    "#**#  #*#   ",
    "#*#    ##   ",
    "##          ",
    "            ",
    "            ",
    "            ",
};

static multiboot_info_t* g_mbi = 0;

void desktop_install_mbi(multiboot_info_t* mbi) { g_mbi = mbi; }

/* "video=WxH" on the kernel command line picks the resolution (QEMU shows
   it 1:1 when it fits the host screen - no blurry scaling). */
static bool video_param(int* w, int* h) {
    extern const char* kernel_cmdline(void);
    const char* k = strstr(kernel_cmdline(), "video=");
    if (!k) return false;
    k += 6;
    int a = 0, b = 0;
    while (*k >= '0' && *k <= '9') a = a * 10 + (*k++ - '0');
    if (*k++ != 'x') return false;
    while (*k >= '0' && *k <= '9') b = b * 10 + (*k++ - '0');
    if (a < 640 || b < 400 || a > 4096 || b > 2160) return false;
    *w = a; *h = b;
    return true;
}

bool desktop_init_graphics(void) {
    int vw, vh;
    if (video_param(&vw, &vh) && gfx_init_vbe(vw, vh, 32)) return true;
    /* Try 1080p first (QEMU stdvga happily does it with 16 MiB VRAM).
       Fall back through common modes if the card refuses. */
    bool ok = gfx_init_vbe(1920, 1080, 32);
    if (!ok) ok = gfx_init_vbe(1600, 900,  32);
    if (!ok) ok = gfx_init_vbe(1280, 800,  32);
    if (!ok) ok = gfx_init_vbe(1024, 768,  32);
    if (!ok && g_mbi) ok = gfx_init(g_mbi);
    if (!ok)          ok = gfx_init_mode13h();
    return ok;
}

static void draw_taskbar(int boot_sec_offset) {
    int W = gfx_w();
    gfx_rect_fill(0, 0, W, 28, BAR);
    gfx_rect_fill(0, 26, W, 2, BAR_HI);
    gfx_string(8, 6, "SamaraOS", WHITE, BAR, false);
    /* live "uptime" clock */
    uint32_t s = pit_uptime_ms() / 1000;
    s -= (uint32_t)boot_sec_offset;
    int h = (int)(s / 3600), m = (int)((s / 60) % 60), sec = (int)(s % 60);
    char buf[16]; int i = 0;
    buf[i++] = '0' + (h / 10) % 10;
    buf[i++] = '0' + h % 10;
    buf[i++] = ':';
    buf[i++] = '0' + (m / 10) % 10;
    buf[i++] = '0' + m % 10;
    buf[i++] = ':';
    buf[i++] = '0' + (sec / 10) % 10;
    buf[i++] = '0' + sec % 10;
    buf[i] = 0;
    gfx_rect_fill(W - 88, 4, 80, 20, BAR);
    gfx_string(W - 80, 6, buf, WHITE, BAR, false);
}

void desktop_draw_chrome(int boot_sec_offset) {
    int W = gfx_w(), H = gfx_h();
    if (W >= 640) {
        for (int y = 0; y < H; y++) {
            uint8_t t = (uint8_t)((y * 64) / H);
            uint32_t col = RGB(0x14 + t/3, 0x18 + t/3, 0x2E + t/2);
            gfx_rect_fill(0, y, W, 1, col);
        }
    } else {
        gfx_clear(BG);
    }
    draw_taskbar(boot_sec_offset);
}

void desktop_draw_terminal_window(int x, int y, int w, int h, const char* title) {
    /* shadow */
    gfx_rect_fill(x + 4, y + 4, w, h, RGB(0, 0, 0));
    /* body */
    gfx_rect_fill(x, y, w, h, BLACK);
    /* title bar */
    gfx_rect_fill(x, y, w, 24, TITLE);
    gfx_rect_fill(x, y, w, 1, TITLE_HI);
    gfx_string(x + 8, y + 4, title, WHITE, TITLE, false);
    /* close button look */
    gfx_rect_fill(x + w - 22, y + 4, 16, 16, RED);
    gfx_string(x + w - 18, y + 4, "x", WHITE, RED, false);
    /* outer border */
    gfx_rect(x, y, w, h, DGREY);
}

static void draw_window(int x, int y, int w, int h, const char* title) {
    /* shadow */
    gfx_rect_fill(x + 4, y + 4, w, h, RGB(0, 0, 0));
    /* body */
    gfx_rect_fill(x, y, w, h, WIN_BG);
    /* title bar */
    gfx_rect_fill(x, y, w, 24, TITLE);
    gfx_rect_fill(x, y, w, 1, TITLE_HI);
    gfx_string(x + 8, y + 4, title, WHITE, TITLE, false);
    /* close button */
    gfx_rect_fill(x + w - 22, y + 4, 16, 16, RED);
    gfx_string(x + w - 18, y + 4, "x", WHITE, RED, false);
    /* border */
    gfx_rect(x, y, w, h, DGREY);
}

static void draw_button(int x, int y, int w, int h, const char* label) {
    gfx_rect_fill(x, y, w, h, RGB(0xE0, 0xE5, 0xF0));
    gfx_rect(x, y, w, h, DGREY);
    int tw = (int)strlen(label) * 8;
    gfx_string(x + (w - tw) / 2, y + (h - 16) / 2, label, WIN_FG, 0, false);
}

static void draw_static_scene(void) {
    int W = gfx_w(), H = gfx_h();
    bool small = (W < 640);

    /* Background: gradient on hi-res, flat fill on small. */
    if (!small) {
        for (int y = 0; y < H; y++) {
            uint8_t t = (uint8_t)((y * 64) / H);
            uint32_t col = RGB(0x14 + t/3, 0x18 + t/3, 0x2E + t/2);
            gfx_rect_fill(0, y, W, 1, col);
        }
    } else {
        gfx_clear(BG);
    }

    draw_taskbar(0);

    if (!small) {
        /* Two windows side by side. */
        int wx = W / 8, wy = 80;
        int ww = W / 2, wh = 240;
        if (ww < 360) ww = 360;
        draw_window(wx, wy, ww, wh, "Welcome to SamaraOS");
        gfx_string(wx + 16, wy + 40,  "Hello! This is a real graphical desktop.", WIN_FG, WIN_BG, false);
        gfx_string(wx + 16, wy + 60,  "Built from C, Multiboot, no libc.",        WIN_FG, WIN_BG, false);
        gfx_string(wx + 16, wy + 80,  "Move the mouse around (PS/2).",            WIN_FG, WIN_BG, false);
        gfx_string(wx + 16, wy + 100, "Press Esc to return to the shell.",        DGREY,  WIN_BG, false);
        draw_button(wx + 16, wy + wh - 40, 100, 24, "OK");
        draw_button(wx + 130, wy + wh - 40, 100, 24, "Cancel");

        int sx = wx + ww + 24, sy = wy;
        if (sx + 320 > W - 16) sx = W - 340;
        int swh = 200;
        draw_window(sx, sy, 320, swh, "System");
        gfx_string(sx + 16, sy + 40,  "OS:    SamaraOS 0.3", WIN_FG, WIN_BG, false);
        gfx_string(sx + 16, sy + 60,  "Kernel: samara 0.3", WIN_FG, WIN_BG, false);
        gfx_string(sx + 16, sy + 80,  "CPU:   i686 (qemu)", WIN_FG, WIN_BG, false);

        char res[40] = "Mode:  ";
        char tmp[16];
        itoa(gfx_w(), tmp, 10); strcat(res, tmp);
        strcat(res, " x ");
        itoa(gfx_h(), tmp, 10); strcat(res, tmp);
        gfx_string(sx + 16, sy + 100, res, WIN_FG, WIN_BG, false);
        gfx_string(sx + 16, sy + 120, "FB:    32 bpp linear", WIN_FG, WIN_BG, false);
        gfx_string(sx + 16, sy + 140, "Press Esc -> shell.", GREY, WIN_BG, false);

        gfx_rect_fill(0, H - 22, W, 22, BAR);
        gfx_rect_fill(0, H - 22, W, 1, BAR_HI);
        gfx_string(8, H - 18, "[Esc] Shell    [Mouse] move    real framebuffer", WHITE, BAR, false);
    } else {
        /* Single centered window for low-res (mode 13h, 320x200). */
        int wy = 32;
        int ww = W - 16, wh = H - wy - 8;
        int wx = (W - ww) / 2;
        draw_window(wx, wy, ww, wh, "Welcome");
        gfx_string(wx + 6, wy + 32, "SamaraOS desktop",  WIN_FG, WIN_BG, false);
        char res[24] = "";
        char tmp[12];
        itoa(W, tmp, 10); strcat(res, tmp); strcat(res, " x ");
        itoa(H, tmp, 10); strcat(res, tmp);
        gfx_string(wx + 6, wy + 52, res,                 WIN_FG, WIN_BG, false);
        gfx_string(wx + 6, wy + 72, "Mouse: PS/2",        WIN_FG, WIN_BG, false);
        gfx_string(wx + 6, wy + 92, "Esc -> shell",       DGREY,  WIN_BG, false);
    }
}

int desktop_run(void) {
    if (!desktop_init_graphics()) return -1;

    mouse_set_text_cursor(false);
    int prev_max_x, prev_max_y;
    mouse_get_range(&prev_max_x, &prev_max_y);
    mouse_set_range(gfx_w() - 1, gfx_h() - 1);
    mouse_set_pos(gfx_w() / 2, gfx_h() / 2);

    draw_static_scene();

    static uint32_t under[CUR_W * CUR_H];
    int prev_x = -1, prev_y = -1;
    uint32_t boot_sec_offset = pit_uptime_ms() / 1000;
    int last_clock = -1;

    while (1) {
        int sx, sy; uint8_t bt;
        mouse_get(&sx, &sy, &bt);

        /* clock once per second */
        int now = (int)(pit_uptime_ms() / 1000) - (int)boot_sec_offset;
        if (now != last_clock) {
            if (prev_x >= 0) gfx_restore_rect(prev_x, prev_y, CUR_W, CUR_H, under);
            draw_taskbar((int)boot_sec_offset);
            last_clock = now;
            prev_x = -1;
        }

        if (sx != prev_x || sy != prev_y) {
            if (prev_x >= 0) gfx_restore_rect(prev_x, prev_y, CUR_W, CUR_H, under);
            gfx_save_rect(sx, sy, CUR_W, CUR_H, under);
            for (int r = 0; r < CUR_H; r++)
                for (int c = 0; c < CUR_W; c++) {
                    char p = cursor_pix[r][c];
                    if (p == '#') gfx_pixel(sx + c, sy + r, BLACK);
                    else if (p == '*') gfx_pixel(sx + c, sy + r, WHITE);
                }
            prev_x = sx;
            prev_y = sy;
        }

        if (kbd_has_key()) {
            char k = kbd_trygetc();
            if (k == 0x1B) {        /* Esc */
                /* restore mouse + signal main code to switch back to text mode */
                mouse_set_range(prev_max_x, prev_max_y);
                mouse_set_text_cursor(true);
                return 0;
            }
        }

        for (volatile int i = 0; i < 50000; i++) { __asm__ volatile (""); }
    }
}


/* ======================================================================
   Graphical console: the SamaraOS shell full screen on the framebuffer,
   with the desktop terminal's engine (true colour, anti-aliased Unicode
   font in several sizes) instead of the 16-colour VGA text mode.
   ====================================================================== */

static bool con_gfx;
static int  con_font = -1;

bool console_is_gfx(void) { return con_gfx; }

/* Grid for the current font, centred on the screen. Returns lines the
   content moved up (the cursor row stays visible). */
static int con_layout(void) {
    int cw = termfont_cw(), ch = termfont_ch();
    int cols = gfx_w() / cw, rows = gfx_h() / ch;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    int shift = gfx_term_resize(cols, rows);
    int ox = (gfx_w() - cols * cw) / 2, oy = (gfx_h() - rows * ch) / 2;
    gfx_reset_clip();
    gfx_rect_fill(0, 0, gfx_w(), gfx_h(), gfx_term_color(0));
    gfx_term_repos(ox, oy);
    return shift;
}

bool console_gfx_start(void) {
    if (!desktop_init_graphics()) return false;
    gfx_reset_clip();
    mouse_set_text_cursor(false);
    if (con_font < 0) con_font = gfx_w() >= 1600 ? 1 : 0;         /* 10x20 on big screens */
    termfont_set(con_font);
    int cw = termfont_cw(), ch = termfont_ch();
    int cols = gfx_w() / cw, rows = gfx_h() / ch;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    gfx_term_resize(cols, rows);
    gfx_rect_fill(0, 0, gfx_w(), gfx_h(), gfx_term_color(0));
    vga_use_gfx_term((gfx_w() - cols * cw) / 2, (gfx_h() - rows * ch) / 2);
    gfx_term_show_cursor(true);
    con_gfx = true;
    return true;
}

/* Ctrl +/-: next/previous font size. Returns lines the content moved up. */
int console_font_step(int d) {
    int n = termfont_get() + d;
    if (n < 0 || n >= termfont_sizes()) return 0;
    termfont_set(n);
    if (con_gfx && vga_is_gfx()) con_font = n;
    return con_layout();
}
