#ifndef SAMARA_DESKTOP_H
#define SAMARA_DESKTOP_H
#include "boot/multiboot.h"

void desktop_install_mbi(multiboot_info_t* mbi);
int  desktop_run(void);                       /* 0 ok */

/* Helpers reusable from the shell when running terminal-on-desktop. */
bool desktop_init_graphics(void);             /* try VBE then mode 13h */
void desktop_draw_chrome(int boot_sec_offset);
void desktop_draw_terminal_window(int x, int y, int w, int h, const char* title);

#endif
