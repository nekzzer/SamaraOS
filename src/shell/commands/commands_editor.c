#include "../shell_priv.h"
#include "core/string.h"
#include "core/task.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "gfx/gfx_term.h"
#include "drivers/vga.h"
#include "fs/fs.h"

#define E_BUF 8192

static char ebuf[E_BUF];
static int elen, ecur, etop, edesired_col, edirty;
static fs_node_t *efile;
static char ename[64];
static char emsg[64];
static int emsg_ticks;

static int e_line_of(int off) {
  int n = 0;
  for (int i = 0; i < off; i++)
    if (ebuf[i] == '\n')
      n++;
  return n;
}
static int e_col_of(int off) {
  int c = 0;
  for (int i = off - 1; i >= 0 && ebuf[i] != '\n'; i--)
    c++;
  return c;
}
static int e_offset_of(int line, int col) {
  int off = 0, l = 0;
  while (off < elen && l < line) {
    if (ebuf[off++] == '\n')
      l++;
  }
  if (l < line)
    return elen;
  int c = 0;
  while (off < elen && ebuf[off] != '\n' && c < col) {
    off++;
    c++;
  }
  return off;
}
static int e_total_lines(void) {
  int n = 1;
  for (int i = 0; i < elen; i++)
    if (ebuf[i] == '\n')
      n++;
  return n;
}
static void e_set_msg(const char *m) {
  int i = 0;
  while (m[i] && i < (int)sizeof(emsg) - 1) {
    emsg[i] = m[i];
    i++;
  }
  emsg[i] = 0;
  emsg_ticks = 1;
}

static void e_render(void) {
  int cur_line = e_line_of(ecur);
  int cur_col = e_col_of(ecur);
  int rows = vga_rows() - 2;

  if (cur_line < etop)
    etop = cur_line;
  if (cur_line >= etop + rows)
    etop = cur_line - rows + 1;
  if (etop < 0)
    etop = 0;

  vga_set_color(VGA_BLACK, VGA_LCYAN);
  vga_set_cursor(0, 0);
  char hdr[TERM_MAX_COLS + 1];
  for (int i = 0; i < vga_cols(); i++)
    hdr[i] = ' ';
  const char *p = " SamaraOS nano 0.2   ";
  int hp = 0;
  for (; p[hp]; hp++)
    hdr[hp] = p[hp];
  for (int i = 0; ename[i] && hp < vga_cols(); i++, hp++)
    hdr[hp] = ename[i];
  if (edirty && hp < vga_cols() - 4) {
    hdr[hp++] = ' ';
    hdr[hp++] = '*';
  }
  for (int i = 0; i < vga_cols(); i++)
    vga_putc(hdr[i]);

  vga_set_color(VGA_LGREY, VGA_BLACK);
  int off = 0, line = 0;
  while (off < elen && line < etop) {
    if (ebuf[off++] == '\n')
      line++;
  }

  for (int row = 1; row <= rows; row++) {
    vga_set_cursor(0, row);
    for (int i = 0; i < vga_cols(); i++)
      vga_putc(' ');
    vga_set_cursor(0, row);
    if (off >= elen && line + (row - 1) > 0) {
      vga_set_color(VGA_DGREY, VGA_BLACK);
      vga_putc('~');
      vga_set_color(VGA_LGREY, VGA_BLACK);
      continue;
    }
    int col = 0;
    while (off < elen && ebuf[off] != '\n') {
      if (col < vga_cols())
        vga_putc(ebuf[off]);
      off++;
      col++;
    }
    if (off < elen && ebuf[off] == '\n')
      off++;
  }

  vga_set_cursor(0, vga_rows() - 1);
  vga_set_color(VGA_BLACK, VGA_LGREY);
  char st[TERM_MAX_COLS + 1];
  for (int i = 0; i < vga_cols(); i++)
    st[i] = ' ';
  const char *sc = " ^S Save   ^X Exit   ^Q Quit ";
  int sp = 0;
  for (; sc[sp]; sp++)
    st[sp] = sc[sp];

  char info[64];
  int ip = 0;
  char tmp[16];
  info[ip++] = ' ';
  info[ip++] = 'L';
  info[ip++] = ':';
  itoa(cur_line + 1, tmp, 10);
  for (int i = 0; tmp[i]; i++)
    info[ip++] = tmp[i];
  info[ip++] = ' ';
  info[ip++] = 'C';
  info[ip++] = ':';
  itoa(cur_col + 1, tmp, 10);
  for (int i = 0; tmp[i]; i++)
    info[ip++] = tmp[i];
  info[ip++] = ' ';
  info[ip++] = 'B';
  info[ip++] = ':';
  itoa(elen, tmp, 10);
  for (int i = 0; tmp[i]; i++)
    info[ip++] = tmp[i];
  info[ip++] = ' ';
  info[ip] = 0;
  int istart = vga_cols() - ip;
  if (istart < sp + 1)
    istart = sp + 1;
  for (int i = 0; info[i] && istart + i < vga_cols(); i++)
    st[istart + i] = info[i];

  if (emsg_ticks > 0) {
    int ml = (int)strlen(emsg);
    int mstart = sp + 2;
    if (mstart + ml > istart - 1)
      ml = istart - 1 - mstart;
    for (int i = 0; i < ml; i++)
      st[mstart + i] = emsg[i];
    emsg_ticks = 0;
  }
  for (int i = 0; i < vga_cols(); i++)
    vga_putc(st[i]);
  vga_set_color(VGA_LGREY, VGA_BLACK);

  int sx = cur_col;
  int sy = (cur_line - etop) + 1;
  if (sx >= vga_cols())
    sx = vga_cols() - 1;
  if (sy < 1)
    sy = 1;
  if (sy > rows)
    sy = rows;
  vga_set_cursor(sx, sy);
}

