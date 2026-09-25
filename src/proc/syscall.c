/* Linux i386 system call ABI (int 0x80): eax = number, args in ebx, ecx,
   edx, esi, edi, ebp; result (or -errno) back in eax. Only what static musl
   binaries such as busybox actually need is implemented; everything else
   answers -ENOSYS, which musl and busybox handle gracefully. */

#include "gui/uwin.h"
#include "proc/proc.h"
#include "proc/file.h"
#include "drivers/fbdev.h"
#include "proc/tty.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/vmm.h"
#include "core/clock.h"
#include "core/io.h"
#include "boot/idt.h"
#include "boot/gdt.h"
#include "boot/pit.h"
#include "drivers/ata.h"
#include "net/sock.h"
#include "fs/fatfs.h"
#include "net/net.h"

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ESPIPE 29
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39

#define O_ACCMODE   3
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

#define AT_FDCWD      -100
#define AT_REMOVEDIR  0x200

#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000

bool g_strace;                          /* `strace on` in the kernel shell */

/* ---------------- serial ---------------- */

static void com_putc(char c) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, c); }
static void klog(const char* s) { while (*s) com_putc(*s++); }
static void klog_num(int32_t v) { char b[16]; itoa(v, b, 10); klog(b); }

/* ---------------- user memory checks ---------------- */

static bool uok(const void* p, uint32_t len) {
    uint32_t a = (uint32_t)p;
    return a >= USER_BASE && a < USER_TOP && len <= USER_TOP - a;
}
#define UCHK(p, n) do { if (!uok((p), (n))) return -EFAULT; } while (0)

/* ---------------- fds + paths ---------------- */

static proc_t* me(void) { return proc_current(); }

static file_t* getf(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return me()->fds[fd];
}

static int alloc_fd(int from) {
    for (int i = from < 0 ? 0 : from; i < MAX_FDS; i++)
        if (!me()->fds[i]) return i;
    return -EMFILE;
}

static int install_fd(file_t* f, int from, bool cloexec) {
    int fd = alloc_fd(from);
    if (fd < 0) { file_close(f); return fd; }
    me()->fds[fd] = f;
    me()->cloexec[fd] = cloexec;
    return fd;
}

static fs_node_t* dir_base(int dirfd, const char* path, int* err) {
    if (path[0] == '/' || dirfd == AT_FDCWD) return me()->cwd;
    file_t* f = getf(dirfd);
    if (!f) { *err = -EBADF; return NULL; }
    if (f->type != F_NODE || f->node->type != FS_DIR) { *err = -ENOTDIR; return NULL; }
    return f->node;
}

/* /proc is regenerated whenever a path lookup might land in it. */
static bool in_procfs(fs_node_t* n) {
    for (; n; n = n->parent)
        if (n->parent == fs_root() && !strcmp(n->name, "proc")) return true;
    return false;
}

static void maybe_refresh_proc(fs_node_t* base, const char* path) {
    if (path[0] == '/' ? !strncmp(path, "/proc", 5) : in_procfs(base))
        procfs_refresh();
}

static fs_node_t* lookup(int dirfd, const char* path, int* err) {
    *err = -ENOENT;
    if (!path[0]) return NULL;
    fs_node_t* base = dir_base(dirfd, path, err);
    if (!base) return NULL;
    maybe_refresh_proc(base, path);
    fs_node_t* n = fs_resolve(base, path);
    if (!n) *err = -ENOENT;
    return n;
}

/* Split "a/b/c/" into parent node of "c" and the name "c". */
static fs_node_t* lookup_parent(int dirfd, const char* path, char* name, int* err) {
    char tmp[256];
    int len = (int)strlen(path);
    if (len == 0) { *err = -ENOENT; return NULL; }
    if (len >= (int)sizeof(tmp)) { *err = -ENAMETOOLONG; return NULL; }
    strcpy(tmp, path);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;
    int slash = -1;
    for (int i = 0; i < len; i++) if (tmp[i] == '/') slash = i;
    const char* base = tmp + slash + 1;
    if (strlen(base) >= FS_NAME_MAX) { *err = -ENAMETOOLONG; return NULL; }
    strcpy(name, base);
    fs_node_t* b = dir_base(dirfd, path, err);
    if (!b) return NULL;
    fs_node_t* parent;
    if (slash < 0) parent = b;
    else if (slash == 0) parent = fs_root();
    else { tmp[slash] = 0; parent = fs_resolve(b, tmp); }
    if (!parent) { *err = -ENOENT; return NULL; }
    if (parent->type != FS_DIR) { *err = -ENOTDIR; return NULL; }
    return parent;
}

/* ---------------- stat ---------------- */

typedef struct {
    uint64_t st_dev;
    uint32_t pad0;
    uint32_t st_ino32;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid, st_gid;
    uint64_t st_rdev;
    uint32_t pad3;
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime, st_atime_ns;
    uint32_t st_mtime, st_mtime_ns;
    uint32_t st_ctime, st_ctime_ns;
    uint64_t st_ino;
} __attribute__((packed)) kstat64_t;

static void fill_stat_node(kstat64_t* st, fs_node_t* n) {
    memset(st, 0, sizeof(*st));
    int vol = fatfs_owner(n);
    st->st_dev = vol ? (8u << 8) | (uint32_t)vol : 1;            /* distinct per volume (df) */
    st->st_ino = st->st_ino32 = ((uint32_t)n >> 4) & 0x0FFFFFFF;
    st->st_nlink = n->type == FS_DIR ? 2 : 1;
    if (n->dev >= FS_DEV_DISK) {
        int idx = n->dev - FS_DEV_DISK;
        st->st_mode = 0060000 | (n->mode & 07777);            /* S_IFBLK */
        st->st_rdev = idx >= DISK_AHCI_BASE ? ((8u << 8) | (uint32_t)(idx - DISK_AHCI_BASE) * 16)
                                            : ((3u << 8) | (uint32_t)idx * 64);
        st->st_size = (int64_t)ata_drive_sectors(idx) * 512;
    } else if (n->dev) {
        st->st_mode = S_IFCHR | (n->mode & 07777);
        st->st_rdev = n->dev == FS_DEV_TTY ? (5u << 8) : ((1u << 8) | n->dev);
    } else if (n->type == FS_DIR) {
        st->st_mode = S_IFDIR | (n->mode & 07777);
        st->st_size = 4096;
    } else {
        st->st_mode = S_IFREG | (n->mode & 07777);
        st->st_size = n->size;
    }
    st->st_blksize = 4096;
    st->st_blocks = ((uint64_t)st->st_size + 511) / 512;
    st->st_atime = st->st_mtime = st->st_ctime = n->mtime;
}

static void fill_stat_file(kstat64_t* st, file_t* f) {
    if (f->type == F_NODE) { fill_stat_node(st, f->node); return; }
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = st->st_ino32 = ((uint32_t)f >> 4) & 0x0FFFFFFF;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (f->type == F_PIPE_R || f->type == F_PIPE_W) st->st_mode = S_IFIFO | 0600;
    else if (f->type == F_SOCKET) st->st_mode = 0140000 | 0777;          /* S_IFSOCK */
    else { st->st_mode = S_IFCHR | 0666; st->st_rdev = f->type == F_TTY ? (5u << 8) : (1u << 8) | 3; }
    st->st_atime = st->st_mtime = st->st_ctime = clock_epoch();
}

/* ---------------- file syscalls ---------------- */

