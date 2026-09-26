/* Tab completion for the SamaraOS shell (text console and desktop terminal),
   bash-style:

     - first word            -> builtins + programs on PATH (+ paths if it has '/')
     - any other word        -> files/dirs, relative to cwd or absolute
     - `cd <word>`           -> directories only
     - one match             -> completed, then '/' for dirs or ' ' otherwise
     - several matches       -> extended to their longest common prefix;
                                a second Tab in a row lists them */

#include "shell/complete.h"
#include "shell_priv.h"

#include "core/string.h"
#include "drivers/vga.h"
#include "fs/fs.h"
#include "proc/proc.h"

#define MAX_MATCH 256
#define NAME_CAP  FS_NAME_MAX

typedef struct {
  char name[NAME_CAP];
  bool dir;
  bool exec;
} match_t;

static match_t matches[MAX_MATCH];
static int n_matches;

static const char *const path_dirs[] = {"/bin", "/sbin", "/usr/bin", "/usr/sbin",
                                        "/usr/games", "/opt/gcc/bin"};

static bool starts_with(const char *s, const char *pre, int n) {
  for (int i = 0; i < n; i++)
    if (s[i] != pre[i])
      return false;
  return true;
}

static void add_match(const char *name, bool dir, bool exec) {
  for (int i = 0; i < n_matches; i++)          /* PATH dirs overlap builtins */
    if (!strcmp(matches[i].name, name))
      return;
  if (n_matches >= MAX_MATCH)
    return;
  match_t *m = &matches[n_matches++];
  strncpy(m->name, name, NAME_CAP - 1);
  m->name[NAME_CAP - 1] = 0;
  m->dir = dir;
  m->exec = exec;
}

static void sort_matches(void) {
  for (int i = 1; i < n_matches; i++) {
    match_t v = matches[i];
    int j = i - 1;
    while (j >= 0 && strcmp(matches[j].name, v.name) > 0) {
      matches[j + 1] = matches[j];
      j--;
    }
    matches[j + 1] = v;
  }
}

static void collect_dir(fs_node_t *d, const char *pre, int pn, bool dirs_only,
                        bool exec_only) {
  if (!d || d->type != FS_DIR)
    return;
  for (fs_node_t *c = d->child; c; c = c->next) {
    if (c->unlinked)
      continue;
    if (c->name[0] == '.' && (pn == 0 || pre[0] != '.'))
      continue;                                  /* hidden unless asked for */
    if (!starts_with(c->name, pre, pn))
      continue;
    bool is_dir = c->type == FS_DIR;
    if (dirs_only && !is_dir)
      continue;
    if (exec_only && (is_dir || !(c->mode & 0111)))
      continue;
    add_match(c->name, is_dir, !is_dir && (c->mode & 0111));
  }
}

static void collect_commands(const char *pre, int pn) {
  const char *name;
  for (int i = 0; (name = shell_builtin_name(i)) != NULL; i++)
    if (starts_with(name, pre, pn))
      add_match(name, false, true);
  for (unsigned i = 0; i < sizeof(path_dirs) / sizeof(path_dirs[0]); i++)
    collect_dir(fs_resolve(fs_root(), path_dirs[i]), pre, pn, false, true);
}

/* Is the word at `start` the first word of the line? */
static bool is_first_word(const char *buf, int start) {
  for (int i = 0; i < start; i++)
    if (buf[i] != ' ' && buf[i] != '\t')
      return false;
  return true;
}

static bool first_word_is(const char *buf, const char *cmd) {
  int i = 0;
  while (buf[i] == ' ' || buf[i] == '\t')
    i++;
  int n = strlen(cmd);
  return starts_with(buf + i, cmd, n) && (buf[i + n] == ' ' || buf[i + n] == '\t');
}

static bool last_was_tab;

void shell_complete_reset(void) { last_was_tab = false; }

