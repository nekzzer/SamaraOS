#include "../shell_priv.h"
#include "drivers/vga.h"

void cmd_help(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts(
      "files:   ls cd cat pwd mkdir touch rm cp mv head tail wc grep find\n"
      "edit:    nano <file> (GNU nano)   edit <file> (built-in, ^S save ^X exit)\n"
      "system:  uptime mem df ps mouse uname whoami date about history\n"
      "shell:   help clear echo calc sleep beep history exit reboot shutdown\n"
      "gfx:     desktop              terminal-on-desktop graphical mode\n"
      "         neofetch\n"
      "apps:    player (music)  paint  clock  bounce  doom  snake\n"
      "         doom-mini (in-tree mini engine TITLEPIC)  play (mini E1M1)\n"
      "audio:   playwav <file>  stopwav  sbinfo   (try: playwav welcome.wav)\n"
      "fat:     fatmount [drv]  fatls  fatload <name> [ram]  playfat <name>\n"
      "net:     ifconfig  ping <ip> [count]  wget <http://ip[:port]/path> "
      "[file]\n"
      "         browser [url]   www [url]    (full web browser in desktop)\n"
      "i18n:    klayout [ru|en]              (F11 toggles, indicator in "
      "taskbar)\n"
      "python:  python [file.py]   MicroPython (demos in /usr/src/py)\n"
      "Line ed: Left/Right Home/End Up/Down history Del ^C ^L ^D\n");
}

void cmd_clear(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_clear();
}

void cmd_echo(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    vga_puts(argv[i]);
    if (i < argc - 1)
      vga_putc(' ');
  }
  vga_putc('\n');
}
