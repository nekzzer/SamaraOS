#ifndef _LIBC_UNISTD_H
#define _LIBC_UNISTD_H

#include "../../types.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int   isatty(int fd);
int   close(int fd);
ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
int   unlink(const char* path);
int   access(const char* path, int mode);
int   fileno(void* fp);
unsigned int sleep(unsigned int sec);
int   usleep(unsigned int usec);

#endif