static int insert(char *buf, int *len, int *cur, int cap, const char *s, int n) {
  if (*len + n > cap - 1)
    n = cap - 1 - *len;
  if (n <= 0)
    return 0;
  memmove(buf + *cur + n, buf + *cur, *len - *cur);
  memcpy(buf + *cur, s, n);
  *len += n;
  *cur += n;
  buf[*len] = 0;
  return n;
}

int shell_complete(char *buf, int *len, int *cur, int cap, int *from) {
  bool second = last_was_tab;
  last_was_tab = true;
  procfs_refresh();                           /* so /proc completes too */

  int start = *cur;
  while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t')
    start--;
  int wlen = *cur - start;
  char word[LINE_MAX];
  memcpy(word, buf + start, wlen);
  word[wlen] = 0;

  int slash = -1;
  for (int i = 0; i < wlen; i++)
    if (word[i] == '/')
      slash = i;
  const char *pre = word + slash + 1;          /* part being completed */
  int pn = wlen - slash - 1;

  n_matches = 0;
  if (is_first_word(buf, start) && slash < 0) {
    collect_commands(pre, pn);
  } else {
    fs_node_t *dir = cwd;
    if (slash >= 0) {
      char dpath[LINE_MAX];
      memcpy(dpath, word, slash);
      dpath[slash] = 0;
      dir = slash == 0 ? fs_root() : fs_resolve(cwd, dpath);
    }
    bool dirs_only = first_word_is(buf, "cd") || first_word_is(buf, "mkdir") ||
                     first_word_is(buf, "fatls");
    if (pn == 1 && pre[0] == '.') {             /* offer ./ and ../ like bash */
      add_match(".", true, false);
      add_match("..", true, false);
    } else if (pn == 2 && pre[0] == '.' && pre[1] == '.') {
      add_match("..", true, false);
    }
    collect_dir(dir, pre, pn, dirs_only, false);
  }
  sort_matches();

  if (n_matches == 0)
    return SC_NONE;

  *from = *cur;
  if (n_matches == 1) {
    last_was_tab = false;
    const match_t *m = &matches[0];
    int ins = insert(buf, len, cur, cap, m->name + pn, strlen(m->name) - pn);
    /* Don't double the separator if the user is completing mid-line. */
    char sep = m->dir ? '/' : ' ';
    if (buf[*cur] != sep)
      ins += insert(buf, len, cur, cap, &sep, 1);
    else
      (*cur)++;
    return SC_EDIT;
  }

  /* Longest common prefix of all candidates. */
  int lcp = strlen(matches[0].name);
  for (int i = 1; i < n_matches; i++) {
    int k = 0;
    while (k < lcp && matches[i].name[k] == matches[0].name[k])
      k++;
    lcp = k;
  }
  if (lcp > pn) {
    insert(buf, len, cur, cap, matches[0].name + pn, lcp - pn);
    return SC_EDIT;
  }
  return second ? SC_LIST : SC_NONE;
}

void shell_complete_print_list(void) {
  int widest = 0;
  for (int i = 0; i < n_matches; i++) {
    int w = strlen(matches[i].name) + (matches[i].dir ? 1 : 0);
    if (w > widest)
      widest = w;
  }
  int colw = widest + 2;
  int cols = vga_cols() / colw;
  if (cols < 1)
    cols = 1;
  int rows = (n_matches + cols - 1) / cols;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      int i = c * rows + r;                     /* column-major, like ls */
      if (i >= n_matches)
        break;
      const match_t *m = &matches[i];
      if (m->dir)
        vga_set_color(VGA_LBLUE, VGA_BLACK);
      else if (m->exec)
        vga_set_color(VGA_LGREEN, VGA_BLACK);
      vga_puts(m->name);
      int w = strlen(m->name);
      if (m->dir) {
        vga_putc('/');
        w++;
      }
      vga_set_color(VGA_LGREY, VGA_BLACK);
      bool last_in_row = (c + 1) * rows + r >= n_matches || c + 1 == cols;
      if (!last_in_row)
        for (; w < colw; w++)
          vga_putc(' ');
    }
    vga_putc('\n');
  }
}
