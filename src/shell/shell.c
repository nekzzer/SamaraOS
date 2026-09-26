#include "shell.h"
#include "commands.h"
#include "shell_priv.h"
#include "complete.h"
#include "drivers/mouse.h"
#include "gui/desktop.h"
#include "proc/tty.h"

#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "fs/fs.h"
#include "gfx/gfx.h"
#include "gfx/gfx_term.h"
#include "gui/wm.h"
#include "proc/proc.h"

fs_node_t *cwd;
int shell_request_exit = 0;
static int shell_exit_on_esc = 0; /* if true, Esc inside readline exits shell */

/* WM-terminal state (declared in shell_priv.h; shell.c owns the storage) */
bool g_in_wm_terminal;
window_t *g_current_term_window;

/* ===================== readline with cursor + history ===================== */

char history[HIST_MAX][LINE_MAX];
int hist_count = 0;

static void hist_push(const char *line) {
  if (!line[0])
    return;
  if (hist_count > 0 && !strcmp(history[0], line))
    return;
  int n = hist_count < HIST_MAX ? hist_count + 1 : HIST_MAX;
  for (int i = n - 1; i > 0; i--)
    memcpy(history[i], history[i - 1], LINE_MAX);
  strncpy(history[0], line, LINE_MAX - 1);
  history[0][LINE_MAX - 1] = 0;
  if (hist_count < HIST_MAX)
    hist_count++;
}

/* ---- colour gradients (true colour in the desktop terminal, nearest of
   the 16 VGA colours on the text screen) ---- */

static uint32_t mix_rgb(uint32_t a, uint32_t b, int t, int n) {   /* t/n of the way a->b */
  if (n <= 0)
    return a;
  uint32_t r = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int ca = (int)((a >> sh) & 255), cb = (int)((b >> sh) & 255);
    r |= (uint32_t)(ca + (cb - ca) * t / n) << sh;
  }
  return r;
}

/* colour of position i of n along stops[0..k-1] */
static uint32_t grad_at(const uint32_t *stops, int k, int i, int n) {
  if (n <= 1 || k == 1)
    return stops[0];
  int span = (n - 1) * (k - 1);
  int pos = i * (k - 1) * 1000 / (n - 1);                /* 0..(k-1)*1000 */
  int seg = pos / 1000;
  if (seg >= k - 1)
    return stops[k - 1];
  (void)span;
  return mix_rgb(stops[seg], stops[seg + 1], pos % 1000, 1000);
}

static uint32_t term_bg(void) { return gfx_term_color(0); }

static void grad_puts(const char *s, const uint32_t *stops, int k) {
  int n = (int)strlen(s);
  for (int i = 0; i < n; i++) {
    vga_set_rgb(grad_at(stops, k, i, n), term_bg());
    vga_putc(s[i]);
  }
  vga_set_color(VGA_LGREY, VGA_BLACK);
}

static const uint32_t G_SUNSET[] = {0xFF6A88, 0xFF9A5A, 0xFFD36E};   /* pink -> orange -> gold */
static const uint32_t G_SEA[] = {0x6EE7D2, 0x8FB8F0, 0xB79CF2};      /* aqua -> sky -> lavender */

/* Block-letter "SAMARA OS" (CP866 half blocks), a horizontal gradient. */
static void banner(void) {
  static const char *const logo[] = {
      "\xdc\xdf\xdf\xdf \xdc\xdf\xdf\xdc \xdb\xdc \xdc\xdb \xdc\xdf\xdf\xdc \xdb\xdf\xdf\xdc \xdc\xdf\xdf\xdc    \xdc\xdf\xdf\xdc \xdc\xdf\xdf\xdf",
      " \xdf\xdf\xdc \xdb\xdf\xdf\xdb \xdb \xdf \xdb \xdb\xdf\xdf\xdb \xdb\xdf\xdb  \xdb\xdf\xdf\xdb    \xdb  \xdb  \xdf\xdf\xdc",
      "\xdf\xdf\xdf  \xdf  \xdf \xdf   \xdf \xdf  \xdf \xdf  \xdf \xdf  \xdf     \xdf\xdf  \xdf\xdf\xdf",
  };
  static const uint32_t stops[] = {0xFF5E8A, 0xFF8A5B, 0xFFC857, 0x9BE38B, 0x5CC8F0};
  const int w = 44;
  for (int r = 0; r < 3; r++) {
    vga_putc(' ');
    for (int i = 0; logo[r][i]; i++) {
      vga_set_rgb(grad_at(stops, 5, i, w), term_bg());
      vga_putc(logo[r][i]);
    }
    vga_putc('\n');
  }
  vga_set_color(VGA_LGREY, VGA_BLACK);
}

