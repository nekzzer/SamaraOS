#ifndef SAMARA_PTERM_H
#define SAMARA_PTERM_H
#include "gui/wm.h"

/* One more Terminal window: /bin/sh on a pty, any number of them. */
window_t* pterm_open(int x, int y);

#endif
