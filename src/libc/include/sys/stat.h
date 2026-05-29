#ifndef _LIBC_SYS_STAT_H
#define _LIBC_SYS_STAT_H

#include "types.h"

struct stat {
    dev_t  st_dev;
    ino_t  st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t  st_uid;
    gid_t  st_gid;
    off_t  st_size;
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

int stat(const char* path, struct stat* st);
int mkdir(const char* path, mode_t mode);

#endif
