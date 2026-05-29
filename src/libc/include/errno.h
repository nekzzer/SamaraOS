#ifndef _LIBC_ERRNO_H
#define _LIBC_ERRNO_H

extern int errno;

#define EPERM    1
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define ENOMEM   12
#define EACCES   13
#define EBUSY    16
#define EEXIST   17
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENOSPC   28
#define ERANGE   34

#endif
