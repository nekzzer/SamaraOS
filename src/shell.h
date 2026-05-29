#ifndef SAMARA_SHELL_H
#define SAMARA_SHELL_H
#include "wm.h"

void shell_run(void);

/* Hooks used by the window manager terminal window. */
void        wm_terminal_init(window_t* w);
void        wm_terminal_handle_key(window_t* w, char c);
const char* wm_sysinfo_text(void);

#endif