static int do_open(int dirfd, const char* path, int flags, int mode) {
    UCHK(path, 1);
    int err;
    fs_node_t* n = lookup(dirfd, path, &err);
    if (n && (flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (!n) {
        if (!(flags & O_CREAT) || err != -ENOENT) return err;
        char name[FS_NAME_MAX];
        fs_node_t* parent = lookup_parent(dirfd, path, name, &err);
        if (!parent) return err;
        n = fs_create(parent, name, FS_FILE);
        if (!n) return -EACCES;
        n->mode = (uint16_t)(mode & ~me()->umask & 07777);
    }
    if ((flags & O_DIRECTORY) && n->type != FS_DIR) return -ENOTDIR;
    if (n->type == FS_DIR && (flags & O_ACCMODE) != 0) return -EISDIR;
    if ((flags & O_TRUNC) && n->type == FS_FILE && !n->dev && (flags & O_ACCMODE)) node_truncate(n, 0);
    file_t* f = file_open_node(n, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
    if (!f) return -ENOMEM;
    return install_fd(f, 0, (flags & O_CLOEXEC) != 0);
}

static int do_read(int fd, char* buf, uint32_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    UCHK(buf, n);
    return file_read(f, buf, n);
}

static int do_write(int fd, const char* buf, uint32_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == 0 && f->type == F_NODE) return -EBADF;
    UCHK(buf, n);
    return file_write(f, buf, n);
}

typedef struct { uint32_t base, len; } iovec_t;

static int do_rwv(int fd, iovec_t* iov, int cnt, bool wr) {
    UCHK(iov, (uint32_t)cnt * sizeof(iovec_t));
    int total = 0;
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        int r = wr ? do_write(fd, (const char*)iov[i].base, iov[i].len)
                   : do_read(fd, (char*)iov[i].base, iov[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint32_t)r < iov[i].len) break;
    }
    return total;
}

static int64_t do_lseek(int fd, int64_t off, int whence) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if (f->type == F_FB) {                                   /* linear pixel memory */
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : -1;
        if (base < 0) return -EINVAL;
        if (base + off < 0) return -EINVAL;
        f->off = (uint32_t)(base + off);
        return base + off;
    }
    if (f->type != F_NODE && f->type != F_DISK) return f->type == F_NULL || f->type == F_ZERO ? 0 : -ESPIPE;
    uint32_t end = f->type == F_DISK ? file_disk_size(f) : f->node->size;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off :
                   whence == 2 ? (int64_t)end : -1;
    if (base < 0) return -EINVAL;
    int64_t pos = base + off;
    if (pos < 0 || pos > 0x7FFFFFFF) return -EINVAL;
    f->off = (uint32_t)pos;
    return pos;
}

static int do_getdents64(int fd, uint8_t* buf, uint32_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if (f->type != F_NODE || f->node->type != FS_DIR) return -ENOTDIR;
    UCHK(buf, n);
    fs_node_t* dir = f->node;
    if (f->off == 0 && in_procfs(dir)) procfs_refresh();
    uint32_t pos = 0, idx = 0;
    fs_node_t* c = dir->child;
    for (;; idx++) {
        const char* name;
        fs_node_t* node;
        if (idx == 0)      { name = ".";  node = dir; }
        else if (idx == 1) { name = ".."; node = dir->parent ? dir->parent : dir; }
        else {
            if (!c) break;
            name = c->name; node = c; c = c->next;
        }
        if (idx < f->off) continue;
        uint32_t nl = strlen(name);
        uint32_t reclen = (19 + nl + 1 + 7) & ~7u;
        if (pos + reclen > n) { if (pos == 0) return -EINVAL; break; }
        uint8_t* d = buf + pos;
        uint64_t ino = ((uint32_t)node >> 4) & 0x0FFFFFFF;
        int64_t next = idx + 1;
        memcpy(d, &ino, 8);
        memcpy(d + 8, &next, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(d + 16, &rl, 2);
        d[18] = node->dev ? 2 : node->type == FS_DIR ? 4 : 8;
        memcpy(d + 19, name, nl + 1);
        pos += reclen;
        f->off = idx + 1;
    }
    return (int)pos;
}

static int do_unlink(int dirfd, const char* path, int flags) {
    UCHK(path, 1);
    int err;
    fs_node_t* n = lookup(dirfd, path, &err);
    if (!n) return err;
    if (flags & AT_REMOVEDIR) {
        if (n->type != FS_DIR) return -ENOTDIR;
        if (n->child) return -ENOTEMPTY;
        if (n == fs_root()) return -EBUSY;
        for (int i = 0; i < proc_count(); i++) {
            proc_t* p = proc_at(i);
            if (p && p->cwd == n) return -EBUSY;
        }
    } else if (n->type == FS_DIR) {
        return -EISDIR;
    }
    fs_node_t* parent = n->parent;
    if (!parent) return -EBUSY;
    fs_detach(n);
    if (n->refs > 0) n->unlinked = true;
    else { if (n->data) kfree(n->data); kfree(n); }
    return 0;
}

static int do_mkdir(int dirfd, const char* path, int mode) {
    UCHK(path, 1);
    int err;
    if (lookup(dirfd, path, &err)) return -EEXIST;
    char name[FS_NAME_MAX];
    fs_node_t* parent = lookup_parent(dirfd, path, name, &err);
    if (!parent) return err;
    fs_node_t* n = fs_create(parent, name, FS_DIR);
    if (!n) return -EEXIST;
    n->mode = (uint16_t)(mode & ~me()->umask & 07777);
    return 0;
}

static bool is_ancestor(fs_node_t* a, fs_node_t* n) {
    for (; n; n = n->parent) if (n == a) return true;
    return false;
}

static int do_rename(int ofd, const char* from, int nfd, const char* to) {
    UCHK(from, 1); UCHK(to, 1);
    int err;
    fs_node_t* src = lookup(ofd, from, &err);
    if (!src) return err;
    char name[FS_NAME_MAX];
    fs_node_t* parent = lookup_parent(nfd, to, name, &err);
    if (!parent) return err;
    if (src->type == FS_DIR && is_ancestor(src, parent)) return -EINVAL;
    fs_node_t* dst = fs_child(parent, name);
    if (dst == src) return 0;
    if (dst) {
        if (dst->type == FS_DIR && src->type != FS_DIR) return -EISDIR;
        if (dst->type != FS_DIR && src->type == FS_DIR) return -ENOTDIR;
        if (dst->type == FS_DIR && dst->child) return -ENOTEMPTY;
        int r = do_unlink(AT_FDCWD, to, dst->type == FS_DIR ? AT_REMOVEDIR : 0);
        if (r < 0) return r;
    }
    fs_detach(src);
    strncpy(src->name, name, FS_NAME_MAX - 1);
    src->name[FS_NAME_MAX - 1] = 0;
    fs_attach(parent, src);
    return 0;
}

static int do_access(int dirfd, const char* path) {
    UCHK(path, 1);
    int err;
    return lookup(dirfd, path, &err) ? 0 : err;
}

static int do_dup(int fd, int from, bool cloexec) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    file_ref(f);
    return install_fd(f, from, cloexec);
}

static int do_dup2(int fd, int nfd, bool cloexec) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if (nfd < 0 || nfd >= MAX_FDS) return -EBADF;
    if (fd == nfd) return nfd;
    if (me()->fds[nfd]) file_close(me()->fds[nfd]);
    file_ref(f);
    me()->fds[nfd] = f;
    me()->cloexec[nfd] = cloexec;
    return nfd;
}

static int do_fcntl(int fd, int cmd, uint32_t arg) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    switch (cmd) {
        case 0:    return do_dup(fd, (int)arg, false);          /* F_DUPFD */
        case 1030: return do_dup(fd, (int)arg, true);           /* F_DUPFD_CLOEXEC */
        case 1:    return me()->cloexec[fd] ? 1 : 0;            /* F_GETFD */
        case 2:    me()->cloexec[fd] = arg & 1; return 0;       /* F_SETFD */
        case 3:    return f->flags;                              /* F_GETFL */
        case 4:    f->flags = (f->flags & O_ACCMODE) | (int)(arg & (O_APPEND | O_NONBLOCK)); return 0;
        case 5: case 6: case 7: case 12: case 13: case 14:       /* locks: always granted */
            return 0;
    }
    return -EINVAL;
}