static void e_insert(char c) {
  if (elen >= E_BUF - 1)
    return;
  if (ecur < elen)
    memmove(ebuf + ecur + 1, ebuf + ecur, elen - ecur);
  ebuf[ecur] = c;
  elen++;
  ecur++;
  edirty = 1;
  edesired_col = e_col_of(ecur);
}
static void e_backspace(void) {
  if (ecur == 0)
    return;
  if (ecur < elen)
    memmove(ebuf + ecur - 1, ebuf + ecur, elen - ecur);
  elen--;
  ecur--;
  edirty = 1;
  edesired_col = e_col_of(ecur);
}
static void e_delete(void) {
  if (ecur >= elen)
    return;
  memmove(ebuf + ecur, ebuf + ecur + 1, elen - ecur - 1);
  elen--;
  edirty = 1;
}

/* Mouse wheel: move the view 3 lines per notch, dragging the cursor along
   only when it would fall off screen. */
static void e_wheel(int dz) {
  int rows = vga_rows() - 2;
  int max_top = e_total_lines() - rows;
  if (max_top < 0)
    max_top = 0;
  etop += dz * 3;
  if (etop > max_top)
    etop = max_top;
  if (etop < 0)
    etop = 0;
  int line = e_line_of(ecur);
  if (line < etop)
    ecur = e_offset_of(etop, edesired_col);
  else if (line >= etop + rows)
    ecur = e_offset_of(etop + rows - 1, edesired_col);
}

void cmd_nano(int argc, char **argv) {
  if (argc < 2) {
    vga_puts("nano: missing file\n");
    return;
  }
  efile = fs_resolve(cwd, argv[1]);
  if (!efile)
    efile = fs_create(cwd, argv[1], FS_FILE);
  if (!efile || efile->type != FS_FILE) {
    vga_puts("nano: cannot open\n");
    return;
  }

  strncpy(ename, argv[1], sizeof(ename) - 1);
  ename[sizeof(ename) - 1] = 0;
  elen = 0;
  if (efile->data) {
    elen = (int)efile->size;
    if (elen > E_BUF - 1)
      elen = E_BUF - 1;
    memcpy(ebuf, efile->data, elen);
  }
  ebuf[elen] = 0;
  ecur = 0;
  etop = 0;
  edesired_col = 0;
  edirty = 0;
  emsg[0] = 0;
  emsg_ticks = 0;

  vga_clear();
  e_render();

  mouse_wheel_take();
  while (1) {
    int dz = 0;
    while (!kbd_has_key() && !(dz = mouse_wheel_take()))
      task_yield();
    if (dz) {
      e_wheel(dz);
      e_render();
      continue;
    }
    char c = kbd_trygetc();
    if (c == 17) {
      vga_clear();
      return;
    } /* ^Q */
    if (c == 24) { /* ^X */
      if (edirty) {
        e_set_msg("Unsaved! ^X again or ^S");
        edirty = 2;
        e_render();
        continue;
      }
      if (edirty == 2) {
        vga_clear();
        return;
      }
      vga_clear();
      return;
    }
    if (c == 19) {
      fs_write(efile, ebuf, elen);
      edirty = 0;
      e_set_msg("Saved");
      e_render();
      continue;
    }
    if (edirty == 2)
      edirty = 1;

    if (c == (char)K_LEFT) {
      if (ecur > 0) {
        ecur--;
        edesired_col = e_col_of(ecur);
      }
    } else if (c == (char)K_RIGHT) {
      if (ecur < elen) {
        ecur++;
        edesired_col = e_col_of(ecur);
      }
    } else if (c == (char)K_UP) {
      int line = e_line_of(ecur);
      if (line > 0)
        ecur = e_offset_of(line - 1, edesired_col);
    } else if (c == (char)K_DOWN) {
      int line = e_line_of(ecur);
      if (line < e_total_lines() - 1)
        ecur = e_offset_of(line + 1, edesired_col);
    } else if (c == (char)K_HOME) {
      ecur = e_offset_of(e_line_of(ecur), 0);
      edesired_col = 0;
    } else if (c == (char)K_END) {
      int line = e_line_of(ecur);
      int off = e_offset_of(line, 0);
      while (off < elen && ebuf[off] != '\n')
        off++;
      ecur = off;
      edesired_col = e_col_of(ecur);
    } else if (c == (char)K_PGUP) {
      int line = e_line_of(ecur) - (vga_rows() - 4);
      if (line < 0)
        line = 0;
      ecur = e_offset_of(line, edesired_col);
    } else if (c == (char)K_PGDN) {
      int line = e_line_of(ecur) + (vga_rows() - 4);
      int total = e_total_lines();
      if (line > total - 1)
        line = total - 1;
      ecur = e_offset_of(line, edesired_col);
    } else if (c == (char)K_DEL) {
      e_delete();
    } else if (c == '\b') {
      e_backspace();
    } else if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
      e_insert(c);
    }
    e_render();
  }
}
