#include "../shell_priv.h"
#include "../commands.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"
#include "drivers/keyboard.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"
#include "drivers/vga.h"
#include "fs/fs.h"
#include "proc/proc.h"
#include "proc/tty.h"

/* Running ring-3 programs (busybox et al.) from the SamaraOS shell.

   In text mode the shell blocks here, shuttling keys into the tty and tty
   output onto the screen until the program exits. In the desktop terminal
   the window manager keeps running: the program becomes the terminal's
   foreground job and wm_terminal_poll() does the same shuttling per frame. */

static char *const user_env[] = {
    "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/games",
    "HOME=/home/user",
    "USER=root",
    "LOGNAME=root",
    "SHELL=/bin/sh",
    "TERM=linux",
    "PS1=\\u@\\h:\\w\\$ ",
    NULL,
};

static int fg_pid;                  /* desktop terminal's foreground job */

bool shell_find_program(const char *name, char *out, int cap) {
  if (!name[0])
    return false;
  if (strchr(name, '/')) {
    fs_node_t *n = fs_resolve(cwd, name);
    if (!n || n->type != FS_FILE)
      return false;
    strncpy(out, name, cap - 1);
    out[cap - 1] = 0;
    return true;
  }
  static const char *const path[] = {"/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/", "/usr/games/"};
  for (unsigned i = 0; i < sizeof(path) / sizeof(path[0]); i++) {
    char full[128];
    strncpy(full, path[i], sizeof(full) - 1);
    full[sizeof(full) - 1] = 0;
    if (strlen(full) + strlen(name) >= sizeof(full))
      continue;
    strcat(full, name);
    fs_node_t *n = fs_resolve(fs_root(), full);
    if (n && n->type == FS_FILE && (n->mode & 0111)) {
      strncpy(out, full, cap - 1);
      out[cap - 1] = 0;
      return true;
    }
  }
  return false;
}

static void report_status(int st) {
  int sig = st & 0x7F;
  if (!sig)
    return;
  static const char *const names[] = {
      [4] = "Illegal instruction", [6] = "Aborted", [8] = "Floating point exception",
      [9] = "Killed", [11] = "Segmentation fault", [13] = "Broken pipe",
      [15] = "Terminated"};
  const char *n = (sig < (int)(sizeof(names) / sizeof(names[0])) && names[sig]) ? names[sig] : NULL;
  if (sig == 2) {                   /* ^C already echoed by the tty */
    return;
  }
  if (n)
    vga_printf("%s\n", n);
  else
    vga_printf("Killed by signal %d\n", sig);
}

static void finish(int pid) {
  while (tty_has_output())
    tty_pump();
  int st = proc_reap(pid);
  vga_set_color(VGA_LGREY, VGA_BLACK);
  int x, y;
  vga_get_cursor(&x, &y);
  if (x != 0)
    vga_putc('\n');
  if (st >= 0)
    report_status(st);
}

int shell_exec_program(const char *path, int argc, char **argv) {
  char *args[32];
  int n = 0;
  for (int i = 0; i < argc && n < 31; i++)
    args[n++] = argv[i];
  args[n] = NULL;
  int pid = proc_spawn(path, args, user_env);
  if (pid < 0) {
    vga_printf("%s: cannot execute (error %d)\n", argv[0], -pid);
    return pid;
  }
  if (g_in_wm_terminal) {
    fg_pid = pid;
    return 0;
  }
  while (proc_alive(pid)) {
    tty_pump();
    while (kbd_has_key())
      tty_key(kbd_trygetc());
    task_yield();
  }
  finish(pid);
  return 0;
}

/* ---- desktop terminal hooks (called from shell.c / wm.c) ---- */

bool shell_fg_running(void) { return fg_pid != 0; }

void shell_fg_key(char c) { tty_key(c); }

/* Returns true when the foreground job just finished (prompt needed). */
bool shell_fg_poll(void) {
  if (!fg_pid)
    return false;
  tty_pump();
  if (proc_alive(fg_pid))
    return false;
  finish(fg_pid);
  fg_pid = 0;
  return true;
}

