#include "../shell_priv.h"
#include "core/string.h"
#include "gui/uwin.h"
#include "fs/fs.h"
#include "apps/mediaplayer.h"
#include "boot/pit.h"
#include "drivers/ata.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/vga.h"
#include "gfx/font.h"
#include "gfx/gfx.h"
#include "gui/desktop.h"
#include "gui/wm.h"

/* ===================== sample App: bouncing ball ===================== */
/* Demonstrates the wm_open_app API used for porting future apps (e.g. DOOM):
   provide on_paint to draw the window's client area, on_key for input. */

typedef struct {
  int x, y, vx, vy;
  uint32_t last_ms;
} bounce_state_t;

static bounce_state_t bounce_st;

static void bounce_paint(window_t *w) {
  int cx, cy, cw, ch;
  wm_client_rect(w, &cx, &cy, &cw, &ch);
  bounce_state_t *s = (bounce_state_t *)w->user;

  uint32_t now = pit_uptime_ms();
  uint32_t dt = now - s->last_ms;
  s->last_ms = now;
  int steps = (int)(dt / 16);
  if (steps < 1)
    steps = 1;
  if (steps > 6)
    steps = 6;
  int r = 12;
  for (int i = 0; i < steps; i++) {
    s->x += s->vx;
    s->y += s->vy;
    if (s->x < r) {
      s->x = r;
      s->vx = -s->vx;
    }
    if (s->y < r) {
      s->y = r;
      s->vy = -s->vy;
    }
    if (s->x > cw - r) {
      s->x = cw - r;
      s->vx = -s->vx;
    }
    if (s->y > ch - r) {
      s->y = ch - r;
      s->vy = -s->vy;
    }
  }

  gfx_rect_fill(cx, cy, cw, ch, RGB(0x10, 0x18, 0x30));
  int bx = cx + s->x, by = cy + s->y;
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r)
        gfx_pixel(bx + dx, by + dy, RGB(0xFF, 0x80, 0x40));
  gfx_string(cx + 6, cy + 6, "Sample App  (paint+key callbacks)",
             RGB(0xFF, 0xFF, 0xFF), 0, false);
  gfx_string(cx + 6, cy + 22, "Arrows kick ball, Esc closes",
             RGB(0x90, 0xA0, 0xC0), 0, false);
}

static void bounce_key(window_t *w, char c) {
  bounce_state_t *s = (bounce_state_t *)w->user;
  if (c == (char)K_LEFT)
    s->vx -= 1;
  if (c == (char)K_RIGHT)
    s->vx += 1;
  if (c == (char)K_UP)
    s->vy -= 1;
  if (c == (char)K_DOWN)
    s->vy += 1;
  if (c == 0x1B)
    wm_close(w);
}

void cmd_bounce(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("bounce: enter 'desktop' first\n");
    return;
  }
  bounce_st.x = 100;
  bounce_st.y = 60;
  bounce_st.vx = 2;
  bounce_st.vy = 2;
  bounce_st.last_ms = pit_uptime_ms();
  wm_open_app(120, 120, 380, 240, "Bounce", bounce_paint, bounce_key,
              &bounce_st);
}

void cmd_disk(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!ata_present() && !ata_init()) {
    vga_puts("ata: no disk found on primary master\n");
    return;
  }
  uint32_t s = ata_total_sectors();
  vga_printf("ata: primary master  %u sectors  (~%u KiB / ~%u MiB)\n", s,
             (s * 512U) / 1024U, (s * 512U) / (1024U * 1024U));
}

/* ===================== desktop entry ===================== */

