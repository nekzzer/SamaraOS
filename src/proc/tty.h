#ifndef SAMARA_TTY_H
#define SAMARA_TTY_H
#include "core/types.h"

/* The one console tty user processes talk to.

   Processes write into an output ring; whoever owns the screen (the text
   shell loop or the desktop terminal window) drains it with tty_pump(),
   which runs a small VT100/ANSI interpreter on top of the vga_* console.
   Keys go the other way through tty_key(), which applies the termios line
   discipline (canonical editing, echo, ^C -> SIGINT). */

/* Linux termios bits we honour. */
#define TTY_ISIG   0x0001
#define TTY_ICANON 0x0002
#define TTY_ECHO   0x0008
#define TTY_ECHOE  0x0010
#define TTY_ICRNL  0x0100
#define TTY_OPOST  0x0001
#define TTY_ONLCR  0x0004

#define TTY_NCCS 19
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6

typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[TTY_NCCS];
} __attribute__((packed)) ktermios_t;

void tty_init(void);
void tty_reset(void);                 /* fresh session: default termios, empty buffers */

/* Process side (may block by yielding). */
int  tty_read(char* buf, int n, bool nonblock);   /* bytes, 0 = EOF, <0 = -errno */
int  tty_write(const char* buf, int n);
bool tty_readable(void);
void tty_get_termios(ktermios_t* t);
void tty_set_termios(const ktermios_t* t);
int  tty_fg_pgrp(void);
void tty_set_fg_pgrp(int pgrp);
void tty_winsize(int* rows, int* cols);

/* Console side. */
void tty_key(char c);                 /* keyboard byte / K_* code */
void tty_pump(void);                  /* render pending output */
bool tty_has_output(void);
void tty_resized(void);               /* console grid changed: SIGWINCH */
bool tty_wheel(int dz);               /* wheel -> keys for full-screen apps */
bool tty_fullscreen(void);

#endif