void shell_fg_abandon(void) {
  if (!fg_pid)
    return;
  proc_kill_all();
  proc_reap(fg_pid);
  fg_pid = 0;
}

/* ---- builtins ---- */

void cmd_sh(int argc, char **argv) {
  char *def[] = {"sh", NULL};
  if (argc < 1 || !argv) {
    argc = 1;
    argv = def;
  }
  char path[128];
  if (!shell_find_program("sh", path, sizeof(path))) {
    vga_puts("sh: busybox is not installed\n");
    return;
  }
  char *args[32];
  int n = 0;
  args[n++] = "sh";
  for (int i = 1; i < argc && n < 31; i++)
    args[n++] = argv[i];
  shell_exec_program(path, n, args);
}

void cmd_run(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("usage: run <program> [args...]\n");
    return;
  }
  char path[128];
  if (!shell_find_program(argv[1], path, sizeof(path))) {
    vga_printf("run: %s: not found\n", argv[1]);
    return;
  }
  shell_exec_program(path, argc - 1, argv + 1);
}

void cmd_strace(int argc, char **argv) {
  extern bool g_strace;
  if (argc > 1)
    g_strace = !strcmp(argv[1], "on");
  vga_printf("syscall trace to COM1: %s\n", g_strace ? "on" : "off");
}

void cmd_procs(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts("  PID  PPID  PGID  STATE   NAME\n");
  for (int i = 0; i < proc_count(); i++) {
    proc_t *p = proc_at(i);
    if (!p)
      continue;
    vga_printf("  %d    %d     %d     %s  %s\n", p->pid, p->ppid, p->pgid,
               p->state == P_ALIVE ? "run   " : "zombie", p->name);
  }
  vga_printf("user memory: %u / %u KB free\n", pmm_free_frames() * 4,
             pmm_total_frames() * 4);
}

void cmd_disks(int argc, char **argv) {
  (void)argc;
  (void)argv;
  vga_puts("  DEV   BUS   SIZE(MB)  MODEL\n");
  for (int i = 0; i < DISK_MAX; i++) {
    if (!ata_drive_present(i))
      continue;
    bool sata = i >= DISK_AHCI_BASE;
    vga_printf("  %s   %s  %u       %s\n", ata_drive_name(i), sata ? "sata" : "ide ",
               ata_drive_sectors(i) / 2048, sata ? ahci_model(i - DISK_AHCI_BASE) : "");
  }
  vga_printf("ahci: %s\n", ahci_status());
}

/* `nano` runs GNU nano from /usr/bin when installed; the built-in editor
   stays available as `edit`. */
void cmd_gnu_nano(int argc, char **argv) {
  char path[128];
  if (shell_find_program("nano", path, sizeof(path))) {
    shell_exec_program(path, argc, argv);
    return;
  }
  cmd_nano(argc, argv);
}

/* `python` / `python3` run MicroPython (/usr/bin/micropython). */
void cmd_python(int argc, char **argv) {
  char path[128];
  if (!shell_find_program("micropython", path, sizeof(path))) {
    vga_puts("python: MicroPython is not installed (userland/build-micropython.sh)\n");
    return;
  }
  argv[0] = "micropython";
  shell_exec_program(path, argc, argv);
}

void cmd_kbdignore(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("usage: kbdignore <hex scancode>... | kbdignore off\n"
             "  e.g. kbdignore 35   (the '/' key)\n");
    return;
  }
  if (!strcmp(argv[1], "off")) {
    kbd_unignore_all();
    vga_puts("all keys enabled\n");
    return;
  }
  for (int i = 1; i < argc; i++) {
    uint32_t v = 0;
    for (const char *c = argv[i]; *c; c++)
      v = v * 16 + (uint32_t)(*c >= 'a' ? *c - 'a' + 10 : *c >= 'A' ? *c - 'A' + 10 : *c - '0');
    if (v < 0x80) {
      kbd_ignore_scancode((uint8_t)v);
      vga_printf("ignoring scancode 0x%x\n", v);
    }
  }
}
