#include "../shell_priv.h"
#include "boot/pit.h"
#include "core/heap.h"
#include "core/io.h"
#include "core/string.h"
#include "core/task.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "gfx/gfx.h"
#include "gui/wm.h"
#include "net/net.h"

void cmd_uptime(int argc, char **argv) {
  (void)argc;
  (void)argv;
  uint32_t ms = pit_uptime_ms();
  vga_printf("up %u ms (%u s)  ticks=%u\n", ms, ms / 1000, pit_ticks());
}

void cmd_mem(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_printf("heap: %u / %u bytes used\n", (uint32_t)heap_used(),
             (uint32_t)heap_total());
}

void cmd_df(int argc, char **argv) {
  (void)argc;
  (void)argv;
  int t = (int)(heap_total() / 1024);
  int u = (int)(heap_used() / 1024);
  int f = t - u;
  int p = t > 0 ? (u * 100) / t : 0;
  vga_puts("Filesystem  KB-total  KB-used  KB-free  Use%  Mount\n");
  vga_printf("ramfs       %d      %d     %d    %d%%   /\n", t, u, f, p);
}

void cmd_ps(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts(" id  state  ticks  name\n");
  task_dump(vga_puts);
}

void cmd_mouse(int argc, char **argv) {
  (void)argc;
  (void)argv;
  int x, y;
  uint8_t b;
  mouse_get(&x, &y, &b);
  vga_printf("mouse x=%d y=%d btn=0x%x\n", x, y, b);
}

void cmd_uname(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "-a"))
    vga_puts("SamaraOS 0.5 i686 samara graphics\n");
  else
    vga_puts("SamaraOS\n");
}

void cmd_whoami(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts("user\n");
}

void cmd_about(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_set_color(VGA_LCYAN, VGA_BLACK);
  vga_puts("SamaraOS 0.5\n");
  vga_set_color(VGA_LGREY, VGA_BLACK);
  vga_puts(
      "Hobby OS in C: multitasking, fs, PS/2, VBE/13h graphics, terminal\n");
  vga_puts("Type 'help' for the full command list.\n");
}

void cmd_history(int argc, char **argv) {
  (void)argc;
  (void)argv;
  for (int i = hist_count - 1; i >= 0; i--) {
    vga_printf(" %d  %s\n", hist_count - i, history[i]);
  }
}

void cmd_exit(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (g_in_wm_terminal && g_current_term_window &&
      g_current_term_window->open) {
    wm_close(g_current_term_window);
    g_in_wm_terminal = false;
  } else {
    shell_request_exit = 1;
  }
}

void cmd_sleep(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("sleep: ms\n");
    return;
  }
  int ms = atoi(argv[1]);
  uint32_t start = pit_uptime_ms();
  while ((int)(pit_uptime_ms() - start) < ms)
    __asm__ volatile("hlt");
}

void cmd_shutdown(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts("shutdown...\n");
  outw(0x604, 0x2000);  /* QEMU >= 2.0 */
  outw(0xB004, 0x2000); /* older QEMU/Bochs */
  outw(0x4004, 0x3400); /* virtualbox */
  __asm__ volatile("cli; hlt");
}

void cmd_reboot(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts("rebooting...\n");
  while (inb(0x64) & 0x02) {
  }
  outb(0x64, 0xFE);
  __asm__ volatile("cli; hlt");
}

void cmd_beep(int argc, char **argv) {
  int freq = 1000;
  if (argc > 1)
    freq = atoi(argv[1]);
  if (freq < 50)
    freq = 50;
  if (freq > 10000)
    freq = 10000;
  uint32_t div = 1193180U / (uint32_t)freq;
  outb(0x43, 0xB6);
  outb(0x42, (uint8_t)(div & 0xFF));
  outb(0x42, (uint8_t)((div >> 8) & 0xFF));
  uint8_t tmp = inb(0x61);
  outb(0x61, tmp | 3);
  uint32_t start = pit_uptime_ms();
  while (pit_uptime_ms() - start < 200)
    __asm__ volatile("hlt");
  outb(0x61, tmp & 0xFC);
}

