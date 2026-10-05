#ifndef SAMARA_FILE_H
#define SAMARA_FILE_H
#include "core/types.h"
#include "fs/fs.h"
#include "core/wq.h"
#include "core/smp.h"

/* Open file descriptions, shared between fds after dup()/fork(). */

typedef enum { F_NODE = 1, F_TTY, F_PIPE_R, F_PIPE_W, F_NULL, F_ZERO, F_RANDOM, F_DISK, F_SOCKET, F_FB, F_INPUT,
               F_PTM, F_PTS,                        /* pty master / slave */
               F_SPAIR,                            /* AF_UNIX socketpair end: rx=pipe, tx=pipe2 */
               F_NETLINK,                          /* AF_NETLINK, the answer waits in pipe */
               F_USOCK, F_ULISTEN,                 /* AF_UNIX not connected yet / listening (ux) */
               F_EPOLL,
               F_EVENTFD, F_TIMERFD, F_DRM, F_SIGNALFD, F_SND,
               F_URING, F_INOTIFY, F_PIDFD, F_PMEM, F_RTC } ftype_t;

#define PIPE_SZ 65536           /* was 8k, x11 images through a socketpair crawled */

typedef struct pipe {
    char buf[PIPE_SZ];
    int  head, tail, count;
    int  readers, writers;
    struct file* fds[8];    /* SCM_RIGHTS in flight (unix sockets) */
    int  nfds;
    wq_t wq;
    uint32_t wgen;          /* bumped on every write, io_uring multishot poll looks at it */
    spin_t lk;              /* buf, head/tail/count, readers/writers */
} pipe_t;

typedef struct file {
    ftype_t    type;
    int        refs;
    int        flags;       /* O_* status flags (access mode, O_APPEND, O_NONBLOCK) */
    uint64_t   off;
    fs_node_t* node;
    pipe_t*    pipe;
    pipe_t*    pipe2;       /* F_SPAIR: transmit direction */
    int        shut;        /* F_SPAIR: 1 = rx shut, 2 = tx shut */
    struct snd_fd* snd;     /* F_SND */
    struct drm_fd* drm;     /* F_DRM, disk = 1 for renderD128 */
    int        disk;        /* F_DISK: ata index */
    struct sock* sock;      /* F_SOCKET */
    int        pty;         /* F_PTM / F_PTS: pair index */
    struct ux* ux;          /* F_USOCK / F_ULISTEN */
    struct ep* ep;          /* F_EPOLL */
    struct uring* ur;       /* F_URING */
    uint64_t   cnt;         /* F_EVENTFD counter */
    wq_t       wq;          /* F_EVENTFD */
    uint32_t   t_next, t_int;   /* F_TIMERFD: uptime ms of the next expiry (0 = off), interval */
} file_t;

file_t* file_new(ftype_t type, int flags);
uint32_t file_gen(file_t* f);
file_t* file_open_node(fs_node_t* n, int flags);   /* handles device nodes */
void    file_ref(file_t* f);
void    file_close(file_t* f);
void    efd_wake(void);
file_t* ino_new(int fl);
int     ino_read(file_t* f, char* buf, uint32_t n);
void    ino_close(file_t* f);
int     ino_add(file_t* f, fs_node_t* n, uint32_t mask);
int     ino_rm(file_t* f, int wd);
void    ino_ev(fs_node_t* n, uint32_t mask, const char* name, uint32_t cookie);
void    ino_node(fs_node_t* n, uint32_t mask);
void    ino_gone(fs_node_t* n, bool self);                            /* an eventfd counter went up */

int     file_read(file_t* f, char* buf, uint32_t n);
int     file_write(file_t* f, const char* buf, uint32_t n);
bool    file_readable(file_t* f);
bool    file_writable(file_t* f);

int     pipe_create(file_t** rd, file_t** wr);
int     pipe_tee(file_t* a, file_t* b, uint32_t len);
int     spair_create(file_t** a, file_t** b);
int     spair_shutdown(file_t* f, int how);
uint32_t file_disk_size(file_t* f);
void    ux_release(file_t* f);                     /* syscall.c: unbind, drop the backlog */
bool    ux_pending(file_t* f);                     /* a connection waits for accept() */

int     flk_flock(file_t* f, int op);
int     flk_fcntl(file_t* f, int cmd, uint8_t* u);
void    flk_close(void* sh, file_t* f);     /* a process closed an fd of f: its posix locks go */
void    flk_exit(void* sh);
void    flk_release(file_t* f);             /* last close */
int     flk_text(char* b, int cap);

/* ramfs helpers */
int     node_write_at(fs_node_t* n, uint32_t off, const char* buf, uint32_t len);
int     node_truncate(fs_node_t* n, uint32_t len);

#endif