static int do_pipe(int* fds, int flags) {
    UCHK(fds, 8);
    file_t *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    if (flags & O_NONBLOCK) { r->flags |= O_NONBLOCK; w->flags |= O_NONBLOCK; }
    int a = install_fd(r, 0, (flags & O_CLOEXEC) != 0);
    if (a < 0) { file_close(w); return a; }
    int b = install_fd(w, 0, (flags & O_CLOEXEC) != 0);
    if (b < 0) { file_close(me()->fds[a]); me()->fds[a] = NULL; return b; }
    fds[0] = a;
    fds[1] = b;
    return 0;
}

static int do_ioctl(int fd, uint32_t req, uint32_t arg) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if (req == 0x5421) {                                      /* FIONBIO */
        UCHK((void*)arg, 4);
        if (*(int*)arg) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK;
        return 0;
    }
    if (req == 0x541B) {                                      /* FIONREAD */
        UCHK((void*)arg, 4);
        int n = 0;
        if (f->type == F_PIPE_R) n = f->pipe->count;
        else if (f->type == F_NODE && f->node->type == FS_FILE && f->off < f->node->size)
            n = (int)(f->node->size - f->off);
        else if (f->type == F_TTY) n = tty_readable() ? 1 : 0;
        *(int*)arg = n;
        return 0;
    }
    if (f->type == F_FB) {
        if (req == FBIOGET_VSCREENINFO) { UCHK((void*)arg, 160); fbdev_vscreeninfo((uint32_t*)arg); return 0; }
        if (req == FBIOGET_FSCREENINFO) { UCHK((void*)arg, 68);  fbdev_fscreeninfo((uint32_t*)arg); return 0; }
        if (req == FBIO_BLIT8) {
            UCHK((void*)arg, sizeof(fb_blit8_t));
            const fb_blit8_t* b = (const fb_blit8_t*)arg;
            if (b->w > 4096 || b->h > 4096) return -EINVAL;
            UCHK((void*)b->pixels, b->w * b->h);
            UCHK((void*)b->palette, 1024);
            return fbdev_blit8((const uint8_t*)b->pixels, (int)b->w, (int)b->h, (const uint32_t*)b->palette);
        }
        return -ENOTTY;
    }
    if (f->type == F_DISK) {
        if (req == 0x1260) {                                  /* BLKGETSIZE (sectors) */
            UCHK((void*)arg, 4); *(uint32_t*)arg = ata_drive_sectors(f->disk); return 0;
        }
        if (req == 0x80041272) {                              /* BLKGETSIZE64 */
            UCHK((void*)arg, 8);
            uint64_t b = (uint64_t)ata_drive_sectors(f->disk) * 512;
            memcpy((void*)arg, &b, 8);
            return 0;
        }
        if (req == 0x1268) { UCHK((void*)arg, 4); *(int*)arg = 512; return 0; }   /* BLKSSZGET */
        return -ENOTTY;
    }
    if (f->type != F_TTY) return -ENOTTY;
    switch (req) {
        case 0x5401: {                                        /* TCGETS */
            UCHK((void*)arg, sizeof(ktermios_t));
            ktermios_t t; tty_get_termios(&t);
            memcpy((void*)arg, &t, sizeof(t));
            return 0;
        }
        case 0x5402: case 0x5403: case 0x5404: {              /* TCSETS/W/F */
            UCHK((void*)arg, sizeof(ktermios_t));
            ktermios_t t;
            memcpy(&t, (void*)arg, sizeof(t));
            tty_set_termios(&t);
            return 0;
        }
        case 0x5413: {                                        /* TIOCGWINSZ */
            UCHK((void*)arg, 8);
            int rows, cols;
            tty_winsize(&rows, &cols);
            uint16_t* ws = (uint16_t*)arg;
            ws[0] = (uint16_t)rows; ws[1] = (uint16_t)cols; ws[2] = ws[3] = 0;
            return 0;
        }
        case 0x540F:                                          /* TIOCGPGRP */
            UCHK((void*)arg, 4);
            *(int*)arg = tty_fg_pgrp() ? tty_fg_pgrp() : me()->pgid;
            return 0;
        case 0x5410:                                          /* TIOCSPGRP */
            UCHK((void*)arg, 4);
            tty_set_fg_pgrp(*(int*)arg);
            return 0;
        case 0x5429:                                          /* TIOCGSID */
            UCHK((void*)arg, 4);
            *(int*)arg = me()->sid;
            return 0;
        case 0x5414: case 0x540E: case 0x5422:                /* SWINSZ, SCTTY, NOTTY */
        case 0x5409: case 0x540A: case 0x540B:                /* TCSBRK, TCXONC, TCFLSH */
            return 0;
    }
    return -EINVAL;
}

/* ---------------- memory ---------------- */

static uint32_t do_brk(uint32_t want) {
    proc_t* p = me();
    if (want < p->brk_start || want >= USER_MMAP_BASE) return p->brk;
    uint32_t old_top = (p->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_top = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_top > old_top) {
        if (!vmm_range_unmapped(p->pd, old_top, new_top - old_top)) return p->brk;
        if (vmm_alloc_range(p->pd, old_top, new_top - old_top, true) < 0) {
            vmm_free_range(p->pd, old_top, new_top - old_top);
            vmm_flush();
            return p->brk;
        }
    } else if (new_top < old_top) {
        vmm_free_range(p->pd, new_top, old_top - new_top);
        vmm_flush();
    }
    p->brk = want;
    return want;
}

#define MAP_FIXED 0x10
#define MAP_ANON  0x20
#define PROT_WRITE 2

static int32_t do_mmap(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t off) {
    proc_t* p = me();
    if (!len) return -EINVAL;
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    file_t* f = NULL;
    if (!(flags & MAP_ANON)) {
        f = getf(fd);
        if (!f) return -EBADF;
        if (f->type != F_NODE && f->type != F_ZERO) return -EACCES;
    }
    uint32_t lo = USER_MMAP_BASE, hi = USER_STACK_TOP - USER_STACK_MAX;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_TOP || addr + len < addr)
            return -EINVAL;
        vmm_free_range(p->pd, addr, len);
    } else if (addr && !(addr & (PAGE_SIZE - 1)) && addr >= lo && addr + len <= hi &&
               vmm_range_unmapped(p->pd, addr, len)) {
        /* honour the hint */
    } else {
        addr = vmm_find_free(p->pd, lo, hi, len);
        if (!addr) return -ENOMEM;
    }
    if (vmm_alloc_range(p->pd, addr, len, true) < 0) {
        vmm_free_range(p->pd, addr, len);
        vmm_flush();
        return -ENOMEM;
    }
    if (f && f->type == F_NODE && f->node->type == FS_FILE && off < f->node->size) {
        uint32_t n = f->node->size - off;
        if (n > len) n = len;
        vmm_copy_to(p->pd, addr, f->node->data + off, n);
    }
    if (!(prot & PROT_WRITE)) vmm_set_writable(p->pd, addr, len, false);
    vmm_flush();
    return (int32_t)addr;
}

static int do_munmap(uint32_t addr, uint32_t len) {
    if ((addr & (PAGE_SIZE - 1)) || !len) return -EINVAL;
    if (addr < USER_BASE || addr >= USER_TOP) return 0;
    vmm_free_range(me()->pd, addr, len);
    vmm_flush();
    return 0;
}

static int do_mprotect(uint32_t addr, uint32_t len, int prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (addr < USER_BASE || addr >= USER_TOP) return 0;
    vmm_set_writable(me()->pd, addr, len, (prot & PROT_WRITE) != 0);
    vmm_flush();
    return 0;
}

/* ---------------- time ---------------- */