void cmd_desktop(int argc, char **argv) {
  (void)argc;
  (void)argv;

  if (!desktop_init_graphics()) {
    vga_puts("desktop: no graphics available\n");
    return;
  }

  int prev_max_x, prev_max_y;
  mouse_get_range(&prev_max_x, &prev_max_y);
  mouse_set_text_cursor(false);
  mouse_set_range(gfx_w() - 1, gfx_h() - 1);
  mouse_set_pos(gfx_w() / 2, gfx_h() / 2);

  wm_init();
  /* open a Welcome info window and a Terminal so user sees something */
  static const char welcome[] = "Welcome to SamaraOS.\n"
                                "\n"
                                "github.com/nekzzer/SamaraOS\n"
                                "\n"
                                "Drag windows by the title bar.\n"
                                "Double-click an icon on the left, or\n"
                                "open 'samara' in the taskbar. Esc: text\n"
                                "shell. 'wallpaper file.bmp' sets a wallpaper.\n";
  wm_open_info(gfx_w() - 500, 60, 440, 220, "Welcome", welcome);
  wm_open_terminal(120, 64);

  wm_run();

  /* cleanup */
  mediaplayer_force_stop();
  g_in_wm_terminal = false;
  g_current_term_window = NULL;
  if (console_is_gfx()) {             /* back to the graphical console */
    console_gfx_start();
  } else {
    vga_use_text();
    vga_set_text_mode_3();
    font_restore();
    mouse_set_text_cursor(true);
    mouse_set_range(prev_max_x, prev_max_y);
    vga_init();
  }
  vga_set_color(VGA_LCYAN, VGA_BLACK);
  vga_puts("returned from desktop.\n");
  vga_set_color(VGA_LGREY, VGA_BLACK);
}

/* ===================== wallpaper ===================== */

static const char *const wp_paths[] = {"/home/user/wallpaper.bmp", "/mnt/wallpaper.bmp",
                                       "/wallpaper.bmp"};

static bool bmp_usable(const fs_node_t *n) {
  if (!n || n->type != FS_FILE || n->size < 54 || !n->data)
    return false;
  const uint8_t *b = (const uint8_t *)n->data;
  int bpp = b[28] | (b[29] << 8);
  uint32_t comp = b[30] | (b[31] << 8) | (b[32] << 16) | ((uint32_t)b[33] << 24);
  return b[0] == 'B' && b[1] == 'M' && (bpp == 24 || bpp == 32) &&
         (comp == 0 || (comp == 3 && bpp == 32));
}

/* wallpaper [file.bmp | off]: installs a BMP as the desktop wallpaper
   (kept on /mnt when the persistent disk is mounted), removes it, or with
   no argument reloads/shows the current one. */
void cmd_wallpaper(int argc, char **argv) {
  fs_node_t *mnt = fs_resolve(fs_root(), "/mnt");
  bool persist = mnt && mnt->mount_id;
  const char *dst = persist ? "/mnt/wallpaper.bmp" : "/home/user/wallpaper.bmp";

  if (argc < 2) {
    for (unsigned i = 0; i < sizeof(wp_paths) / sizeof(wp_paths[0]); i++)
      if (bmp_usable(fs_resolve(fs_root(), wp_paths[i]))) {
        vga_printf("wallpaper: %s\n", wp_paths[i]);
        wm_invalidate_wallpaper();
        return;
      }
    vga_puts("wallpaper: none (usage: wallpaper <file.bmp> | wallpaper off)\n");
    wm_invalidate_wallpaper();
    return;
  }
  if (!strcmp(argv[1], "off")) {
    for (unsigned i = 0; i < sizeof(wp_paths) / sizeof(wp_paths[0]); i++)
      if (fs_resolve(fs_root(), wp_paths[i]))
        fs_unlink(fs_root(), wp_paths[i]);
    wm_invalidate_wallpaper();
    vga_puts("wallpaper: off\n");
    return;
  }
  fs_node_t *src = fs_resolve(cwd, argv[1]);
  if (!src || src->type != FS_FILE) {
    vga_printf("wallpaper: %s: no such file\n", argv[1]);
    return;
  }
  if (!bmp_usable(src)) {
    vga_puts("wallpaper: need an uncompressed 24/32-bit BMP\n");
    return;
  }
  fs_node_t *d = fs_resolve(fs_root(), dst);
  if (d != src) {
    if (!d)
      d = fs_create(fs_root(), dst, FS_FILE);
    if (!d || fs_write(d, src->data, src->size) < 0) {
      vga_printf("wallpaper: cannot write %s\n", dst);
      return;
    }
  }
  /* /home/user wins the lookup: drop a stale copy there. */
  if (persist && fs_resolve(fs_root(), wp_paths[0]) && fs_resolve(fs_root(), wp_paths[0]) != src)
    fs_unlink(fs_root(), wp_paths[0]);
  wm_invalidate_wallpaper();
  vga_printf("wallpaper: %s -> %s\n", argv[1], dst);
}