static void prompt(void) {
  char path[256];
  fs_path(cwd, path, sizeof(path));
  grad_puts("user@samara", G_SUNSET, 3);
  vga_set_color(VGA_DGREY, VGA_BLACK);
  vga_putc(':');
  grad_puts(path, G_SEA, 3);
  vga_set_rgb(0xFFD36E, term_bg());
  vga_puts("$ ");
  vga_set_color(VGA_LGREY, VGA_BLACK);
}

static void redraw_tail(char *buf, int len, int cur, int prompt_x,
                        int prompt_y) {
  vga_set_cursor(prompt_x + cur, prompt_y);
  for (int i = cur; i < len; i++)
    vga_putc(buf[i]);
  vga_putc(' ');
  vga_set_cursor(prompt_x + cur, prompt_y);
}

static bool font_key(char c) {
  return kbd_ctrl_held() && (c == '=' || c == '+' || c == '-');
}

/* Next key for the console shell. Meanwhile the wheel moves through the
   scrollback and Ctrl +/- change the font size (the grid is rebuilt, *py
   follows the prompt line). */
static char console_getc(int *py) {
  for (;;) {
    if (kbd_has_key()) {
      char c = kbd_trygetc();
      if (vga_is_gfx() && console_is_gfx() && font_key(c)) {
        *py -= console_font_step(c == '-' ? -1 : 1);
        if (*py < 0)
          *py = 0;
        continue;
      }
      if (gfx_term_scrolled())
        gfx_term_view_scroll(-(1 << 20));
      return c;
    }
    int dz = mouse_wheel_take();
    if (dz && vga_is_gfx())
      gfx_term_view_scroll(-dz * 3);
    extern volatile int cpu_idle;
    cpu_idle = 1;
    __asm__ volatile("sti; hlt");
    cpu_idle = 0;
  }
}