static int sleep_ms(uint32_t ms) {
    uint32_t end = pit_uptime_ms() + ms;
    while ((int32_t)(pit_uptime_ms() - end) < 0) {
        if (proc_interrupted()) return -EINTR;
        uint32_t left = end - pit_uptime_ms();
        task_sleep_ms(left > 20 ? 20 : left);        /* short naps: signals stay prompt */
    }
    return 0;
}

static int do_clock_gettime(int clk, uint32_t* ts, bool t64) {
    UCHK(ts, t64 ? 16 : 8);
    uint32_t s, ns;
    if (clk == 0 || clk == 5 || clk == 8) clock_now(&s, &ns);     /* REALTIME(_COARSE), BOOTTIME */
    else { uint32_t ms = pit_uptime_ms(); s = ms / 1000; ns = (ms % 1000) * 1000000u; }
    if (t64) { ts[0] = s; ts[1] = 0; ts[2] = ns; ts[3] = 0; }
    else     { ts[0] = s; ts[1] = ns; }
    return 0;
}

/* ---------------- poll / select ---------------- */

typedef struct { int fd; int16_t events, revents; } pollfd_t;

static int poll_once(pollfd_t* fds, uint32_t n) {
    int ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        file_t* f = getf(fds[i].fd);
        if (!f) { fds[i].revents = 0x20; ready++; continue; }        /* POLLNVAL */
        int16_t ev = 0;
        if ((fds[i].events & 0x1) && file_readable(f)) ev |= 0x1;
        if ((fds[i].events & 0x4) && file_writable(f)) ev |= 0x4;
        if (f->type == F_PIPE_R && f->pipe->writers <= 0) ev |= 0x10; /* POLLHUP */
        if (f->type == F_SOCKET && sock_hup(f->sock)) ev |= 0x10;
        if (f->type == F_PIPE_W && f->pipe->readers <= 0) ev |= 0x8;  /* POLLERR */
        fds[i].revents = ev;
        if (ev) ready++;
    }
    return ready;
}

static int do_poll(pollfd_t* fds, uint32_t n, int timeout_ms) {
    if (n > MAX_FDS * 2) return -EINVAL;
    UCHK(fds, n * sizeof(pollfd_t));
    uint32_t start = pit_uptime_ms();
    for (;;) {
        net_poll();
        int r = poll_once(fds, n);
        if (r || timeout_ms == 0) return r;
        if (timeout_ms > 0 && pit_uptime_ms() - start >= (uint32_t)timeout_ms) return 0;
        task_yield();
    }
}

static int do_select(int n, uint32_t* rd, uint32_t* wr, uint32_t* ex, int timeout_ms) {
    if (n < 0 || n > MAX_FDS) n = n < 0 ? -1 : MAX_FDS;
    if (n < 0) return -EINVAL;
    uint32_t words = ((uint32_t)n + 31) / 32;
    if (rd) UCHK(rd, words * 4);
    if (wr) UCHK(wr, words * 4);
    if (ex) UCHK(ex, words * 4);
    uint32_t want_r[2] = {0}, want_w[2] = {0};
    for (uint32_t i = 0; i < words && i < 2; i++) {
        if (rd) want_r[i] = rd[i];
        if (wr) want_w[i] = wr[i];
    }
    uint32_t start = pit_uptime_ms();
    for (;;) {
        net_poll();
        int ready = 0;
        uint32_t got_r[2] = {0}, got_w[2] = {0};
        for (int fd = 0; fd < n; fd++) {
            uint32_t bit = 1u << (fd & 31);
            bool wr_ = want_w[fd >> 5] & bit, rd_ = want_r[fd >> 5] & bit;
            if (!rd_ && !wr_) continue;
            file_t* f = getf(fd);
            if (!f) return -EBADF;
            if (rd_ && file_readable(f)) { got_r[fd >> 5] |= bit; ready++; }
            if (wr_ && file_writable(f)) { got_w[fd >> 5] |= bit; ready++; }
        }
        if (ready || timeout_ms == 0 ||
            (timeout_ms > 0 && pit_uptime_ms() - start >= (uint32_t)timeout_ms)) {
            for (uint32_t i = 0; i < words && i < 2; i++) {
                if (rd) rd[i] = got_r[i];
                if (wr) wr[i] = got_w[i];
                if (ex) ex[i] = 0;
            }
            return ready;
        }
        task_yield();
    }
}

/* ---------------- misc ---------------- */

static int do_uname(char* u) {
    UCHK(u, 65 * 6);
    memset(u, 0, 65 * 6);
    strcpy(u + 0 * 65, "Linux");                 /* what the ABI looks like to userland */
    fs_node_t* hn = fs_resolve(fs_root(), "/etc/hostname");
    const char* host = "samara";
    char tmp[64];
    if (hn && hn->data && hn->size) {
        int k = 0;
        while (k < 63 && k < (int)hn->size && hn->data[k] != '\n') { tmp[k] = hn->data[k]; k++; }
        tmp[k] = 0;
        if (k) host = tmp;
    }
    strcpy(u + 1 * 65, host);
    strcpy(u + 2 * 65, "5.0.0-samara");
    strcpy(u + 3 * 65, "#1 SamaraOS 0.5");
    strcpy(u + 4 * 65, "i686");
    strcpy(u + 5 * 65, "(none)");
    return 0;
}

static int do_getcwd(char* buf, uint32_t size) {
    UCHK(buf, size);
    char path[256];
    fs_path(me()->cwd, path, sizeof(path));
    uint32_t n = strlen(path) + 1;
    if (n > size) return -ERANGE;
    memcpy(buf, path, n);
    return (int)n;
}

static int do_chdir(fs_node_t* n) {
    if (n->type != FS_DIR) return -ENOTDIR;
    me()->cwd = n;
    return 0;
}

static int do_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG_MAX) return -EINVAL;
    if (pid > 0) {
        proc_t* p = proc_by_pid(pid);
        if (!p || p->state != P_ALIVE) return -ESRCH;
        return proc_send_signal(p, sig);
    }
    if (pid == 0) { proc_signal_group(me()->pgid, sig); return 0; }
    if (pid < -1) { proc_signal_group(-pid, sig); return 0; }
    /* -1: everyone but ourselves */
    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (p && p != me() && p->state == P_ALIVE) proc_send_signal(p, sig);
    }
    return 0;
}

static int do_sigprocmask(int how, const uint32_t* set, uint32_t* old, uint32_t size) {
    proc_t* p = me();
    if (size > 8) size = 8;
    if (old) { UCHK(old, size); memcpy(old, &p->sig_mask, size); }
    if (set) {
        UCHK(set, size);
        uint64_t s = 0;
        memcpy(&s, set, size);
        if (how == 0) p->sig_mask |= s;
        else if (how == 1) p->sig_mask &= ~s;
        else if (how == 2) p->sig_mask = s;
        else return -EINVAL;
        p->sig_mask &= ~(SIGBIT(9) | SIGBIT(19));
    }
    return 0;
}

static int do_statfs(uint32_t* b, uint32_t size, fs_node_t* at) {
    UCHK(b, size < 84 ? size : 84);
    memset(b, 0, size < 84 ? size : 84);
    uint32_t cs, tc, fc;
    if (at && fatfs_statfs(fatfs_owner(at), &cs, &tc, &fc)) {
        uint64_t t64 = tc, f64 = fc;
        b[0] = 0x4d44;                                  /* MSDOS_SUPER_MAGIC */
        b[1] = cs;
        memcpy(&b[2], &t64, 8); memcpy(&b[4], &f64, 8); memcpy(&b[6], &f64, 8);
        b[14] = 255; b[15] = cs;
        return 0;
    }
    b[0] = 0x858458f6;                                  /* RAMFS_MAGIC */
    b[1] = 4096;
    uint64_t tot = pmm_total_frames(), fr = pmm_free_frames();
    memcpy(&b[2], &tot, 8);
    memcpy(&b[4], &fr, 8);
    memcpy(&b[6], &fr, 8);
    b[14] = 255;                                        /* f_namelen (after 5 u64 + fsid) */
    b[15] = 4096;                                       /* f_frsize */
    return 0;
}

