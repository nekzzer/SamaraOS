#ifndef SAMARA_PTERM_H
#define SAMARA_PTERM_H
#include "gui/wm.h"

/* One more Terminal window: /bin/sh on a pty, any number of them. */
window_t* pterm_open(int x, int y);

/* file drag and drop: types the (quoted) paths into the shell, 0 if w is not one of ours */
bool pterm_drop(window_t* w, const char* paths, int n);

#endif
