#include "../shell_priv.h"
#include "apps/browser.h"
#include "apps/clock.h"
#include "apps/doom.h"
#include "apps/mediaplayer.h"
#include "apps/paint.h"
#include "apps/snake.h"
#include "core/string.h"
#include "gui/uwin.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "gfx/gfx.h"

void cmd_doom_mini(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (doom_load_from_disk() != 0) {
    vga_puts("doom-mini: ");
    vga_puts(doom_status());
    vga_putc('\n');
    return;
  }
  vga_puts("doom-mini: ");
  vga_puts(doom_status());
  vga_putc('\n');
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("doom-mini: enter 'desktop' first to view TITLEPIC\n");
    return;
  }
  doom_open_window();
}

void cmd_play(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("play: enter 'desktop' first\n");
    return;
  }
  int r = doom_play_e1m1();
  if (r != 0) {
    vga_puts("play: ");
    vga_puts(doom_status());
    vga_putc('\n');
  }
}

extern int samara_doom_launch(void);
extern const char *samara_doom_status(void);

void cmd_doom(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("doom: enter 'desktop' first\n");
    return;
  }
  int r = samara_doom_launch();
  vga_puts("doom: ");
  vga_puts(samara_doom_status());
  vga_putc('\n');
  (void)r;
}

void cmd_snake(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("snake: enter 'desktop' first\n");
    return;
  }
  if (snake_open() != 0)
    vga_puts("snake: failed to open window\n");
}

void cmd_player(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("player: enter 'desktop' first\n");
    return;
  }
  if (mediaplayer_open() != 0)
    vga_puts("player: failed to open\n");
}

void cmd_paint(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("paint: enter 'desktop' first\n");
    return;
  }
  if (paint_open() != 0)
    vga_puts("paint: failed to open\n");
}

void cmd_clock(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("clock: enter 'desktop' first\n");
    return;
  }
  if (clock_open() != 0)
    vga_puts("clock: failed to open\n");
}

/* Launches the desktop (if not already) and opens the browser there. */
void cmd_browser(int argc, char **argv) {
  /* If a graphics mode is up and the WM is alive, just nudge browser_open.
     Otherwise, kick off the desktop which auto-opens it via the start menu. */
  if (!uwin_wm_running()) {             /* graphics alone is the console now */
    vga_puts("browser: needs graphical desktop — run 'desktop' first\n");
    return;
  }
  const char *url = (argc >= 2) ? argv[1] : NULL;
  if (browser_open(url) != 0)
    vga_puts("browser: failed to open\n");
}

void cmd_klayout(int argc, char **argv) {
  if (argc >= 2) {
    if (!strcmp(argv[1], "ru") || !strcmp(argv[1], "RU"))
      kbd_set_ru(true);
    else if (!strcmp(argv[1], "en") || !strcmp(argv[1], "EN"))
      kbd_set_ru(false);
    else {
      vga_puts("klayout: usage: klayout [ru|en]    (Alt+Shift toggles)\n");
      return;
    }
  } else {
    kbd_set_ru(!kbd_is_ru());
  }
  vga_puts("layout: ");
  vga_puts(kbd_is_ru() ? "RU\n" : "EN\n");
}