static int do_sysinfo(uint32_t* s) {
    UCHK(s, 64);
    memset(s, 0, 64);
    s[0] = pit_uptime_ms() / 1000;
    s[4] = heap_total() + pmm_total_frames() * PAGE_SIZE;        /* totalram */
    s[5] = (heap_total() - heap_used()) + pmm_free_frames() * PAGE_SIZE;
    int n = 0;
    for (int i = 0; i < proc_count(); i++) if (proc_at(i)) n++;
    ((uint16_t*)s)[20] = (uint16_t)n;                             /* procs */
    s[13] = 1;                                                    /* mem_unit */
    return 0;
}

static int do_rlimit(int res, uint32_t* old) {
    if (!old) return 0;
    UCHK(old, 8);
    old[0] = old[1] = 0xFFFFFFFFu;
    if (res == 3) old[0] = old[1] = USER_STACK_MAX;              /* RLIMIT_STACK */
    if (res == 7) old[0] = old[1] = MAX_FDS;                     /* RLIMIT_NOFILE */
    return 0;
}

static int do_wait(int pid, int* status, int options) {
    if (status) UCHK(status, 4);
    int st = 0;
    int r = proc_wait(pid, &st, options);
    if (r > 0 && status) *status = st;
    return r;
}

/* ---------------- sockets ---------------- */

#define AF_INET 2
typedef struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } sockaddr_in_t;

static inline uint16_t nbo16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t nbo32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static sock_t* getsock(int fd, int* err) {
    file_t* f = getf(fd);
    if (!f) { *err = -EBADF; return NULL; }
    if (f->type != F_SOCKET) { *err = -88; return NULL; }            /* ENOTSOCK */
    return f->sock;
}

static int read_addr(uint32_t uaddr, uint32_t len, uint32_t* ip, uint16_t* port) {
    if (len < 8) return -EINVAL;
    UCHK((void*)uaddr, 8);
    sockaddr_in_t* sa = (sockaddr_in_t*)uaddr;
    if (sa->family != AF_INET) return -97;                            /* EAFNOSUPPORT */
    *ip = nbo32(sa->addr);
    *port = nbo16(sa->port);
    return 0;
}

static int write_addr(uint32_t uaddr, uint32_t ulen, uint32_t ip, uint16_t port) {
    if (!uaddr || !ulen) return 0;
    UCHK((void*)ulen, 4);
    uint32_t cap = *(uint32_t*)ulen;
    sockaddr_in_t sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_INET;
    sa.port = nbo16(port);
    sa.addr = nbo32(ip);
    uint32_t n = cap < sizeof(sa) ? cap : sizeof(sa);
    UCHK((void*)uaddr, n);
    memcpy((void*)uaddr, &sa, n);
    *(uint32_t*)ulen = sizeof(sa);
    return 0;
}

static int32_t sys_socket_call(int call, uint32_t a, uint32_t b, uint32_t c,
                               uint32_t d, uint32_t e, uint32_t f6) {
    int err = 0;
    sock_t* s;
    uint32_t ip; uint16_t port;
    file_t* fl;
    switch (call) {
        case 1: {                                                     /* socket */
            if (a != AF_INET) return -97;
            int type = (int)(b & 0xF);
            if (c && !((type == 1 && c == 6) || (type == 2 && c == 17))) return -93;  /* EPROTONOSUPPORT */
            s = sock_create(type, &err);
            if (!s) return err ? err : -93;
            fl = file_new(F_SOCKET, 2 | ((b & 04000) ? O_NONBLOCK : 0));
            if (!fl) { sock_close(s); return -ENOMEM; }
            fl->sock = s;
            return install_fd(fl, 0, (b & 02000000) != 0);
        }
        case 2:                                                       /* bind */
            if (!(s = getsock((int)a, &err))) return err;
            if ((err = read_addr(b, c, &ip, &port)) < 0) return err;
            return sock_bind(s, ip, port);
        case 3:                                                       /* connect */
            if (!(s = getsock((int)a, &err))) return err;
            if ((err = read_addr(b, c, &ip, &port)) < 0) return err;
            return sock_connect(s, ip, port, (getf((int)a)->flags & O_NONBLOCK) != 0);
        case 4:                                                       /* listen */
            if (!(s = getsock((int)a, &err))) return err;
            return sock_listen(s, (int)b);
        case 5: case 18: {                                            /* accept(4) */
            if (!(s = getsock((int)a, &err))) return err;
            sock_t* ns = sock_accept(s, (getf((int)a)->flags & O_NONBLOCK) != 0, &err, &ip, &port);
            if (!ns) return err;
            fl = file_new(F_SOCKET, 2 | ((call == 18 && (d & 04000)) ? O_NONBLOCK : 0));
            if (!fl) { sock_close(ns); return -ENOMEM; }
            fl->sock = ns;
            write_addr(b, c, ip, port);
            return install_fd(fl, 0, call == 18 && (d & 02000000));
        }
        case 6: case 7:                                               /* getsockname / getpeername */
            if (!(s = getsock((int)a, &err))) return err;
            sock_name(s, call == 7, &ip, &port);
            if (call == 7 && !port) return -107;                      /* ENOTCONN */
            return write_addr(b, c, ip, port);
        case 8: return -95;                                           /* socketpair: AF_UNIX only */
        case 9: case 11: {                                            /* send / sendto */
            if (!(s = getsock((int)a, &err))) return err;
            UCHK((void*)b, c);
            bool nb = (getf((int)a)->flags & O_NONBLOCK) || (d & 0x40);
            if (call == 11 && e) {
                if ((err = read_addr(e, f6, &ip, &port)) < 0) return err;
                return sock_send(s, (const uint8_t*)b, c, nb, &ip, &port);
            }
            return sock_send(s, (const uint8_t*)b, c, nb, 0, 0);
        }
        case 10: case 12: {                                           /* recv / recvfrom */
            if (!(s = getsock((int)a, &err))) return err;
            UCHK((void*)b, c);
            bool nb = (getf((int)a)->flags & O_NONBLOCK) || (d & 0x40);
            int r = sock_recv(s, (uint8_t*)b, c, nb, (d & 2) != 0, &ip, &port);
            if (r >= 0 && call == 12) write_addr(e, f6, ip, port);
            return r;
        }
        case 13:                                                      /* shutdown */
            if (!(s = getsock((int)a, &err))) return err;
            return sock_shutdown(s, (int)b);
        case 14: return getsock((int)a, &err) ? 0 : err;             /* setsockopt: accepted */
        case 15: {                                                    /* getsockopt */
            if (!(s = getsock((int)a, &err))) return err;
            if (!d || !e) return -EFAULT;
            UCHK((void*)e, 4);
            UCHK((void*)d, 4);
            int v = 0;
            if (b == 1 && c == 4) v = sock_take_error(s);             /* SO_ERROR */
            else if (b == 1 && c == 3) v = sock_type(s);              /* SO_TYPE */
            else if (b == 1 && (c == 7 || c == 8)) v = 65536;         /* SO_SNDBUF/RCVBUF */
            *(int*)d = v;
            *(uint32_t*)e = 4;
            return 0;
        }
        case 16: case 17: {                                           /* sendmsg / recvmsg */
            if (!(s = getsock((int)a, &err))) return err;
            UCHK((void*)b, 28);
            uint32_t* m = (uint32_t*)b;              /* name, namelen, iov, iovlen, ctl, ctllen, flags */
            iovec_t* iov = (iovec_t*)m[2];
            UCHK(iov, m[3] * sizeof(iovec_t));
            bool nb = (getf((int)a)->flags & O_NONBLOCK) || (c & 0x40);
            int total = 0;
            if (call == 16 && m[0]) { if ((err = read_addr(m[0], m[1], &ip, &port)) < 0) return err; }
            for (uint32_t i = 0; i < m[3]; i++) {
                if (!iov[i].len) continue;
                UCHK((void*)iov[i].base, iov[i].len);
                int r = call == 16
                    ? sock_send(s, (const uint8_t*)iov[i].base, iov[i].len, nb, m[0] ? &ip : 0, m[0] ? &port : 0)
                    : sock_recv(s, (uint8_t*)iov[i].base, iov[i].len, nb || total > 0, false, &ip, &port);
                if (r < 0) { if (total) break; return r; }
                total += r;
                if ((uint32_t)r < iov[i].len || sock_type(s) == 2) break;
            }
            if (call == 17) {
                if (m[0]) { uint32_t len = m[1]; write_addr(m[0], (uint32_t)&len, ip, port); m[1] = len; }
                m[5] = 0; m[6] = 0;
            }
            return total;
        }
    }
    return -EINVAL;
}