/* ----- date via CMOS RTC ----- */
static uint8_t bcd2bin(uint8_t v) {
  return (uint8_t)((v >> 4) * 10 + (v & 0xF));
}

void cmd_date(int argc, char **argv) {
  (void)argc;
  (void)argv;
  /* wait for not-updating */
  do {
    outb(0x70, 0x0A);
  } while (inb(0x71) & 0x80);

  outb(0x70, 0x00);
  uint8_t s = inb(0x71);
  outb(0x70, 0x02);
  uint8_t m = inb(0x71);
  outb(0x70, 0x04);
  uint8_t h = inb(0x71);
  outb(0x70, 0x07);
  uint8_t d = inb(0x71);
  outb(0x70, 0x08);
  uint8_t mo = inb(0x71);
  outb(0x70, 0x09);
  uint8_t y = inb(0x71);
  outb(0x70, 0x0B);
  uint8_t b = inb(0x71);

  if (!(b & 0x04)) {
    s = bcd2bin(s);
    m = bcd2bin(m);
    h = bcd2bin(h);
    d = bcd2bin(d);
    mo = bcd2bin(mo);
    y = bcd2bin(y);
  }

  char buf[32];
  char tmp[8];
  int p = 0;
#define APPEND2(v)                                                            \
  do {                                                                        \
    if ((v) < 10)                                                             \
      buf[p++] = '0';                                                         \
    itoa((v), tmp, 10);                                                       \
    for (int i = 0; tmp[i]; i++)                                              \
      buf[p++] = tmp[i];                                                      \
  } while (0)
  buf[p++] = '2';
  buf[p++] = '0';
  APPEND2(y);
  buf[p++] = '-';
  APPEND2(mo);
  buf[p++] = '-';
  APPEND2(d);
  buf[p++] = ' ';
  APPEND2(h);
  buf[p++] = ':';
  APPEND2(m);
  buf[p++] = ':';
  APPEND2(s);
  buf[p++] = '\n';
  buf[p] = 0;
#undef APPEND2
  vga_puts(buf);
}

/* ----- calc: simple recursive descent + - * / % ----- */
static const char *calc_p;
static int calc_expr(void);
static void skip_ws(void) {
  while (*calc_p == ' ' || *calc_p == '\t')
    calc_p++;
}
static int calc_factor(void) {
  skip_ws();
  if (*calc_p == '(') {
    calc_p++;
    int v = calc_expr();
    skip_ws();
    if (*calc_p == ')')
      calc_p++;
    return v;
  }
  int sign = 1;
  if (*calc_p == '-') {
    sign = -1;
    calc_p++;
  } else if (*calc_p == '+')
    calc_p++;
  int v = 0;
  if (*calc_p < '0' || *calc_p > '9')
    return 0;
  while (*calc_p >= '0' && *calc_p <= '9') {
    v = v * 10 + (*calc_p - '0');
    calc_p++;
  }
  return sign * v;
}
static int calc_term(void) {
  int v = calc_factor();
  while (1) {
    skip_ws();
    char op = *calc_p;
    if (op != '*' && op != '/' && op != '%')
      break;
    calc_p++;
    int r = calc_factor();
    if (op == '*')
      v *= r;
    else if (op == '/')
      v = r ? v / r : 0;
    else
      v = r ? v % r : 0;
  }
  return v;
}
static int calc_expr(void) {
  int v = calc_term();
  while (1) {
    skip_ws();
    char op = *calc_p;
    if (op != '+' && op != '-')
      break;
    calc_p++;
    int r = calc_term();
    if (op == '+')
      v += r;
    else
      v -= r;
  }
  return v;
}
void cmd_calc(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("calc: usage: calc <expr>\n");
    return;
  }
  char buf[256] = "";
  for (int i = 1; i < argc; i++) {
    if (strlen(buf) + strlen(argv[i]) + 1 >= sizeof(buf))
      break;
    strcat(buf, argv[i]);
    if (i < argc - 1)
      strcat(buf, " ");
  }
  calc_p = buf;
  int r = calc_expr();
  vga_printf("= %d\n", r);
}

