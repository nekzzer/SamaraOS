#ifndef SAMARA_SHELL_COMPLETE_H
#define SAMARA_SHELL_COMPLETE_H
#include "core/types.h"

enum {
  SC_NONE = 0,   /* nothing to do */
  SC_EDIT,       /* buf changed from *from onwards: redraw the tail */
  SC_LIST,       /* several candidates: caller prints the list + reprompts */
};

/* Tab pressed with the cursor at *cur in buf[0..*len). */
int  shell_complete(char *buf, int *len, int *cur, int cap, int *from);
/* Any key other than Tab: the next Tab is a "first" Tab again. */
void shell_complete_reset(void);
/* Prints the candidates of the last SC_LIST in columns. */
void shell_complete_print_list(void);

/* Builtin command names, NULL past the end (shell.c). */
const char *shell_builtin_name(int i);

#endif