static void readline(char *buf, int cap) {
  int len = 0, cur = 0;
  int hist_idx = 0;
  int prompt_x, prompt_y;
  vga_get_cursor(&prompt_x, &prompt_y);
  buf[0] = 0;

  while (1) {
    char c = console_getc(&prompt_y);

    if (c == '\t') {
      int from;
      int r = shell_complete(buf, &len, &cur, cap, &from);
      if (r == SC_EDIT) {
        vga_set_cursor(prompt_x + from, prompt_y);
        for (int i = from; i < len; i++)
          vga_putc(buf[i]);
        vga_set_cursor(prompt_x + cur, prompt_y);
      } else if (r == SC_LIST) {
        vga_set_cursor(prompt_x + len, prompt_y);
        vga_putc('\n');
        shell_complete_print_list();
        prompt();
        vga_get_cursor(&prompt_x, &prompt_y);
        vga_puts(buf);
        vga_set_cursor(prompt_x + cur, prompt_y);
      }
      continue;
    }
    shell_complete_reset();

    if (c == '\n') {
      buf[len] = 0;
      vga_set_cursor(prompt_x + len, prompt_y);
      vga_putc('\n');
      hist_push(buf);
      return;
    }
    if (c == 0x1B) { /* Esc */
      if (shell_exit_on_esc) {
        shell_request_exit = 1;
        buf[0] = 0;
        vga_putc('\n');
        return;
      }
      continue;
    }
    if (c == '\b') {
      if (cur > 0) {
        memmove(buf + cur - 1, buf + cur, len - cur);
        len--;
        cur--;
        buf[len] = 0;
        redraw_tail(buf, len, cur, prompt_x, prompt_y);
      }
      continue;
    }
    if (c == (char)K_DEL) {
      if (cur < len) {
        memmove(buf + cur, buf + cur + 1, len - cur);
        len--;
        buf[len] = 0;
        redraw_tail(buf, len, cur, prompt_x, prompt_y);
      }
      continue;
    }
    if (c == (char)K_LEFT) {
      if (cur > 0) {
        cur--;
        vga_set_cursor(prompt_x + cur, prompt_y);
      }
      continue;
    }
    if (c == (char)K_RIGHT) {
      if (cur < len) {
        cur++;
        vga_set_cursor(prompt_x + cur, prompt_y);
      }
      continue;
    }
    if (c == (char)K_HOME) {
      cur = 0;
      vga_set_cursor(prompt_x, prompt_y);
      continue;
    }
    if (c == (char)K_END) {
      cur = len;
      vga_set_cursor(prompt_x + cur, prompt_y);
      continue;
    }
    if (c == (char)K_UP) {
      if (hist_idx < hist_count) {
        vga_set_cursor(prompt_x, prompt_y);
        for (int i = 0; i < len; i++)
          vga_putc(' ');
        hist_idx++;
        strncpy(buf, history[hist_idx - 1], cap - 1);
        buf[cap - 1] = 0;
        len = strlen(buf);
        cur = len;
        vga_set_cursor(prompt_x, prompt_y);
        vga_puts(buf);
      }
      continue;
    }
    if (c == (char)K_DOWN) {
      if (hist_idx > 0) {
        vga_set_cursor(prompt_x, prompt_y);
        for (int i = 0; i < len; i++)
          vga_putc(' ');
        hist_idx--;
        if (hist_idx == 0) {
          buf[0] = 0;
          len = 0;
        } else {
          strncpy(buf, history[hist_idx - 1], cap - 1);
          buf[cap - 1] = 0;
          len = strlen(buf);
        }
        cur = len;
        vga_set_cursor(prompt_x, prompt_y);
        vga_puts(buf);
      }
      continue;
    }
    if (c == 3) { /* Ctrl+C */
      vga_putc('\n');
      buf[0] = 0;
      return;
    }
    if (c == 12) { /* Ctrl+L */
      vga_clear();
      prompt();
      vga_get_cursor(&prompt_x, &prompt_y);
      vga_puts(buf);
      vga_set_cursor(prompt_x + cur, prompt_y);
      continue;
    }
    if (c == 4) { /* Ctrl+D = exit */
      shell_request_exit = 1;
      buf[0] = 0;
      vga_putc('\n');
      return;
    }
    if (c >= 0x20 && c < 0x7F && len < cap - 1) {
      if (cur < len)
        memmove(buf + cur + 1, buf + cur, len - cur);
      buf[cur] = c;
      len++;
      cur++;
      buf[len] = 0;
      vga_set_cursor(prompt_x + cur - 1, prompt_y);
      for (int i = cur - 1; i < len; i++)
        vga_putc(buf[i]);
      vga_set_cursor(prompt_x + cur, prompt_y);
    }
  }
}