/* ===================== neofetch ===================== */

/* Pixel-art rocket (Samara builds the Soyuz). Two pixel rows per text row,
   drawn with CP866 half blocks so each cell carries a top and bottom colour. */
#define NF_LOGO_W 13
#define NF_PIX_H 18
#define NF_LOGO_H (NF_PIX_H / 2)

static const char *nf_rocket[NF_PIX_H] = {
    "......R......",
    ".....RRR.....",
    ".....RRR.....",
    "....WWWWW....",
    "....WWWWW....",
    "....WWbWW....",
    "....WbBbW....",
    "....WWbWW....",
    "....WWWWW....",
    "....LWWWL....",
    "...RLWWWLR...",
    "..RRLWWWLRR..",
    ".RRRLLLLLRRR.",
    ".RR..DDD..RR.",
    ".....YYY.....",
    ".....YOY.....",
    "......Y......",
    "......O......",
};

static uint8_t nf_pal(char c) {
  switch (c) {
  case 'W': return VGA_WHITE;
  case 'L': return VGA_LGREY;
  case 'D': return VGA_DGREY;
  case 'R': return VGA_RED;
  case 'b': return VGA_BLUE;
  case 'B': return VGA_LBLUE;
  case 'Y': return VGA_YELLOW;
  case 'O': return VGA_BROWN;
  default:  return VGA_BLACK;
  }
}

/* VGA text mode treats bright background colours as blink, so the darker
   colour of a two-colour cell always goes to the background. */
static void nf_logo_row(int row) {
  for (int x = 0; x < NF_LOGO_W; x++) {
    uint8_t t = nf_pal(nf_rocket[row * 2][x]);
    uint8_t b = nf_pal(nf_rocket[row * 2 + 1][x]);
    char ch;
    uint8_t fg, bg = VGA_BLACK;
    if (t == VGA_BLACK && b == VGA_BLACK) { ch = ' '; fg = VGA_LGREY; }
    else if (t == b)                      { ch = '\xDB'; fg = t; }
    else if (b == VGA_BLACK)              { ch = '\xDF'; fg = t; }
    else if (t == VGA_BLACK)              { ch = '\xDC'; fg = b; }
    else if (t < 8 || g_in_wm_terminal)   { ch = '\xDC'; fg = b; bg = t; }
    else                                  { ch = '\xDF'; fg = t; bg = b & 7; }
    vga_set_color(fg, bg);
    vga_putc(ch);
  }
  vga_set_color(VGA_LGREY, VGA_BLACK);
}

static void nf_cpuid(uint32_t leaf, uint32_t r[4]) {
  __asm__ volatile("cpuid"
                   : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                   : "a"(leaf), "c"(0));
}

/* CPU brand string, whitespace-collapsed; falls back to the vendor id. */
static void nf_cpu_name(char *out, int cap) {
  char raw[49];
  uint32_t r[4];
  nf_cpuid(0x80000000, r);
  if (r[0] >= 0x80000004) {
    for (uint32_t leaf = 0; leaf < 3; leaf++) {
      nf_cpuid(0x80000002 + leaf, r);
      memcpy(raw + leaf * 16, r, 16);
    }
    raw[48] = 0;
  } else {
    nf_cpuid(0, r);
    memcpy(raw, &r[1], 4);
    memcpy(raw + 4, &r[3], 4);
    memcpy(raw + 8, &r[2], 4);
    raw[12] = 0;
  }
  int n = 0;
  bool space = true;
  for (int i = 0; raw[i] && n < cap - 1; i++) {
    if (raw[i] == ' ') {
      if (!space)
        out[n++] = ' ';
      space = true;
    } else {
      out[n++] = raw[i];
      space = false;
    }
  }
  if (n && out[n - 1] == ' ')
    n--;
  out[n] = 0;
}

static void nf_append(char *buf, uint32_t v, const char *unit) {
  char tmp[16];
  utoa(v, tmp, 10);
  strcat(buf, tmp);
  strcat(buf, unit);
}

