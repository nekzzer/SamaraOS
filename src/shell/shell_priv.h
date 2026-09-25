#ifndef SAMARA_SHELL_PRIV_H
#define SAMARA_SHELL_PRIV_H

/* Internal to the shell: state and helpers shared between shell.c (the
 * line editor + dispatcher) and the command builtins under shell/commands.
 * Not part of the public shell.h API. */

#include "core/types.h"
#include "fs/fs.h"
#include "gui/wm.h"

#define LINE_MAX 256
#define HIST_MAX 16

/* Current directory and shell lifecycle flags, owned by shell.c. */
extern fs_node_t *cwd;
extern int shell_request_exit;

/* WM-terminal state: which window (if any) is running this shell instance
 * as a desktop terminal, owned by shell.c. */
extern bool g_in_wm_terminal;
extern window_t *g_current_term_window;

/* Command history ring, owned by shell.c. */
extern char history[HIST_MAX][LINE_MAX];
extern int hist_count;

typedef struct {
  const char *name;
  void (*fn)(int, char **);
} cmd_t;

#endif