static int split(char *line, char **argv, int max) {
  int n = 0;
  char *p = line;
  while (*p && n < max) {
    while (*p == ' ' || *p == '\t')
      *p++ = 0;
    if (!*p)
      break;
    argv[n++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
  }
  return n;
}

/* ===================== forward decl ===================== */
static void execute(char *line);

/* ===================== terminal-as-window (used by WM) =====================
 */

static char term_buf[LINE_MAX];
static int term_len = 0, term_cur = 0, term_hist_idx = 0;
static int term_prompt_x = 0, term_prompt_y = 0;
/* g_in_wm_terminal and g_current_term_window declared near top of file */

static void term_redraw_tail(void) {
  vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
  for (int i = term_cur; i < term_len; i++)
    vga_putc(term_buf[i]);
  vga_putc(' ');
  vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
}

static void term_show_prompt(void) {
  prompt();
  vga_get_cursor(&term_prompt_x, &term_prompt_y);
  term_buf[0] = 0;
  term_len = 0;
  term_cur = 0;
  term_hist_idx = 0;
}

void wm_terminal_init(window_t *w) {
  vga_use_gfx_term(w->term_x, w->term_y);
  gfx_term_show_cursor(false);        /* the desktop draws its own caret */
  vga_clear();
  vga_putc('\n');
  banner();
  vga_set_color(VGA_DGREY, VGA_BLACK);
  vga_puts("\n 'help' for commands  \xfa  Tab completes  \xfa  wheel scrolls back  \xfa  F11 fullscreen\n\n");
  vga_set_color(VGA_LGREY, VGA_BLACK);
  g_in_wm_terminal = true;
  g_current_term_window = w;
  term_show_prompt();
}

/* The window's grid changed size: follow the prompt if the content moved
   up, and tell the foreground program (SIGWINCH). */
void wm_terminal_resized(window_t *w, int cols, int rows) {
  (void)w;
  if (cols > TERM_MAX_COLS)
    cols = TERM_MAX_COLS;
  if (rows > TERM_MAX_ROWS)
    rows = TERM_MAX_ROWS;
  if (cols == gfx_term_cols() && rows == gfx_term_rows())
    return;
  int shift = gfx_term_resize(cols, rows);
  term_prompt_y -= shift;
  if (term_prompt_y < 0)
    term_prompt_y = 0;
  tty_resized();
}

/* Mouse wheel over the terminal: full-screen programs (nano, less, vi)
   get scroll keys, otherwise move through the scrollback. */
void wm_terminal_wheel(window_t *w, int dz) {
  (void)w;
  if (shell_fg_running() && !gfx_term_scrolled() && tty_wheel(dz))
    return;
  gfx_term_view_scroll(-dz * 3);
}

void wm_terminal_handle_key(window_t *w, char c) {
  g_current_term_window = w;
  if (gfx_term_scrolled())
    gfx_term_view_scroll(-(1 << 20));   /* typing returns to the live screen */

  if (shell_fg_running()) {         /* keys belong to the running program */
    shell_fg_key(c);
    return;
  }

  if (c == '\t') {
    int from;
    int r = shell_complete(term_buf, &term_len, &term_cur, LINE_MAX, &from);
    if (r == SC_EDIT) {
      vga_set_cursor(term_prompt_x + from, term_prompt_y);
      for (int i = from; i < term_len; i++)
        vga_putc(term_buf[i]);
      vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    } else if (r == SC_LIST) {
      vga_set_cursor(term_prompt_x + term_len, term_prompt_y);
      vga_putc('\n');
      shell_complete_print_list();
      prompt();
      vga_get_cursor(&term_prompt_x, &term_prompt_y);
      vga_puts(term_buf);
      vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    }
    return;
  }
  shell_complete_reset();

  if (c == '\n') {
    term_buf[term_len] = 0;
    vga_set_cursor(term_prompt_x + term_len, term_prompt_y);
    vga_putc('\n');
    hist_push(term_buf);
    char copy[LINE_MAX];
    strncpy(copy, term_buf, LINE_MAX - 1);
    copy[LINE_MAX - 1] = 0;
    execute(copy);
    if (!w->open) {
      g_in_wm_terminal = false;
      return;
    }
    if (!shell_fg_running())
      term_show_prompt();
    return;
  }
  if (c == '\b') {
    if (term_cur > 0) {
      memmove(term_buf + term_cur - 1, term_buf + term_cur,
              term_len - term_cur);
      term_len--;
      term_cur--;
      term_buf[term_len] = 0;
      term_redraw_tail();
    }
    return;
  }
  if (c == (char)K_DEL) {
    if (term_cur < term_len) {
      memmove(term_buf + term_cur, term_buf + term_cur + 1,
              term_len - term_cur);
      term_len--;
      term_buf[term_len] = 0;
      term_redraw_tail();
    }
    return;
  }
  if (c == (char)K_LEFT) {
    if (term_cur > 0) {
      term_cur--;
      vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    }
    return;
  }
  if (c == (char)K_RIGHT) {
    if (term_cur < term_len) {
      term_cur++;
      vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    }
    return;
  }
  if (c == (char)K_HOME) {
    term_cur = 0;
    vga_set_cursor(term_prompt_x, term_prompt_y);
    return;
  }
  if (c == (char)K_END) {
    term_cur = term_len;
    vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    return;
  }
  if (c == (char)K_UP) {
    if (term_hist_idx < hist_count) {
      vga_set_cursor(term_prompt_x, term_prompt_y);
      for (int i = 0; i < term_len; i++)
        vga_putc(' ');
      term_hist_idx++;
      strncpy(term_buf, history[term_hist_idx - 1], LINE_MAX - 1);
      term_buf[LINE_MAX - 1] = 0;
      term_len = strlen(term_buf);
      term_cur = term_len;
      vga_set_cursor(term_prompt_x, term_prompt_y);
      vga_puts(term_buf);
    }
    return;
  }
  if (c == (char)K_DOWN) {
    if (term_hist_idx > 0) {
      vga_set_cursor(term_prompt_x, term_prompt_y);
      for (int i = 0; i < term_len; i++)
        vga_putc(' ');
      term_hist_idx--;
      if (term_hist_idx == 0) {
        term_buf[0] = 0;
        term_len = 0;
      } else {
        strncpy(term_buf, history[term_hist_idx - 1], LINE_MAX - 1);
        term_buf[LINE_MAX - 1] = 0;
        term_len = strlen(term_buf);
      }
      term_cur = term_len;
      vga_set_cursor(term_prompt_x, term_prompt_y);
      vga_puts(term_buf);
    }
    return;
  }
  if (c == 3) {
    vga_putc('\n');
    term_show_prompt();
    return;
  } /* ^C */
  if (c == 4) {
    wm_close(w);
    g_in_wm_terminal = false;
    return;
  } /* ^D */
  if (c == 12) {
    vga_clear();
    term_show_prompt();
    return;
  } /* ^L */
  if (c >= 0x20 && c < 0x7F && term_len < LINE_MAX - 1) {
    if (term_cur < term_len)
      memmove(term_buf + term_cur + 1, term_buf + term_cur,
              term_len - term_cur);
    term_buf[term_cur] = c;
    term_len++;
    term_cur++;
    term_buf[term_len] = 0;
    vga_set_cursor(term_prompt_x + term_cur - 1, term_prompt_y);
    for (int i = term_cur - 1; i < term_len; i++)
      vga_putc(term_buf[i]);
    vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
  }
}

/* Types `line` at the prompt (replacing any half-typed input) and runs it,
   as if the user had entered it. Used by the desktop icons. */
bool wm_terminal_submit(window_t *w, const char *line) {
  if (shell_fg_running())
    return false;
  g_current_term_window = w;
  vga_set_cursor(term_prompt_x, term_prompt_y);
  for (int i = 0; i < term_len; i++)
    vga_putc(' ');
  vga_set_cursor(term_prompt_x, term_prompt_y);
  term_len = term_cur = 0;
  term_buf[0] = 0;
  for (const char *p = line; *p; p++)
    wm_terminal_handle_key(w, *p);
  wm_terminal_handle_key(w, '\n');
  return true;
}

/* Per-frame: feed a running program's output to the terminal and bring the
   prompt back once it exits. */
void wm_terminal_poll(window_t *w) {
  if (!shell_fg_running())
    return;
  g_current_term_window = w;
  if (shell_fg_poll() && w->open)
    term_show_prompt();
}

bool wm_terminal_busy(void) { return shell_fg_running(); }

void wm_terminal_closed(void) { shell_fg_abandon(); }

const char *wm_sysinfo_text(void) {
  static char buf[1024];
  int p = 0;
  char tmp[24];
#define ADD(s)                                                                \
  do {                                                                        \
    for (const char *_q = (s); *_q; _q++)                                     \
      buf[p++] = *_q;                                                         \
  } while (0)
  ADD("System Information\n\n");
  ADD("OS:      SamaraOS 0.5\n");
  ADD("Kernel:  samara-kernel 0.5\n");
  ADD("CPU:     i686\n");
  ADD("Display: ");
  itoa(gfx_w(), tmp, 10);
  ADD(tmp);
  ADD(" x ");
  itoa(gfx_h(), tmp, 10);
  ADD(tmp);
  ADD(" 32bpp\n");
  ADD("Uptime:  ");
  utoa(pit_uptime_ms() / 1000, tmp, 10);
  ADD(tmp);
  ADD(" s\n");
  ADD("Heap:    ");
  utoa((uint32_t)heap_used(), tmp, 10);
  ADD(tmp);
  ADD(" / ");
  utoa((uint32_t)heap_total(), tmp, 10);
  ADD(tmp);
  ADD(" B\n");
  ADD("Tasks:   ");
  itoa(task_count(), tmp, 10);
  ADD(tmp);
  ADD("\n");
  buf[p] = 0;
#undef ADD
  return buf;
}

/* ===================== dispatch ===================== */

static cmd_t cmds[] = {{"help", cmd_help},
                       {"clear", cmd_clear},
                       {"echo", cmd_echo},
                       {"pwd", cmd_pwd},
                       {"ls", cmd_ls},
                       {"cd", cmd_cd},
                       {"cat", cmd_cat},
                       {"mkdir", cmd_mkdir},
                       {"touch", cmd_touch},
                       {"rm", cmd_rm},
                       {"cp", cmd_cp},
                       {"mv", cmd_mv},
                       {"head", cmd_head},
                       {"tail", cmd_tail},
                       {"wc", cmd_wc},
                       {"grep", cmd_grep},
                       {"find", cmd_find},
                       {"uptime", cmd_uptime},
                       {"mem", cmd_mem},
                       {"df", cmd_df},
                       {"ps", cmd_ps},
                       {"mouse", cmd_mouse},
                       {"uname", cmd_uname},
                       {"whoami", cmd_whoami},
                       {"about", cmd_about},
                       {"date", cmd_date},
                       {"history", cmd_history},
                       {"exit", cmd_exit},
                       {"sleep", cmd_sleep},
                       {"shutdown", cmd_shutdown},
                       {"reboot", cmd_reboot},
                       {"beep", cmd_beep},
                       {"calc", cmd_calc},
                       {"nano", cmd_gnu_nano},
                       {"python", cmd_python},
                       {"python3", cmd_python},
                       {"edit", cmd_nano},
                       {"neofetch", cmd_neofetch},
                       {"desktop", cmd_desktop},
                       {"wallpaper", cmd_wallpaper},
                       {"bounce", cmd_bounce},
                       {"disk", cmd_disk},
                       {"doom", cmd_doom},
                       {"doom2", cmd_doom},
                       {"doom-mini", cmd_doom_mini},
                       {"play", cmd_play},
                       {"snake", cmd_snake},
                       {"ifconfig", cmd_ifconfig},
                       {"ping", cmd_ping},
                       {"wget", cmd_wget},
                       {"browser", cmd_browser},
                       {"www", cmd_browser},
                       {"klayout", cmd_klayout},
                       {"player", cmd_player},
                       {"music", cmd_player},
                       {"paint", cmd_paint},
                       {"clock", cmd_clock},
                       {"playwav", cmd_playwav},
                       {"stopwav", cmd_stopwav},
                       {"sbinfo", cmd_sbinfo},
                       {"fatmount", cmd_fatmount},
                       {"fatls", cmd_fatls},
                       {"fatload", cmd_fatload},
                       {"playfat", cmd_playfat},
                       {"sh", cmd_sh},
                       {"run", cmd_run},
                       {"strace", cmd_strace},
                       {"procs", cmd_procs},
                       {"disks", cmd_disks},
                       {"kbdignore", cmd_kbdignore},
                       {NULL, NULL}};

const char *shell_builtin_name(int i) {
  return i >= 0 && i < (int)(sizeof(cmds) / sizeof(cmds[0])) ? cmds[i].name : NULL;
}

static void execute(char *line) {
  char *argv[16];
  int argc = split(line, argv, 16);
  if (!argc)
    return;
  /* trailing `&` (or `prog&`): run the program in the background */
  bool bg = false;
  if (!strcmp(argv[argc - 1], "&")) {
    bg = true;
    argv[--argc] = NULL;
  } else {
    size_t n = strlen(argv[argc - 1]);
    if (n > 1 && argv[argc - 1][n - 1] == '&') {
      bg = true;
      argv[argc - 1][n - 1] = 0;
    }
  }
  if (!argc)
    return;
  procfs_refresh();                 /* builtins like `cat /proc/meminfo` */
  for (cmd_t *c = cmds; c->name; c++) {
    if (!strcmp(c->name, argv[0])) {
      c->fn(argc, argv);
      return;
    }
  }
  /* Not a builtin: try a ring-3 program from /bin, /usr/bin, ... */
  char path[128];
  if (shell_find_program(argv[0], path, sizeof(path))) {
    if (bg) {
      int pid = shell_spawn_background(path, argc, argv);
      if (pid > 0)
        vga_printf("[%d] %s\n", pid, argv[0]);
    } else {
      shell_exec_program(path, argc, argv);
    }
    return;
  }
  vga_puts(argv[0]);
  vga_puts(": command not found (try 'help')\n");
}

void shell_run_line(const char *line) {
  char copy[LINE_MAX];
  strncpy(copy, line, LINE_MAX - 1);
  copy[LINE_MAX - 1] = 0;
  execute(copy);
}

void shell_run(void) {
  cwd = fs_resolve(fs_root(), "/home/user");
  if (!cwd)
    cwd = fs_root();

  vga_putc('\n');
  banner();
  vga_set_color(VGA_DGREY, VGA_BLACK);
  vga_puts("\n Type 'help' or 'desktop'.\n\n");
  vga_set_color(VGA_LGREY, VGA_BLACK);

  char line[LINE_MAX];
  while (!shell_request_exit) {
    prompt();
    readline(line, LINE_MAX);
    if (shell_request_exit)
      break;
    execute(line);
  }
  shell_request_exit = 0;
}