static void nf_uptime(char *buf) {
  uint32_t s = pit_uptime_ms() / 1000;
  buf[0] = 0;
  if (s >= 86400)
    nf_append(buf, s / 86400, "d ");
  if (s >= 3600)
    nf_append(buf, s / 3600 % 24, "h ");
  if (s >= 60)
    nf_append(buf, s / 60 % 60, "m ");
  nf_append(buf, s % 60, "s");
}

/* bytes -> "12.3 MiB" */
static void nf_mib(char *buf, uint32_t bytes) {
  uint32_t tenths = (uint32_t)(((uint64_t)bytes * 10) >> 20);
  char tmp[16];
  utoa(tenths / 10, tmp, 10);
  strcat(buf, tmp);
  strcat(buf, ".");
  utoa(tenths % 10, tmp, 10);
  strcat(buf, tmp);
  strcat(buf, " MiB");
}

static void nf_field(const char *label, const char *value) {
  vga_set_color(VGA_YELLOW, VGA_BLACK);
  vga_puts(label);
  vga_set_color(VGA_DGREY, VGA_BLACK);
  vga_puts(": ");
  vga_set_color(VGA_LGREY, VGA_BLACK);
  vga_puts(value);
}

static void nf_info_line(int i) {
  char v[80];
  v[0] = 0;
  switch (i) {
  case 0:
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("user");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    vga_putc('@');
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_puts("samara");
    return;
  case 1:
    vga_set_color(VGA_DGREY, VGA_BLACK);
    vga_puts("-----------");
    return;
  case 2:
    nf_field("OS", "SamaraOS 0.5 i686");
    return;
  case 3:
    nf_field("Kernel", "samara 0.5 (multiboot)");
    return;
  case 4:
    nf_uptime(v);
    nf_field("Uptime", v);
    return;
  case 5:
    nf_field("Shell", "samara-sh");
    return;
  case 6:
    if (g_in_wm_terminal) {
      nf_append(v, (uint32_t)gfx_w(), "x");
      nf_append(v, (uint32_t)gfx_h(), " @ 60 Hz");
      nf_field("Display", v);
    } else {
      nf_field("Display", "VGA text 80x25");
    }
    return;
  case 7:
    nf_field("Terminal", g_in_wm_terminal ? "samara-term" : "vga console");
    return;
  case 8:
    nf_cpu_name(v, 60);
    nf_field("CPU", v);
    return;
  case 9: {
    uint32_t used = (uint32_t)heap_used(), total = (uint32_t)heap_total();
    nf_mib(v, used);
    strcat(v, " / ");
    nf_mib(v, total);
    strcat(v, " (");
    nf_append(v, total ? (uint32_t)((uint64_t)used * 100 / total) : 0, "%)");
    nf_field("Memory", v);
    return;
  }
  case 10:
    nf_append(v, (uint32_t)task_count(), "");
    nf_field("Tasks", v);
    return;
  case 11:
    if (net_ready()) {
      uint32_t ip = net_ip();
      for (int k = 0; k < 4; k++)
        nf_append(v, (ip >> (24 - k * 8)) & 0xFF, k < 3 ? "." : "");
      nf_field("Network", v);
    } else {
      nf_field("Network", "down");
    }
    return;
  case 12:
    nf_field("Keyboard", kbd_is_ru() ? "ru (Alt+Shift)" : "en (Alt+Shift)");
    return;
  case 14:
  case 15:
    for (int c = 0; c < 8; c++) {
      vga_set_color((uint8_t)((i - 14) * 8 + c), VGA_BLACK);
      vga_puts("\xDB\xDB\xDB");
    }
    return;
  }
}

#define NF_LINES 16

void cmd_neofetch(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_putc('\n');
  for (int i = 0; i < NF_LINES; i++) {
    vga_puts(" ");
    if (i < NF_LOGO_H) {
      nf_logo_row(i);
    } else {
      for (int k = 0; k < NF_LOGO_W; k++)
        vga_putc(' ');
    }
    vga_puts("   ");
    nf_info_line(i);
    vga_putc('\n');
  }
  vga_set_color(VGA_LGREY, VGA_BLACK);
}
