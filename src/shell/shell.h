#ifndef SAMARA_SHELL_H
#define SAMARA_SHELL_H
#include "gui/wm.h"

void shell_run(void);
void shell_run_line(const char* line);   /* run one command line (boot autorun) */

/* Hooks used by the window manager terminal window. */
void        wm_terminal_init(window_t* w);
void        wm_terminal_handle_key(window_t* w, char c);
const char* wm_sysinfo_text(void);
void        wm_terminal_poll(window_t* w);    /* per frame, while open */
bool        wm_terminal_busy(void);           /* a program owns the keyboard */
void        wm_terminal_closed(void);         /* window gone: kill its programs */
bool        wm_terminal_submit(window_t* w, const char* line); /* type + run a command line */

#endif
