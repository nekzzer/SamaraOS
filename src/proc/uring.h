#ifndef SAMARA_URING_H
#define SAMARA_URING_H
#include "core/types.h"

struct uring;
struct file;
struct proc;

void uring_init(void);          /* starts the worker task */
int  uring_setup(uint32_t entries, void* params);
int  uring_enter(int fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags, const void* arg, size_t argsz);
int  uring_register(int fd, uint32_t op, void* arg, uint32_t nr);
int  uring_mmap(struct uring* r, uint64_t pd, uint64_t addr, uint64_t len, uint64_t off);
void uring_release(struct uring* r);        /* last close of the ring file */
void uring_exit(struct proc* p);            /* a process or thread goes away: drop its requests */
bool uring_readable(struct uring* r);

/* syscall.c helpers, all run in the context of the current process */
struct file* sys_getf(int fd);
bool sys_uok(const void* p, uint32_t len);
int  sys_openat(int dirfd, const char* path, int flags, int mode);
int  sys_close(int fd);
int  sys_sock(int call, int fd, size_t b, size_t c, size_t d, size_t e, size_t f);
int  sys_statx(int dirfd, const char* path, int flags, uint32_t mask, void* out);
int16_t sys_revents(struct file* f, int16_t want);
int  sys_tmpfd(struct file* f);
void sys_untmpfd(int fd);

#endif