/* ---------------- dispatcher ---------------- */

static int32_t dispatch(regs_t* r) {
    uint32_t a = r->ebx, b = r->ecx, c = r->edx, d = r->esi, e = r->edi, f6 = r->ebp;
    proc_t* p = me();
    int err;
    fs_node_t* n;
    switch (r->eax) {
        case 1: case 252: proc_exit((int)((a & 0xFF) << 8));
        case 2: case 190: return proc_fork(r);
        case 120: {                                                  /* clone */
            int pid = proc_fork(r);
            (void)d; (void)e;
            if (pid > 0 && b) {
                /* The child starts on the caller-provided stack. */
                proc_t* ch = proc_by_pid(pid);
                task_t* t = ch ? task_at(ch->task) : NULL;
                if (t) ((regs_t*)t->esp)->useresp = b;
            }
            return pid;
        }
        case 3:   return do_read((int)a, (char*)b, c);
        case 4:   return do_write((int)a, (const char*)b, c);
        case 145: return do_rwv((int)a, (iovec_t*)b, (int)c, false);
        case 146: return do_rwv((int)a, (iovec_t*)b, (int)c, true);
        case 180: case 181: {                                        /* pread64/pwrite64 */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -ESPIPE;
            uint32_t save = fl->off;
            fl->off = d;
            int res = r->eax == 180 ? do_read((int)a, (char*)b, c) : do_write((int)a, (const char*)b, c);
            fl->off = save;
            return res;
        }
        case 5:   return do_open(AT_FDCWD, (const char*)a, (int)b, (int)c);
        case 8:   return do_open(AT_FDCWD, (const char*)a, O_CREAT | O_WRONLY | O_TRUNC, (int)b);
        case 295: return do_open((int)a, (const char*)b, (int)c, (int)d);
        case 6: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            p->fds[a] = NULL;
            file_close(fl);
            return 0;
        }
        case 7:   return do_wait((int)a, (int*)b, (int)c);
        case 114: return do_wait((int)a, (int*)b, (int)c);
        case 9: case 83: case 304: case 14: case 297: return -EPERM;   /* link/symlink/mknod */
        case 10:  return do_unlink(AT_FDCWD, (const char*)a, 0);
        case 301: return do_unlink((int)a, (const char*)b, (int)c);
        case 40:  return do_unlink(AT_FDCWD, (const char*)a, AT_REMOVEDIR);
        case 11: {
            UCHK((void*)a, 1);
            if (b) UCHK((void*)b, 4);
            if (c) UCHK((void*)c, 4);
            return proc_execve(r, (const char*)a, (char* const*)b, (char* const*)c);
        }
        case 12:
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            return n ? do_chdir(n) : err;
        case 133: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -ENOTDIR;
            return do_chdir(fl->node);
        }
        case 13: {
            uint32_t t = clock_epoch();
            if (a) { UCHK((void*)a, 4); *(uint32_t*)a = t; }
            return (int32_t)t;
        }
        case 15: case 306:
            UCHK((void*)(r->eax == 15 ? a : b), 1);
            n = r->eax == 15 ? lookup(AT_FDCWD, (const char*)a, &err) : lookup((int)a, (const char*)b, &err);
            if (!n) return err;
            n->mode = (uint16_t)((r->eax == 15 ? b : c) & 07777);
            return 0;
        case 94: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type == F_NODE) fl->node->mode = (uint16_t)(b & 07777);
            return 0;
        }
        case 186: if (b) { UCHK((void*)b, 12); memset((void*)b, 0, 12); ((uint32_t*)b)[1] = 2; } return 0;  /* sigaltstack: SS_DISABLE */
        case 36: case 118: case 148: case 344:                       /* sync, fsync, fdatasync, syncfs */
            return fatfs_sync_all();
        case 21: {                                                   /* mount */
            UCHK((void*)b, 1);
            if (d & 32) return 0;                                    /* MS_REMOUNT */
            if (c) {
                UCHK((void*)c, 1);
                const char* t = (const char*)c;
                if (!strcmp(t, "proc") || !strcmp(t, "ramfs") || !strcmp(t, "tmpfs") ||
                    !strcmp(t, "sysfs") || !strcmp(t, "devtmpfs")) return 0;
                if (strcmp(t, "vfat") && strcmp(t, "msdos") && strcmp(t, "fat")) return -19;  /* ENODEV */
            }
            UCHK((void*)a, 1);
            fs_node_t* src = lookup(AT_FDCWD, (const char*)a, &err);
            if (!src) return err;
            if (src->dev < FS_DEV_DISK) return -15;                  /* ENOTBLK */
            fs_node_t* dst = lookup(AT_FDCWD, (const char*)b, &err);
            if (!dst) return err;
            return fatfs_mount(src->dev - FS_DEV_DISK, dst);
        }
        case 22: case 52: {                                          /* umount / umount2 */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            if (n->dev >= FS_DEV_DISK) return -EINVAL;              /* give the mount point */
            for (int i = 0; i < proc_count(); i++) {
                proc_t* q = proc_at(i);
                for (fs_node_t* w = q ? q->cwd : NULL; w; w = w->parent)
                    if (w == n) return -EBUSY;
            }
            return fatfs_umount(n);
        }
        case 16: case 182: case 95: case 198: case 207: case 212: case 298:
        case 136: case 172: case 311: case 96: case 97:
        case 23: case 46: case 213: case 214: case 203: case 204: case 208: case 210:
        case 81: case 206:
            return 0;                                                /* chown, sync, uid/gid, ... */
        case 19:  return (int32_t)do_lseek((int)a, (int32_t)b, (int)c);
        case 140: {                                                  /* _llseek */
            int64_t res = do_lseek((int)a, (int64_t)(((uint64_t)b << 32) | c), (int)e);
            if (res < 0) return (int32_t)res;
            UCHK((void*)d, 8);
            memcpy((void*)d, &res, 8);
            return 0;
        }
        case 20:  return p->pid;
        case 224: return p->pid;
        case 64:  return p->ppid ? p->ppid : 1;
        case 24: case 47: case 49: case 50: case 199: case 200: case 201: case 202: return 0;
        case 29:                                                     /* pause */
            while (!proc_interrupted()) task_sleep_ms(10);
            return -EINTR;
        case 33:  return do_access(AT_FDCWD, (const char*)a);
        case 307: case 439: return do_access((int)a, (const char*)b);
        case 37:  return do_kill((int)a, (int)b);
        case 238: case 270: {                                        /* tkill / tgkill */
            int pid = r->eax == 238 ? (int)a : (int)b, sig = r->eax == 238 ? (int)b : (int)c;
            proc_t* t = proc_by_pid(pid);
            return t ? proc_send_signal(t, sig) : -ESRCH;
        }
        case 38:  return do_rename(AT_FDCWD, (const char*)a, AT_FDCWD, (const char*)b);
        case 302: case 353: return do_rename((int)a, (const char*)b, (int)c, (const char*)d);
        case 39:  return do_mkdir(AT_FDCWD, (const char*)a, (int)b);
        case 296: return do_mkdir((int)a, (const char*)b, (int)c);
        case 41:  return do_dup((int)a, 0, false);
        case 63:  return do_dup2((int)a, (int)b, false);
        case 330: return (a == b) ? -EINVAL : do_dup2((int)a, (int)b, (c & O_CLOEXEC) != 0);
        case 42:  return do_pipe((int*)a, 0);
        case 331: return do_pipe((int*)a, (int)b);
        case 43: {
            if (a) { UCHK((void*)a, 16); memset((void*)a, 0, 16); }
            return (int32_t)(pit_uptime_ms() / 10);
        }
        case 45:  return (int32_t)do_brk(a);
        case 54:  return do_ioctl((int)a, b, c);
        case 55: case 221: return do_fcntl((int)a, (int)b, c);
        case 57: {                                                   /* setpgid */
            proc_t* t = a ? proc_by_pid((int)a) : p;
            if (!t) return -ESRCH;
            t->pgid = b ? (int)b : t->pid;
            return 0;
        }
        case 65:  return p->pgid;
        case 132: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->pgid : -ESRCH; }
        case 147: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->sid : -ESRCH; }
        case 66:  p->sid = p->pgid = p->pid; return p->pid;
        case 60:  { int old = p->umask; p->umask = (int)(a & 0777); return old; }
        case 174: case 67:                                           /* rt_sigaction / sigaction */
            if (b) UCHK((void*)b, 20);
            if (c) UCHK((void*)c, 20);
            return proc_sigaction((int)a, (const uint32_t*)b, (uint32_t*)c, r->eax == 67);
        case 27: {                                                   /* alarm */
            uint32_t left = p->alarm_at ? (p->alarm_at - pit_uptime_ms() + 999) / 1000 : 0;
            p->alarm_at = a ? pit_uptime_ms() + a * 1000 : 0;
            p->alarm_interval = 0;
            return (int32_t)left;
        }
        case 104: case 105: {                                        /* setitimer / getitimer */
            if ((int)a != 0) return -EINVAL;                         /* ITIMER_REAL only */
            uint32_t* oldv = (uint32_t*)(r->eax == 104 ? c : b);
            if (oldv) {
                UCHK(oldv, 16);
                uint32_t left = p->alarm_at ? p->alarm_at - pit_uptime_ms() : 0;
                oldv[0] = p->alarm_interval / 1000; oldv[1] = p->alarm_interval % 1000 * 1000;
                oldv[2] = left / 1000; oldv[3] = left % 1000 * 1000;
            }
            if (r->eax == 104 && b) {
                UCHK((void*)b, 16);
                uint32_t* nv = (uint32_t*)b;
                uint32_t val = nv[2] * 1000 + nv[3] / 1000, iv = nv[0] * 1000 + nv[1] / 1000;
                if ((nv[2] || nv[3]) && !val) val = 1;
                p->alarm_at = val ? pit_uptime_ms() + val : 0;
                p->alarm_interval = val ? iv : 0;
            }
            return 0;
        }
        case 119: return proc_sigreturn(r, false);
        case 173: return proc_sigreturn(r, true);
        case 175: return do_sigprocmask((int)a, (const uint32_t*)b, (uint32_t*)c, d);
        case 126: return do_sigprocmask((int)a, (const uint32_t*)b, (uint32_t*)c, 4);
        case 176: case 73:                                           /* sigpending */
            if (a) { UCHK((void*)a, 4); uint64_t pd = p->sig_pending & p->sig_mask;
                     memcpy((void*)a, &pd, r->eax == 73 || b < 8 ? 4 : 8); }
            return 0;
        case 179: case 72: {                                         /* (rt_)sigsuspend */
            UCHK((void*)a, 4);
            uint64_t m = 0;
            memcpy(&m, (void*)a, r->eax == 179 && b >= 8 ? 8 : 4);
            return proc_sigsuspend(&m);
        }
        case 77:  if (b) { UCHK((void*)b, 72); memset((void*)b, 0, 72); } return 0;
        case 78: {                                                   /* gettimeofday */
            if (a) {
                UCHK((void*)a, 8);
                uint32_t s, ns; clock_now(&s, &ns);
                ((uint32_t*)a)[0] = s; ((uint32_t*)a)[1] = ns / 1000;
            }
            if (b) { UCHK((void*)b, 8); memset((void*)b, 0, 8); }
            return 0;
        }
        case 85: case 305: return -EINVAL;                           /* readlink: no symlinks */
        case 90: {                                                   /* old mmap(struct*) */
            UCHK((void*)a, 24);
            uint32_t* m = (uint32_t*)a;
            if (m[5] & (PAGE_SIZE - 1)) return -EINVAL;
            return do_mmap(m[0], m[1], (int)m[2], (int)m[3], (int)m[4], m[5]);
        }
        case 192: return do_mmap(a, b, (int)c, (int)d, (int)e, f6 * PAGE_SIZE);
        case 91:  return do_munmap(a, b);
        case 125: return do_mprotect(a, b, (int)c);
        case 163: return -ENOMEM;                                    /* mremap: musl falls back */
        case 92: case 193: {                                         /* truncate(64) */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            return node_truncate(n, b);
        }
        case 93: case 194: {                                         /* ftruncate(64) */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -EINVAL;
            return node_truncate(fl->node, b);
        }
        case 268: {                                                  /* statfs64 */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            return do_statfs((uint32_t*)c, b, n);
        }
        case 269:                                                    /* fstatfs64 */
            if (!getf((int)a)) return -EBADF;
            return do_statfs((uint32_t*)c, b, getf((int)a)->type == F_NODE ? getf((int)a)->node : NULL);
        case 116: return do_sysinfo((uint32_t*)a);
        case 122: return do_uname((char*)a);
        case 142: {                                                  /* _newselect */
            int to = -1;
            if (e) { UCHK((void*)e, 8); to = (int)(((uint32_t*)e)[0] * 1000 + ((uint32_t*)e)[1] / 1000); }
            return do_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)d, to);
        }
        case 82: {                                                   /* old select(struct*) */
            UCHK((void*)a, 20);
            uint32_t* s = (uint32_t*)a;
            int to = -1;
            if (s[4]) { UCHK((void*)s[4], 8); to = (int)(((uint32_t*)s[4])[0] * 1000 + ((uint32_t*)s[4])[1] / 1000); }
            return do_select((int)s[0], (uint32_t*)s[1], (uint32_t*)s[2], (uint32_t*)s[3], to);
        }
        case 308: {                                                  /* pselect6 */
            int to = -1;
            if (e) { UCHK((void*)e, 8); to = (int)(((uint32_t*)e)[0] * 1000 + ((uint32_t*)e)[1] / 1000000); }
            return do_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)d, to);
        }
        case 168: return do_poll((pollfd_t*)a, b, (int)c);
        case 309: {                                                  /* ppoll */
            int to = -1;
            if (c) { UCHK((void*)c, 8); to = (int)(((uint32_t*)c)[0] * 1000 + ((uint32_t*)c)[1] / 1000000); }
            return do_poll((pollfd_t*)a, b, to);
        }
        case 158: task_yield(); return 0;
        case SYS_SAMARA: return uwin_syscall(a, b, c, d);           /* desktop windows */
        case 162: case 267: {                                        /* nanosleep / clock_nanosleep */
            const uint32_t* ts = (const uint32_t*)(r->eax == 162 ? a : c);
            UCHK(ts, 8);
            if (r->eax == 267 && (b & 1)) {                          /* TIMER_ABSTIME */
                uint32_t s, ns; clock_now(&s, &ns);
                return ts[0] > s ? sleep_ms((ts[0] - s) * 1000) : 0;
            }
            return sleep_ms(ts[0] * 1000 + ts[1] / 1000000);
        }
        case 183: return do_getcwd((char*)a, b);
        case 191: case 76: return do_rlimit((int)a, (uint32_t*)b);
        case 75: return 0;
        case 340: {                                                  /* prlimit64 */
            if (d) {
                UCHK((void*)d, 16);
                uint32_t lim[2];
                do_rlimit((int)b, lim);
                uint32_t* o = (uint32_t*)d;
                o[0] = lim[0]; o[1] = lim[0] == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0;
                o[2] = lim[1]; o[3] = lim[1] == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0;
            }
            return 0;
        }
        case 195: case 196:                                          /* stat64 / lstat64 */
            UCHK((void*)a, 1); UCHK((void*)b, sizeof(kstat64_t));
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            fill_stat_node((kstat64_t*)b, n);
            return 0;
        case 197: {                                                  /* fstat64 */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            UCHK((void*)b, sizeof(kstat64_t));
            fill_stat_file((kstat64_t*)b, fl);
            return 0;
        }
        case 300: {                                                  /* fstatat64 */
            UCHK((void*)b, 1); UCHK((void*)c, sizeof(kstat64_t));
            if (((const char*)b)[0] == 0 && (d & 0x1000)) {         /* AT_EMPTY_PATH */
                file_t* fl = getf((int)a);
                if (!fl) return -EBADF;
                fill_stat_file((kstat64_t*)c, fl);
                return 0;
            }
            n = lookup((int)a, (const char*)b, &err);
            if (!n) return err;
            fill_stat_node((kstat64_t*)c, n);
            return 0;
        }
        case 220: return do_getdents64((int)a, (uint8_t*)b, c);
        case 240: return 0;                                          /* futex: single-threaded */
        case 219: return 0;                                          /* madvise: advisory */
        case 243: {                                                  /* set_thread_area */
            UCHK((void*)a, 16);
            uint32_t* ud = (uint32_t*)a;
            if (ud[0] != 0xFFFFFFFFu && ud[0] != GDT_TLS_INDEX) return -EINVAL;
            ud[0] = GDT_TLS_INDEX;
            p->tls_base = ud[1];
            gdt_set_tls(p->tls_base);
            return 0;
        }
        case 244: {
            UCHK((void*)a, 16);
            uint32_t* ud = (uint32_t*)a;
            ud[0] = GDT_TLS_INDEX; ud[1] = p->tls_base; ud[2] = 0xFFFFF; ud[3] = 0x51;
            return 0;
        }
        case 258: p->clear_child_tid = a; return p->pid;
        case 265: return do_clock_gettime((int)a, (uint32_t*)b, false);
        case 403: return do_clock_gettime((int)a, (uint32_t*)b, true);
        case 266: case 406:
            if (b) { UCHK((void*)b, r->eax == 406 ? 16 : 8); memset((void*)b, 0, r->eax == 406 ? 16 : 8);
                     ((uint32_t*)b)[r->eax == 406 ? 2 : 1] = 1000000; }
            return 0;
        case 30: case 271: case 320: case 412: {                     /* utime(s)/utimensat */
            const char* path = (const char*)(r->eax == 320 || r->eax == 412 ? b : a);
            int dfd = (r->eax == 320 || r->eax == 412) ? (int)a : AT_FDCWD;
            if (!path) { file_t* fl = getf(dfd); if (fl && fl->type == F_NODE) fl->node->mtime = fs_now(); return 0; }
            UCHK(path, 1);
            n = lookup(dfd, path, &err);
            if (!n) return err;
            n->mtime = fs_now();
            return 0;
        }
        case 355: {                                                  /* getrandom */
            UCHK((void*)a, b);
            file_t tmp = { .type = F_RANDOM };
            return file_read(&tmp, (char*)a, b);
        }
        case 102: {                                                  /* socketcall */
            static const uint8_t nargs[19] = { 0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3, 4 };
            if (a < 1 || a > 18) return -EINVAL;
            UCHK((void*)b, nargs[a] * 4u);
            uint32_t* v = (uint32_t*)b;
            return sys_socket_call((int)a, v[0], nargs[a] > 1 ? v[1] : 0, nargs[a] > 2 ? v[2] : 0,
                                   nargs[a] > 3 ? v[3] : 0, nargs[a] > 4 ? v[4] : 0, nargs[a] > 5 ? v[5] : 0);
        }
        case 359: return sys_socket_call(1, a, b, c, 0, 0, 0);        /* socket */
        case 360: return sys_socket_call(8, a, b, c, d, 0, 0);        /* socketpair */
        case 361: return sys_socket_call(2, a, b, c, 0, 0, 0);        /* bind */
        case 362: return sys_socket_call(3, a, b, c, 0, 0, 0);        /* connect */
        case 363: return sys_socket_call(4, a, b, 0, 0, 0, 0);        /* listen */
        case 364: return sys_socket_call(18, a, b, c, d, 0, 0);       /* accept4 */
        case 365: return sys_socket_call(15, a, b, c, d, e, 0);       /* getsockopt */
        case 366: return sys_socket_call(14, a, b, c, d, e, 0);       /* setsockopt */
        case 367: return sys_socket_call(6, a, b, c, 0, 0, 0);        /* getsockname */
        case 368: return sys_socket_call(7, a, b, c, 0, 0, 0);        /* getpeername */
        case 369: return sys_socket_call(11, a, b, c, d, e, f6);      /* sendto */
        case 370: return sys_socket_call(16, a, b, c, 0, 0, 0);       /* sendmsg */
        case 371: return sys_socket_call(12, a, b, c, d, e, f6);      /* recvfrom */
        case 372: return sys_socket_call(17, a, b, c, 0, 0, 0);       /* recvmsg */
        case 373: return sys_socket_call(13, a, b, 0, 0, 0, 0);       /* shutdown */
        case 187: case 239: return -EINVAL;                          /* sendfile: use read/write */
        case 383: return -ENOSYS;                                    /* statx: musl falls back */
        case 88: case 74: case 124: return -EPERM;
    }
    klog("[sys] pid ");
    klog_num(p->pid);
    klog(" ("); klog(p->name); klog(") unimplemented syscall ");
    klog_num((int32_t)r->eax);
    klog("\r\n");
    return -ENOSYS;
}

