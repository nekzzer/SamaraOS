#ifndef SAMARA_PTY_H
#define SAMARA_PTY_H
#include "core/types.h"

/* Pseudo-terminals: /dev/ptmx hands out a master, /dev/pts/N is the slave.
   What the master writes goes through a termios line discipline (canonical
   editing, echo, ^C -> SIGINT, CR/NL mapping) to the slave's reader; what
   the slave writes (NL -> CRNL) is read from the master. telnetd and the
   dropbear ssh server run remote shells on the slave side. */

#define NPTY 16

int  pty_alloc(void);                          /* new pair: index, or -errno */
void pty_master_close(int i);
int  pty_slave_open(int i, int flags);         /* 0 or -errno; may become the ctty */
void pty_slave_close(int i);
int  pty_read(int i, bool master, char* buf, int n, bool nonblock);
int  pty_write(int i, bool master, const char* buf, int n, bool nonblock);
bool pty_readable(int i, bool master);
bool pty_writable(int i, bool master);
int  pty_ioctl(int i, bool master, uint32_t req, uint32_t arg);
int  pty_pending(int i, bool master);          /* FIONREAD */
struct fs_node;
struct fs_node* pty_node(int i);               /* /dev/pts/i */

#endif
