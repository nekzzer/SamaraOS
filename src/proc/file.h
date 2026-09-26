#ifndef SAMARA_FILE_H
#define SAMARA_FILE_H
#include "core/types.h"
#include "fs/fs.h"

/* Open file descriptions, shared between fds after dup()/fork(). */

typedef enum { F_NODE = 1, F_TTY, F_PIPE_R, F_PIPE_W, F_NULL, F_ZERO, F_RANDOM, F_DISK, F_SOCKET, F_FB, F_INPUT,
               F_PTM, F_PTS } ftype_t;             /* pty master / slave */

#define PIPE_SZ 8192

typedef struct pipe {
    char buf[PIPE_SZ];
    int  head, tail, count;
    int  readers, writers;
} pipe_t;

typedef struct file {
    ftype_t    type;
    int        refs;
    int        flags;       /* O_* status flags (access mode, O_APPEND, O_NONBLOCK) */
    uint32_t   off;
    fs_node_t* node;
    pipe_t*    pipe;
    int        disk;        /* F_DISK: ata index */
    struct sock* sock;      /* F_SOCKET */
    int        pty;         /* F_PTM / F_PTS: pair index */
} file_t;

file_t* file_new(ftype_t type, int flags);
file_t* file_open_node(fs_node_t* n, int flags);   /* handles device nodes */
void    file_ref(file_t* f);
void    file_close(file_t* f);

int     file_read(file_t* f, char* buf, uint32_t n);
int     file_write(file_t* f, const char* buf, uint32_t n);
bool    file_readable(file_t* f);
bool    file_writable(file_t* f);

int     pipe_create(file_t** rd, file_t** wr);
uint32_t file_disk_size(file_t* f);

/* ramfs helpers */
int     node_write_at(fs_node_t* n, uint32_t off, const char* buf, uint32_t len);
int     node_truncate(fs_node_t* n, uint32_t len);

#endif
