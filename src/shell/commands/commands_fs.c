#include "../shell_priv.h"
#include "core/string.h"
#include "drivers/vga.h"
#include "fs/fs.h"

void cmd_pwd(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char p[256];
  fs_path(cwd, p, sizeof(p));
  vga_puts(p);
  vga_putc('\n');
}

void cmd_ls(int argc, char **argv) {
  fs_node_t *d = (argc > 1) ? fs_resolve(cwd, argv[1]) : cwd;
  if (!d) {
    vga_puts("ls: no such path\n");
    return;
  }
  if (d->type != FS_DIR) {
    vga_puts(d->name);
    vga_putc('\n');
    return;
  }
  for (fs_node_t *c = d->child; c; c = c->next) {
    if (c->type == FS_DIR) {
      vga_set_color(VGA_LBLUE, VGA_BLACK);
      vga_puts(c->name);
      vga_putc('/');
    } else {
      vga_set_color(VGA_LGREY, VGA_BLACK);
      vga_puts(c->name);
    }
    vga_set_color(VGA_LGREY, VGA_BLACK);
    vga_puts("  ");
  }
  vga_putc('\n');
}

void cmd_cd(int argc, char **argv) {
  if (argc < 2) {
    cwd = fs_root();
    return;
  }
  fs_node_t *d = fs_resolve(cwd, argv[1]);
  if (!d) {
    vga_puts("cd: no such path\n");
    return;
  }
  if (d->type != FS_DIR) {
    vga_puts("cd: not a directory\n");
    return;
  }
  cwd = d;
}

void cmd_cat(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("cat: missing file\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[1]);
  if (!f) {
    vga_puts("cat: no such file\n");
    return;
  }
  if (f->type != FS_FILE) {
    vga_puts("cat: not a file\n");
    return;
  }
  if (f->data)
    for (size_t i = 0; i < f->size; i++)
      vga_putc(f->data[i]);
  if (f->size && f->data[f->size - 1] != '\n')
    vga_putc('\n');
}

void cmd_mkdir(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("mkdir: missing name\n");
    return;
  }
  if (!fs_create(cwd, argv[1], FS_DIR))
    vga_puts("mkdir: failed\n");
}

void cmd_touch(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("touch: missing name\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[1]);
  if (f)
    return;
  if (!fs_create(cwd, argv[1], FS_FILE))
    vga_puts("touch: failed\n");
}

void cmd_rm(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("rm: missing path\n");
    return;
  }
  if (fs_unlink(cwd, argv[1]) != 0)
    vga_puts("rm: failed\n");
}

void cmd_cp(int argc, char **argv) {
  if (argc < 3) {
    vga_puts("cp: usage: cp <src> <dst>\n");
    return;
  }
  fs_node_t *src = fs_resolve(cwd, argv[1]);
  if (!src || src->type != FS_FILE) {
    vga_puts("cp: no source file\n");
    return;
  }
  fs_node_t *dst = fs_resolve(cwd, argv[2]);
  if (!dst)
    dst = fs_create(cwd, argv[2], FS_FILE);
  if (!dst || dst->type != FS_FILE) {
    vga_puts("cp: bad destination\n");
    return;
  }
  if (src->data)
    fs_write(dst, src->data, src->size);
  else
    fs_write(dst, "", 0);
}

void cmd_mv(int argc, char **argv) {
  if (argc < 3) {
    vga_puts("mv: usage: mv <src> <dst>\n");
    return;
  }
  fs_node_t *src = fs_resolve(cwd, argv[1]);
  if (!src) {
    vga_puts("mv: no source\n");
    return;
  }
  if (src->type != FS_FILE) {
    vga_puts("mv: only files supported\n");
    return;
  }
  fs_node_t *dst = fs_create(cwd, argv[2], FS_FILE);
  if (!dst) {
    vga_puts("mv: cannot create destination\n");
    return;
  }
  if (src->data)
    fs_write(dst, src->data, src->size);
  fs_unlink(cwd, argv[1]);
}