void syscall_dispatch(regs_t* r) {
    uint32_t nr = r->eax;
    proc_check_alarm(proc_current(), false);
    int32_t ret = dispatch(r);
    if (g_strace) {
        proc_t* p = proc_current();
        klog("[strace] "); klog_num(p ? p->pid : 0);
        klog(" "); klog_num((int32_t)nr);
        klog("("); klog_num((int32_t)r->ebx); klog(", "); klog_num((int32_t)r->ecx);
        klog(", "); klog_num((int32_t)r->edx); klog(") = "); klog_num(ret); klog("\r\n");
    }
    /* execve and sigreturn have already installed the registers to return with. */
    bool keep = (nr == 11 && ret >= 0) || ((nr == 119 || nr == 173) && ret == 0);
    if (!keep) r->eax = (uint32_t)ret;
    proc_deliver_signal(r, keep ? -1 : (int)nr, keep ? 0 : ret);
}

__attribute__((naked))
static void syscall_isr(void) {
    __asm__ volatile (
        "pusha\n"
        "push %ds\n push %es\n push %fs\n push %gs\n"
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n mov %ax, %es\n"
        "push %esp\n"
        "call syscall_dispatch\n"
        "add $4, %esp\n"
        "pop %gs\n pop %fs\n pop %es\n pop %ds\n"
        "popa\n"
        "iret\n"
    );
}

void syscall_init(void) {
    /* DPL 3 interrupt gate: callable from ring 3, IF cleared on entry. */
    idt_set_gate(0x80, syscall_isr, 0x08, 0xEE);
}