void cmd_head(int argc, char **argv) {
  int n = 10, fi = 1;
  if (argc >= 4 && !strcmp(argv[1], "-n")) {
    n = atoi(argv[2]);
    fi = 3;
  }
  if (fi >= argc) {
    vga_puts("head: missing file\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[fi]);
  if (!f || f->type != FS_FILE || !f->data)
    return;
  int lines = 0;
  for (size_t i = 0; i < f->size && lines < n; i++) {
    vga_putc(f->data[i]);
    if (f->data[i] == '\n')
      lines++;
  }
  if (f->size && f->data[f->size - 1] != '\n')
    vga_putc('\n');
}

void cmd_tail(int argc, char **argv) {
  int n = 10, fi = 1;
  if (argc >= 4 && !strcmp(argv[1], "-n")) {
    n = atoi(argv[2]);
    fi = 3;
  }
  if (fi >= argc) {
    vga_puts("tail: missing file\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[fi]);
  if (!f || f->type != FS_FILE || !f->data)
    return;
  int total = 0;
  for (size_t i = 0; i < f->size; i++)
    if (f->data[i] == '\n')
      total++;
  int skip = total - n;
  if (skip < 0)
    skip = 0;
  int line = 0;
  for (size_t i = 0; i < f->size; i++) {
    if (line >= skip)
      vga_putc(f->data[i]);
    if (f->data[i] == '\n')
      line++;
  }
  if (f->size && f->data[f->size - 1] != '\n')
    vga_putc('\n');
}

void cmd_wc(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("wc: missing file\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[1]);
  if (!f || f->type != FS_FILE) {
    vga_puts("wc: not a file\n");
    return;
  }
  int lines = 0, words = 0, bytes = (int)f->size;
  bool in = false;
  if (f->data)
    for (size_t i = 0; i < f->size; i++) {
      char c = f->data[i];
      if (c == '\n')
        lines++;
      if (c == ' ' || c == '\t' || c == '\n') {
        if (in) {
          words++;
          in = false;
        }
      } else
        in = true;
    }
  if (in)
    words++;
  vga_printf(" %d %d %d %s\n", lines, words, bytes, argv[1]);
}

void cmd_grep(int argc, char **argv) {
  if (argc < 3) {
    vga_puts("grep: usage: grep <pattern> <file>\n");
    return;
  }
  fs_node_t *f = fs_resolve(cwd, argv[2]);
  if (!f || f->type != FS_FILE || !f->data) {
    vga_puts("grep: no such file\n");
    return;
  }
  int ln = 1;
  size_t line_start = 0;
  for (size_t i = 0; i <= f->size; i++) {
    if (i == f->size || f->data[i] == '\n') {
      char buf[256];
      size_t L = i - line_start;
      if (L > 255)
        L = 255;
      memcpy(buf, f->data + line_start, L);
      buf[L] = 0;
      if (strstr(buf, argv[1])) {
        char tmp[16];
        itoa(ln, tmp, 10);
        vga_set_color(VGA_LGREEN, VGA_BLACK);
        vga_puts(tmp);
        vga_set_color(VGA_LGREY, VGA_BLACK);
        vga_putc(':');
        vga_puts(buf);
        vga_putc('\n');
      }
      ln++;
      line_start = i + 1;
    }
  }
}

static const char *g_find_pattern;
static void find_recurse(fs_node_t *n) {
  if (!n)
    return;
  if (strstr(n->name, g_find_pattern)) {
    char p[256];
    fs_path(n, p, sizeof(p));
    vga_puts(p);
    vga_putc('\n');
  }
  if (n->type == FS_DIR)
    for (fs_node_t *c = n->child; c; c = c->next)
      find_recurse(c);
}
void cmd_find(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("find: usage: find <name>\n");
    return;
  }
  g_find_pattern = argv[1];
  find_recurse(cwd);
}
