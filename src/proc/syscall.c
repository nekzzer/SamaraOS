/* Linux x86_64 system call ABI (syscall insn): rax = number, args in rdi,
   rsi, rdx, r10, r8, r9; result (or -errno) back in rax. Only what static
   musl binaries such as busybox actually need is implemented; everything
   else answers -ENOSYS, which musl and busybox handle gracefully. */

#include "gui/uwin.h"
#include "proc/proc.h"
#include "proc/pty.h"
#include "proc/file.h"
#include "drivers/fbdev.h"
#include "drivers/input.h"
#include "gfx/gfx.h"
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
#include "fs/ext2.h"
#include "net/net.h"
#include "drivers/drm.h"
#include "drivers/snd.h"
#include "proc/uring.h"
#include "core/prof.h"

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define ENXIO 6
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define EMLINK 31
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
int  g_strace_pid;
bool g_ftrace;                         /* "ftrace": failed syscalls with the path, to COM1 */                      /* only this pid ("strace=N" on the cmdline), 0 = all */

/* ---------------- serial ---------------- */

extern int setjmp(void* env);
static void com_putc(char c) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, c); }
static void klog(const char* s) { while (*s) com_putc(*s++); }
static void klog_num(int64_t v) { char b[24]; itoa((int)v, b, 10); klog(b); }

/* ---------------- user memory checks ---------------- */

static bool uok(const void* p, uint64_t len) {
    uint64_t a = (uint64_t)p;
    if (a < USER_BASE || a >= USER_TOP || len > USER_TOP - a) return false;
    /* pages have to be there too, otherwise teh kernel faults on them.
       the stack is grown lazily by the fault handler so skip it */
    uint64_t pd = proc_current()->pd;
    for (uint64_t pg = a & ~(PAGE_SIZE - 1); pg < a + len; pg += PAGE_SIZE) {
        if (pg >= USER_STACK_TOP - USER_STACK_MAX) break;
        uint64_t pte = vmm_pte(pd, pg);
        if (!(pte & PTE_P) && !((pte & PTE_LAZY) && (pte & PTE_US))) return false;   /* lazy: the fault fills it */
    }
    return true;
}
bool user_ok(const void* p, uint32_t len) { return uok(p, len); }
#define UCHK2(p, n) do { if (!uok((p), (n))) { proc_current()->ujb_on = false; return -EFAULT; } } while (0)
#define UCHK(p, n) do { if (!uok((p), (n))) return -EFAULT; } while (0)

/* user string: walk it page by page till the NUL */
static bool ustr_ok(const char* s) {
    uint64_t a = (uint64_t)s;
    for (;;) {
        if (!uok((const void*)a, 1)) return false;
        uint64_t end = (a | (PAGE_SIZE - 1)) + 1;
        for (; a < end; a++) if (!*(const char*)a) return true;
    }
}

/* ---------------- fds + paths ---------------- */

static proc_t* me(void) { return proc_current(); }

/* slot changes (close, dup2) and the nobkl lookups, so a file can't die between
   the lookup and the ref */
static spin_t fdl;

static file_t* getf_ref(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    uint64_t fl = spin_lock(&fdl);
    file_t* f = proc_current()->sh->fds[fd];
    file_ref(f);
    spin_unlock(&fdl, fl);
    return f;
}

static file_t* fd_swap(int fd, file_t* nf) {
    uint64_t fl = spin_lock(&fdl);
    file_t* old = me()->sh->fds[fd];
    me()->sh->fds[fd] = nf;
    spin_unlock(&fdl, fl);
    return old;
}

static file_t* getf(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return me()->sh->fds[fd];
}

static int alloc_fd(int from) {
    for (int i = from < 0 ? 0 : from; i < MAX_FDS; i++)
        if (!me()->sh->fds[i]) return i;
    return -EMFILE;
}

static int install_fd(file_t* f, int from, bool cloexec) {
    uint64_t fl = irq_save();                /* threads share the table */
    int fd = alloc_fd(from);
    if (fd >= 0) {
        me()->sh->fds[fd] = f;
        me()->sh->cloexec[fd] = cloexec;
    }
    irq_restore(fl);
    if (fd < 0) file_close(f);
    return fd;
}

static fs_node_t* dir_base(int dirfd, const char* path, int* err) {
    if (path[0] == '/' || dirfd == AT_FDCWD) return me()->sh->cwd;
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

static fs_node_t* lookup_ex(int dirfd, const char* path, int* err, bool follow) {
    *err = -ENOENT;
    if (!ustr_ok(path)) { *err = -EFAULT; return NULL; }
    if (!path[0]) return NULL;
    fs_node_t* base = dir_base(dirfd, path, err);
    if (!base) return NULL;
    maybe_refresh_proc(base, path);
    fs_node_t* n = follow ? fs_resolve(base, path) : fs_resolve_nf(base, path);
    if (!n) *err = -ENOENT;
    return n;
}
static fs_node_t* lookup(int dirfd, const char* path, int* err) { return lookup_ex(dirfd, path, err, true); }
/* no reading lazy ext2 files in: stat & co only want the node */
static fs_node_t* lookup_peek(int dirfd, const char* path, int* err, bool follow) {
    *err = -ENOENT;
    if (!ustr_ok(path)) { *err = -EFAULT; return NULL; }
    if (!path[0]) return NULL;
    fs_node_t* base = dir_base(dirfd, path, err);
    if (!base) return NULL;
    maybe_refresh_proc(base, path);
    fs_node_t* n = fs_peek(base, path, follow);
    if (!n) *err = -ENOENT;
    return n;
}

/* the name itself: a hard link name is not followed to its node */
static fs_node_t* lookup_dent(int dirfd, const char* path, int* err) {
    *err = -ENOENT;
    if (!ustr_ok(path)) { *err = -EFAULT; return NULL; }
    if (!path[0]) return NULL;
    fs_node_t* base = dir_base(dirfd, path, err);
    if (!base) return NULL;
    maybe_refresh_proc(base, path);
    fs_node_t* n = fs_peek_d(base, path);
    if (!n) *err = -ENOENT;
    return n;
}

/* Split "a/b/c/" into parent node of "c" and the name "c". */
static fs_node_t* lookup_parent(int dirfd, const char* path, char* name, int* err) {
    char tmp[256];
    if (!ustr_ok(path)) { *err = -EFAULT; return NULL; }
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
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t  st_size, st_blksize, st_blocks;
    int64_t  st_atime, st_atime_ns, st_mtime, st_mtime_ns, st_ctime, st_ctime_ns;
    int64_t  res[3];
} kstat64_t;

static void fill_stat_node(kstat64_t* st, fs_node_t* n) {
    memset(st, 0, sizeof(*st));
    int vol = fatfs_owner(n);
    st->st_dev = vol ? (8u << 8) | (uint32_t)vol : 1;            /* distinct per volume (df) */
    st->st_ino = ((uint64_t)n >> 4) & 0x0FFFFFFF;
    st->st_nlink = n->type == FS_DIR ? 2 : n->unlinked ? 0 : 1 + n->xl;
    if (FS_DEV_IS_DISK(n->dev)) {
        int idx = n->dev - FS_DEV_DISK;
        st->st_mode = 0060000 | (n->mode & 07777);            /* S_IFBLK */
        st->st_rdev = idx >= DISK_VIRTIO_BASE ? ((253u << 8) | (uint32_t)(idx - DISK_VIRTIO_BASE) * 16) :
                      idx >= DISK_AHCI_BASE ? ((8u << 8) | (uint32_t)(idx - DISK_AHCI_BASE) * 16)
                                            : ((3u << 8) | (uint32_t)idx * 64);
        st->st_size = (int64_t)ata_drive_sectors(idx) * 512;
    } else if (n->dev == FS_DEV_SOCK) {
        st->st_mode = 0140000 | (n->mode & 07777);                 /* S_IFSOCK */
    } else if (n->dev) {
        st->st_mode = S_IFCHR | (n->mode & 07777);
        st->st_rdev = n->dev == FS_DEV_TTY  ? (5u << 8) :
                      n->dev == FS_DEV_PTMX ? ((5u << 8) | 2) :
                      n->dev == FS_DEV_SNDC ? (116u << 8) : n->dev == FS_DEV_SNDP ? ((116u << 8) | 16) :
                      n->dev == FS_DEV_DRM ? (226u << 8) : n->dev == FS_DEV_DRMR ? ((226u << 8) | 128) :
                      FS_DEV_IS_PTS(n->dev) ? ((136u << 8) | (uint32_t)(n->dev - FS_DEV_PTS)) :
                                              ((1u << 8) | n->dev);
    } else if (n->type == FS_DIR) {
        st->st_mode = S_IFDIR | (n->mode & 07777);
        st->st_size = 4096;
    } else if (n->type == FS_LINK) {
        st->st_mode = 0120000 | 0777;                            /* S_IFLNK */
        st->st_size = n->size;
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
    /* a pty is the same file as its /dev node (ttyname compares st_ino) */
    fs_node_t* dn = f->type == F_PTS ? pty_node(f->pty) :
                    f->type == F_PTM ? fs_resolve(fs_root(), "/dev/ptmx") : NULL;
    if (dn) { fill_stat_node(st, dn); return; }
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = ((uint64_t)f >> 4) & 0x0FFFFFFF;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (f->type == F_PIPE_R || f->type == F_PIPE_W) st->st_mode = S_IFIFO | 0600;
    else if (f->type == F_SOCKET || f->type == F_SPAIR || f->type == F_NETLINK || f->type == F_USOCK || f->type == F_ULISTEN) st->st_mode = 0140000 | 0777;          /* S_IFSOCK */
    else {
        st->st_mode = S_IFCHR | 0666;
        /* input: major 13, minor 64+n like /dev/input/eventN. evdev compares
           st_rdev and threw the mouse out as a duplicate of the keyboard */
        st->st_rdev = f->type == F_SND ? (116u << 8) | (f->disk ? 16u : 0) : f->type == F_DRM ? (226u << 8) | (f->disk ? 128u : 0) : f->type == F_TTY ? (5u << 8) : f->type == F_INPUT ? (13u << 8) | (64u + (uint32_t)f->disk)
                                                                       : (1u << 8) | 3;
    }
    st->st_atime = st->st_mtime = st->st_ctime = clock_epoch();
}

/* readlink: the ramfs has no symlinks, but /proc/self/fd/N names what an
   fd refers to (musl's ttyname() relies on it; ssh servers use ttyname). */
static int do_readlink(int dfd, const char* path, char* buf, uint64_t n) {
    UCHK(path, 1);
    UCHK(buf, n);
    {
        int err;
        fs_node_t* ln = lookup_peek(dfd, path, &err, false);
        if (ln && ln->type == FS_LINK) {
            uint64_t k = ln->size < n ? (uint64_t)ln->size : n;
            memcpy(buf, ln->data, k);
            return (int)k;
        }
    }
    const char* p = path;
    if (strncmp(p, "/proc/", 6)) return -EINVAL;
    p += 6;
    if (!strncmp(p, "self/", 5)) p += 5;
    else {
        int pid = 0;
        while (*p >= '0' && *p <= '9') pid = pid * 10 + (*p++ - '0');
        if (*p++ != '/' || pid != me()->tgid) return -EINVAL;
    }
    if (!strcmp(p, "exe")) {
        proc_t* lp = proc_by_pid(me()->tgid);
        const char* x = lp && lp->exe[0] ? lp->exe : me()->exe;
        uint64_t k = strlen(x);
        if (k > n) k = n;
        memcpy(buf, x, k);
        return (int)k;
    }
    if (strncmp(p, "fd/", 3)) return -EINVAL;
    p += 3;
    int fd = 0;
    if (*p < '0' || *p > '9') return -ENOENT;
    while (*p >= '0' && *p <= '9') fd = fd * 10 + (*p++ - '0');
    if (*p) return -ENOENT;
    file_t* f = getf(fd);
    if (!f) return -ENOENT;
    char out[160];
    switch (f->type) {
        case F_NODE: fs_path(f->node, out, sizeof(out)); break;
        case F_PTS: {
            fs_node_t* dn = pty_node(f->pty);
            if (dn) fs_path(dn, out, sizeof(out)); else strcpy(out, "/dev/pts/?");
            break;
        }
        case F_PTM:  strcpy(out, "/dev/ptmx"); break;
        case F_TTY:  strcpy(out, "/dev/tty"); break;
        case F_NULL: strcpy(out, "/dev/null"); break;
        case F_ZERO: strcpy(out, "/dev/zero"); break;
        case F_RANDOM: strcpy(out, "/dev/urandom"); break;
        case F_SOCKET: case F_SPAIR: case F_NETLINK: case F_USOCK: case F_ULISTEN: strcpy(out, "socket:[1]"); break;
        default:     strcpy(out, "pipe:[1]"); break;
    }
    uint64_t l = strlen(out);
    if (l > n) l = n;
    memcpy(buf, out, l);
    return (int)l;
}

/* ---------------- file syscalls ---------------- */

static int do_open(int dirfd, const char* path, int flags, int mode) {
    UCHK(path, 1);
    int err;
    fs_node_t* n = lookup(dirfd, path, &err);
    if (n && (flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (n && n->lazy) return -ENOMEM;                 /* ext2 couldn't read it in, no room */
    if (!n) {
        if (!(flags & O_CREAT) || err != -ENOENT) return err;
        char name[FS_NAME_MAX];
        fs_node_t* parent = lookup_parent(dirfd, path, name, &err);
        if (!parent) return err;
        n = fs_create(parent, name, FS_FILE);
        if (!n) return -EACCES;
        n->mode = (uint16_t)(mode & ~me()->sh->umask & 07777);
        ino_node(n, 0x100);
    }
    if ((flags & O_DIRECTORY) && n->type != FS_DIR) return -ENOTDIR;
    if (n->type == FS_DIR && (flags & O_ACCMODE) != 0) return -EISDIR;
    if ((flags & O_TRUNC) && n->type == FS_FILE && !n->dev && (flags & O_ACCMODE)) { uint32_t os = n->size; node_truncate(n, 0); if (os) ino_node(n, 2); }
    file_t* f = file_open_node(n, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
    if (!f) return n->dev == FS_DEV_TTY ? -ENXIO : -ENOMEM;     // xterm dies on ENOMEM here
    ino_node(n, 0x20);
    return install_fd(f, 0, (flags & O_CLOEXEC) != 0);
}

static int do_read(int fd, char* buf, uint64_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    UCHK(buf, n);
    return file_read(f, buf, n);
}

static int do_write(int fd, const char* buf, uint64_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == 0 && f->type == F_NODE) return -EBADF;
    UCHK(buf, n);
    if (f->type == F_NODE && !strcmp(f->node->name, "prof")) { prof_cmd(buf, n); return n; }
    return file_write(f, buf, n);
}

typedef struct { uint64_t base, len; } iovec_t;

static int do_rwv(int fd, iovec_t* iov, int cnt, bool wr) {
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    UCHK(iov, (uint64_t)cnt * sizeof(iovec_t));
    int total = 0;
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        int r = wr ? do_write(fd, (const char*)iov[i].base, iov[i].len)
                   : do_read(fd, (char*)iov[i].base, iov[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].len) break;
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
        f->off = (uint64_t)(base + off);
        return base + off;
    }
    if (f->type != F_NODE && f->type != F_DISK) return f->type == F_NULL || f->type == F_ZERO ? 0 : -ESPIPE;
    uint32_t end = f->type == F_DISK ? file_disk_size(f) : f->node->size;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off :
                   whence == 2 ? (int64_t)end : -1;
    if (base < 0) return -EINVAL;
    int64_t pos = base + off;
    if (pos < 0) return -EINVAL;
    f->off = (uint64_t)pos;
    return pos;
}

/* copy_file_range / sendfile: through a kernel buffer, offsets are optional pointers */
static int64_t do_copy(int ifd, uint64_t* ioff, int ofd, uint64_t* ooff, uint64_t len) {
    file_t* fi = getf(ifd);
    file_t* fo = getf(ofd);
    if (!fi || !fo) return -EBADF;
    if (ioff) UCHK(ioff, 8);
    if (ooff) UCHK(ooff, 8);
    char* kb = kmalloc(65536);
    if (!kb) return -ENOMEM;
    uint64_t si = fi->off, so = fo->off;
    if (ioff) fi->off = *ioff;
    if (ooff) fo->off = *ooff;
    int64_t tot = 0;
    while (len) {
        int n = len > 65536 ? 65536 : (int)len;
        int r = file_read(fi, kb, n);
        if (r <= 0) { if (!tot) tot = r; break; }
        int w = file_write(fo, kb, r);
        if (w <= 0) { if (!tot) tot = w; break; }
        tot += w; len -= w;
        if (w < r) break;
    }
    kfree(kb);
    if (ioff) { *ioff = fi->off; fi->off = si; }
    if (ooff) { *ooff = fo->off; fo->off = so; }
    return tot;
}

static int do_getdents64(int fd, uint8_t* buf, uint64_t n) {
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
            name = c->name; node = c->hl ? c->hl : c; c = c->next;
        }
        if (idx < f->off) continue;
        uint32_t nl = strlen(name);
        uint32_t reclen = (19 + nl + 1 + 7) & ~7u;
        if (pos + reclen > n) { if (pos == 0) return -EINVAL; break; }
        uint8_t* d = buf + pos;
        uint64_t ino = ((uint64_t)node >> 4) & 0x0FFFFFFF;
        int64_t next = idx + 1;
        memcpy(d, &ino, 8);
        memcpy(d + 8, &next, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(d + 16, &rl, 2);
        d[18] = node->dev ? 2 : node->type == FS_DIR ? 4 : node->type == FS_LINK ? 10 : 8;
        memcpy(d + 19, name, nl + 1);
        pos += reclen;
        f->off = idx + 1;
    }
    return (int)pos;
}

static bool is_shm(fs_node_t* n);
static void shm_drop(fs_node_t* n);
static void shm_drop_ino(fs_node_t* n) { ino_gone(n, false); shm_drop(n); }

static int do_unlink(int dirfd, const char* path, int flags) {
    UCHK(path, 1);
    int err;
    fs_node_t* n = lookup_dent(dirfd, path, &err);
    if (!n) return err;
    if (flags & AT_REMOVEDIR) {
        if (n->type != FS_DIR) return -ENOTDIR;
        if (n->child) return -ENOTEMPTY;
        if (n == fs_root()) return -EBUSY;
        for (int i = 0; i < proc_count(); i++) {
            proc_t* p = proc_at(i);
            if (p && p->sh->cwd == n) return -EBUSY;
        }
    } else if (n->type == FS_DIR) {
        return -EISDIR;
    }
    fs_node_t* parent = n->parent;
    if (!parent) return -EBUSY;
    if (is_shm(n) && !n->xl && !n->hl) shm_drop(n);       // mappings keep their own refs
    ino_ev(parent, 0x200 | (n->type == FS_DIR ? 0x40000000 : 0), n->name, 0);
    ino_gone(n, !n->xl && !n->hl);
    fs_drop_name(n);
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
    n->mode = (uint16_t)(mode & ~me()->sh->umask & 07777);
    ino_node(n, 0x100);
    return 0;
}

static bool is_ancestor(fs_node_t* a, fs_node_t* n) {
    for (; n; n = n->parent) if (n == a) return true;
    return false;
}

static int do_rename(int ofd, const char* from, int nfd, const char* to) {
    UCHK(from, 1); UCHK(to, 1);
    int err;
    fs_node_t* src = lookup_dent(ofd, from, &err);
    if (!src) return err;
    char name[FS_NAME_MAX];
    fs_node_t* parent = lookup_parent(nfd, to, name, &err);
    if (!parent) return err;
    if ((src->hl || src->xl) && fs_owner(src->parent) != fs_owner(parent)) return -EXDEV;
    // to another volume: lazy ext2 files have to come along in memory, fat sync reads ->data
    if (fs_owner(src->parent) != fs_owner(parent)) fs_need_tree(src);
    if (src->type == FS_DIR && is_ancestor(src, parent)) return -EINVAL;
    fs_node_t* dst = fs_child(parent, name);
    if (dst == src) return 0;
    if (dst && (dst->hl ? dst->hl : dst) == (src->hl ? src->hl : src)) return 0;   /* two names of one file */
    if (dst) {
        if (dst->type == FS_DIR && src->type != FS_DIR) return -EISDIR;
        if (dst->type != FS_DIR && src->type == FS_DIR) return -ENOTDIR;
        if (dst->type == FS_DIR && dst->child) return -ENOTEMPTY;
        // was AT_FDCWD: apk renames relative to a dirfd of /, from /root that missed
        int r = do_unlink(nfd, to, dst->type == FS_DIR ? AT_REMOVEDIR : 0);
        if (r < 0) return r;
    }
    static uint32_t cookie;
    uint32_t ck = ++cookie, isd = src->type == FS_DIR ? 0x40000000 : 0;
    ino_ev(src->parent, 0x40 | isd, src->name, ck);
    fs_detach(src);
    strncpy(src->name, name, FS_NAME_MAX - 1);
    src->name[FS_NAME_MAX - 1] = 0;
    fs_attach(parent, src);
    ino_ev(parent, 0x80 | isd, src->name, ck);
    ino_ev(src, 0x800, NULL, 0);
    return 0;
}

static int do_link(int ofd, const char* from, int nfd, const char* to, int flags) {
    UCHK(from, 1); UCHK(to, 1);
    int err;
    fs_node_t* src;
    if ((flags & 0x1000) && !from[0]) {                        /* AT_EMPTY_PATH: the fd itself */
        file_t* f = getf(ofd);
        if (!f) return -EBADF;
        if (f->type != F_NODE) return -ENOENT;
        src = f->node;
    } else src = lookup_peek(ofd, from, &err, (flags & 0x400) != 0);   /* AT_SYMLINK_FOLLOW */
    if (!src) return err;
    if (src->type == FS_DIR) return -EPERM;
    if (src->dev || src->unlinked) return src->unlinked ? -ENOENT : -EPERM;
    char name[FS_NAME_MAX];
    fs_node_t* par = lookup_parent(nfd, to, name, &err);
    if (!par) return err;
    if (fs_child(par, name)) return -EEXIST;
    fs_node_t* ow = fs_owner(src);
    if (ow != fs_owner(par)) return -EXDEV;
    if (ow->mount_id && ow->mount_id < 8) return -EPERM;       /* fat */
    if (src->xl > 60000) return -EMLINK;
    if (fs_hlink(src, par, name) < 0) return -ENOMEM;
    ino_ev(par, 0x100, name, 0);
    return 0;
}

static int do_access(int dirfd, const char* path) {
    UCHK(path, 1);
    int err;
    return lookup_peek(dirfd, path, &err, true) ? 0 : err;
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
    file_ref(f);
    file_t* old = fd_swap(nfd, f);
    if (old) { flk_close(me()->sh, old); file_close(old); }
    me()->sh->cloexec[nfd] = cloexec;
    return nfd;
}

static int do_fcntl(int fd, int cmd, uint64_t arg) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    switch (cmd) {
        case 0:    return do_dup(fd, (int)arg, false);          /* F_DUPFD */
        case 1030: return do_dup(fd, (int)arg, true);           /* F_DUPFD_CLOEXEC */
        case 1:    return me()->sh->cloexec[fd] ? 1 : 0;            /* F_GETFD */
        case 2:    me()->sh->cloexec[fd] = arg & 1; return 0;       /* F_SETFD */
        case 3:    return f->flags;                              /* F_GETFL */
        case 4:    f->flags = (f->flags & O_ACCMODE) | (int)(arg & (O_APPEND | O_NONBLOCK)); return 0;
        case 5: case 6: case 7: case 12: case 13: case 14: case 36: case 37: case 38:   /* record locks, ofd */
            UCHK((void*)arg, 32);
            return flk_fcntl(f, cmd, (uint8_t*)arg);
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
    if (b < 0) { file_close(me()->sh->fds[a]); me()->sh->fds[a] = NULL; return b; }
    fds[0] = a;
    fds[1] = b;
    return 0;
}

static int sock_ioctl(uint32_t req, uint64_t arg);

static int do_ioctl(int fd, uint32_t req, uint64_t arg) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if (req >= 0x8900 && req < 0x8950 && (f->type == F_SOCKET || f->type == F_NETLINK || f->type == F_USOCK)) return sock_ioctl(req, arg);
    if (req == 0x5421) {                                      /* FIONBIO */
        UCHK((void*)arg, 4);
        if (*(int*)arg) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK;
        return 0;
    }
    if (req == 0x541B) {                                      /* FIONREAD */
        UCHK((void*)arg, 4);
        int n = 0;
        if (f->type == F_PIPE_R || f->type == F_SPAIR || f->type == F_INOTIFY) n = f->pipe->count;
        else if (f->type == F_NODE && f->node->type == FS_FILE && f->off < f->node->size)
            n = (int)(f->node->size - f->off);
        else if (f->type == F_TTY) n = tty_readable() ? 1 : 0;
        else if (f->type == F_PTM || f->type == F_PTS) n = pty_pending(f->pty, f->type == F_PTM);
        *(int*)arg = n;
        return 0;
    }
    if (f->type == F_DRM) return drm_ioctl(f->drm, req, (void*)arg);
    if (f->type == F_SND) {
        uint32_t len = (req >> 16) & 0x3FFF;
        if (len) UCHK((void*)arg, len);
        return snd_ioctl(f->snd, req, (void*)arg, (f->flags & 04000) != 0);
    }
    if (f->type == F_INPUT) {
        uint32_t len = (req >> 16) & 0x3FFF;
        if (len) UCHK((void*)arg, len);
        return input_ioctl(f->disk, req, (uint8_t*)arg);
    }
    if (f->type == F_FB) {
        if (req == FBIOGET_VSCREENINFO) { UCHK((void*)arg, 160); fbdev_vscreeninfo((uint32_t*)arg); return 0; }
        if (req == FBIOGET_FSCREENINFO) { UCHK((void*)arg, 68);  fbdev_fscreeninfo((uint32_t*)arg); return 0; }
        /* PUT_VSCREENINFO: no mode setting here, say yes and hand back what we have
           (linux does that too when it rounds to the hw). xorg wants it to work */
        if (req == 0x4601) { UCHK((void*)arg, 160); fbdev_vscreeninfo((uint32_t*)arg); return 0; }
        if (req == 0x4606 || req == 0x4611 || req == 0x4604 || req == 0x4605) return 0;   /* pan, blank, cmap */
        if (req == FBIO_BLIT8) {
            UCHK((void*)arg, sizeof(fb_blit8_t));
            const fb_blit8_t* b = (const fb_blit8_t*)arg;
            if (b->w > 4096 || b->h > 4096) return -EINVAL;
            UCHK((void*)b->pixels, (uint64_t)b->w * b->h);
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
    if (f->type == F_PTM || f->type == F_PTS) return pty_ioctl(f->pty, f->type == F_PTM, req, arg);
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

static uint64_t do_brk(uint64_t want) {
    proc_t* p = me();
    if (want < p->sh->brk_start || want >= USER_MMAP_BASE) return p->sh->brk;
    uint64_t old_top = (p->sh->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_top = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_top > old_top) {
        if (!vmm_range_unmapped(p->pd, old_top, new_top - old_top)) return p->sh->brk;
        if (vmm_alloc_range(p->pd, old_top, new_top - old_top, true) < 0) {
            vmm_free_range(p->pd, old_top, new_top - old_top);
            vmm_flush();
            return p->sh->brk;
        }
    } else if (new_top < old_top) {
        vmm_free_range(p->pd, new_top, old_top - new_top);
        vmm_flush();
    }
    p->sh->brk = want;
    return want;
}

/* /dev/shm files: frames live here, every MAP_SHARED of the node maps the
   same ones. the file itself stays empty, read()/write() don't see it.
   TODO: fork copies these pages instead of sharing */
#define SHM_MAX 128
static struct { fs_node_t* node; uint64_t* fr; uint32_t n; } shm[SHM_MAX];

static bool is_shm(fs_node_t* n) {
    if (!n->parent && !strncmp(n->name, "memfd:", 6)) return true;    /* memfd_create: shared like /dev/shm */
    return n->parent && !strcmp(n->parent->name, "shm") && n->parent->parent &&
           !strcmp(n->parent->parent->name, "dev");
}

static void shm_drop(fs_node_t* n) {
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm[i].node != n) continue;
        for (uint32_t k = 0; k < shm[i].n; k++) pmm_unref(shm[i].fr[k]);
        kfree(shm[i].fr);
        shm[i].node = NULL;
        shm[i].n = 0;
    }
}

static uint64_t* shm_frames(fs_node_t* n, uint32_t pages) {
    int s = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm[i].node == n) { s = i; break; }
        if (s < 0 && !shm[i].node) s = i;
    }
    if (s < 0) return NULL;
    if (shm[s].node == n && shm[s].n >= pages) return shm[s].fr;
    uint64_t* fr = (uint64_t*)kmalloc(pages * 8);
    if (!fr) return NULL;
    uint32_t had = shm[s].node == n ? shm[s].n : 0;
    if (had) memcpy(fr, shm[s].fr, had * 8);
    for (uint32_t k = had; k < pages; k++) {
        fr[k] = pmm_alloc();
        if (!fr[k]) { while (k-- > had) pmm_unref(fr[k]); kfree(fr); return NULL; }
    }
    if (had) kfree(shm[s].fr);
    shm[s].node = n;
    shm[s].fr = fr;
    shm[s].n = pages;
    return fr;
}

/* SysV shm, the four calls end up in do_ipc. a segment is a
   dummy node in the shm[] table above, so every shmat maps the same frames.
   for MIT-SHM: glxgears sent every frame down the socket without it */
#define SV_MAX 64
static struct { int key; uint32_t size; fs_node_t* node; int nattch; bool rm; int cpid; uint16_t mode; } sv[SV_MAX];
static struct { int tgid; uint64_t addr, len; int seg; } sva[128];

static void sv_free(int i) {
    shm_drop(sv[i].node);
    kfree(sv[i].node);
    sv[i].node = NULL;
}

static int64_t do_ipc(uint32_t call, int first, uint64_t second, uint64_t third, uint64_t ptr) {
    proc_t* p = me();
    switch (call & 0xFFFF) {
        case 23: {                                                    /* shmget(key, size, flags) */
            int key = first;
            uint32_t size = second, flg = third;
            if (key) for (int i = 0; i < SV_MAX; i++)
                if (sv[i].node && sv[i].key == key && !sv[i].rm) {
                    if ((flg & 03000) == 03000) return -EEXIST;        /* IPC_CREAT|IPC_EXCL */
                    return i + 1;
                }
            if (!(flg & 01000) && key) return -ENOENT;            /* no IPC_CREAT */
            if (!size || size > 256u << 20) return -EINVAL;
            for (int i = 0; i < SV_MAX; i++) {
                if (sv[i].node) continue;
                fs_node_t* n = kmalloc(sizeof(fs_node_t));
                if (!n) return -ENOMEM;
                memset(n, 0, sizeof(*n));
                uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
                sv[i].node = n;
                if (!shm_frames(n, pages)) { kfree(n); sv[i].node = NULL; return -ENOMEM; }
                sv[i].key = key; sv[i].size = size; sv[i].nattch = 0; sv[i].rm = false;
                sv[i].cpid = p->tgid; sv[i].mode = (uint16_t)(flg & 0777);
                return i + 1;
            }
            return -28;                                           /* ENOSPC */
        }
        case 21: {                                                    /* shmat(id, flags, -, addr) */
            int i = first - 1;
            if (i < 0 || i >= SV_MAX || !sv[i].node) return -EINVAL;
            uint64_t len = (sv[i].size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), addr = ptr;
            if (addr) {
                if (addr & (PAGE_SIZE - 1)) { if (second & 020000) addr &= ~(PAGE_SIZE - 1); else return -EINVAL; }   /* SHM_RND */
                if (!vmm_range_unmapped(p->pd, addr, len)) return -EINVAL;
            } else {
                addr = vmm_find_free(p->pd, USER_MMAP_BASE, USER_STACK_TOP - USER_STACK_MAX, len);
                if (!addr) return -ENOMEM;
            }
            int slot = -1;
            for (int k = 0; k < 128; k++) if (!sva[k].len) { slot = k; break; }
            if (slot < 0) return -EMFILE;
            uint64_t* fr = shm_frames(sv[i].node, len / PAGE_SIZE);
            if (!fr) return -ENOMEM;
            for (uint64_t k = 0; k < len / PAGE_SIZE; k++)
                vmm_map_frame(p->pd, addr + k * PAGE_SIZE, fr[k], !(second & 010000));   /* SHM_RDONLY */
            vmm_flush();
            sva[slot].tgid = p->tgid; sva[slot].addr = addr; sva[slot].len = len; sva[slot].seg = i;
            sv[i].nattch++;
            return (int64_t)addr;
        }
        case 22: {                                                    /* shmdt(addr) */
            for (int k = 0; k < 128; k++) {
                if (!sva[k].len || sva[k].tgid != p->tgid || sva[k].addr != ptr) continue;
                vmm_free_range(p->pd, ptr, sva[k].len);
                vmm_flush();
                int i = sva[k].seg;
                sva[k].len = 0;
                if (--sv[i].nattch <= 0 && sv[i].rm) sv_free(i);
                return 0;
            }
            return -EINVAL;
        }
        case 24: {                                                    /* shmctl(id, cmd, buf) */
            int i = first - 1, cmd = (int)(second & 0xFF);
            if (i < 0 || i >= SV_MAX || !sv[i].node) return -EINVAL;
            if (cmd == 0) {                                           /* IPC_RMID */
                sv[i].rm = true;
                if (sv[i].nattch <= 0) sv_free(i);
                return 0;
            }
            if (cmd == 2 || cmd == 13) {                              /* IPC_STAT, SHM_STAT: shmid_ds */
                UCHK((void*)ptr, 112);
                uint8_t* b = (uint8_t*)ptr;
                memset(b, 0, 112);
                *(int*)b = sv[i].key;
                *(uint16_t*)(b + 20) = sv[i].mode;                    /* uid/gid/cuid/cgid 0 */
                *(uint64_t*)(b + 48) = sv[i].size;
                *(uint32_t*)(b + 80) = (uint32_t)sv[i].cpid;
                *(uint64_t*)(b + 88) = (uint64_t)sv[i].nattch;
                return 0;
            }
            if (cmd == 1) return 0;                                   /* IPC_SET: sure */
            return -EINVAL;
        }
    }
    return -ENOSYS;                                                   /* sem*, msg*: not here */
}

#define MAP_SHARED 0x01
#define MAP_FIXED 0x10
#define MAP_ANON  0x20
#define PROT_WRITE 2

static int64_t do_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off) {
    proc_t* p = me();
    if (!len) return -EINVAL;
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    file_t* f = NULL;
    if (!(flags & MAP_ANON)) {
        f = getf(fd);
        if (!f) return -EBADF;
        if (f->type != F_NODE && f->type != F_ZERO && f->type != F_FB && f->type != F_DRM && f->type != F_URING && f->type != F_SND && f->type != F_SOCKET) return -EACCES;
    }
    uint64_t lo = USER_MMAP_BASE, hi = USER_STACK_TOP - USER_STACK_MAX;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_TOP || addr + len < addr)
            return -EINVAL;
        vmm_free_range(p->pd, addr, len);
    } else {
        addr = vmm_reserve(p->pd, addr, lo, hi, len);
        if (!addr) return -ENOMEM;
    }
    if (f && f->type == F_SOCKET) {
        int sr = sock_pkt_mmap(f->sock, p->pd, addr, len);
        if (sr < 0) { vmm_free_range(p->pd, addr, len); return sr; }
        vmm_flush();
        return (int64_t)addr;
    }
    if (f && f->type == F_URING) {
        int ur = uring_mmap(f->ur, p->pd, addr, len, off);
        if (ur < 0) { vmm_free_range(p->pd, addr, len); return ur; }
        vmm_flush();
        return (int64_t)addr;
    }
    if (f && f->type == F_NODE && (flags & MAP_SHARED) && is_shm(f->node)) {
        uint64_t first = off / PAGE_SIZE, np = len / PAGE_SIZE;
        uint64_t* fr = shm_frames(f->node, first + np);
        if (!fr) { vmm_free_range(p->pd, addr, len); return -ENOMEM; }
        for (uint64_t k = 0; k < np; k++)
            vmm_map_frame(p->pd, addr + k * PAGE_SIZE, fr[first + k], (prot & PROT_WRITE) != 0);
        vmm_flush();
        return (int64_t)addr;
    }
    if (f && f->type == F_SND) {
        int r = snd_mmap(f->snd, p->pd, addr, len, off);
        if (r < 0) { vmm_free_range(p->pd, addr, len); vmm_flush(); return r; }
        vmm_flush();
        return (int64_t)addr;
    }
    if (f && f->type == F_DRM) {
        int r = drm_mmap(f->drm, p->pd, addr, len, off, (prot & PROT_WRITE) != 0);
        if (r < 0) { vmm_free_range(p->pd, addr, len); vmm_flush(); return r; }
        vmm_flush();
        return (int64_t)addr;
    }
    if (f && f->type == F_FB) {
        /* the real lfb pages, shared. pmm_ref/unref skip frames outside the
           pool, so munmap/exit can't hand video memory out as ram. xorg fbdev */
        int pitch, bpp;
        uint64_t fb = V2P(gfx_front_fb(&pitch, &bpp));
        uint64_t size = (uint64_t)pitch * (uint64_t)gfx_h();
        if (!fb || off >= size) return -EINVAL;
        for (uint64_t k = 0; k < len / PAGE_SIZE && off + k * PAGE_SIZE < size; k++)
            vmm_map_frame(p->pd, addr + k * PAGE_SIZE, fb + off + k * PAGE_SIZE, (prot & PROT_WRITE) != 0);
        vmm_flush();
        return (int64_t)addr;
    }
    if (!f || f->type == F_ZERO) {
        /* private anon (and /dev/zero): frames on first touch */
        if (vmm_lazy_range(p->pd, addr, len, (prot & PROT_WRITE) != 0, prot != 0) < 0) {
            vmm_free_range(p->pd, addr, len);
            vmm_flush();
            return -ENOMEM;
        }
        vmm_flush();
        return (int64_t)addr;
    }
    if (vmm_alloc_range(p->pd, addr, len, true) < 0) {
        vmm_free_range(p->pd, addr, len);
        vmm_flush();
        return -ENOMEM;
    }
    if (f && f->type == F_NODE && f->node->type == FS_FILE && off < f->node->size) {
        uint64_t n = f->node->size - off;
        if (n > len) n = len;
        vmm_copy_to(p->pd, addr, f->node->data + off, n);
    }
    if (!(prot & PROT_WRITE)) vmm_set_writable(p->pd, addr, len, false);
    if (!prot) vmm_set_user(p->pd, addr, len, false);
    vmm_flush();
    return (int64_t)addr;
}

static int do_munmap(uint64_t addr, uint64_t len) {
    if ((addr & (PAGE_SIZE - 1)) || !len) return -EINVAL;
    if (addr < USER_BASE || addr >= USER_TOP) return 0;
    vmm_free_range(me()->pd, addr, len);
    vmm_flush();
    return 0;
}

static int do_mprotect(uint64_t addr, uint64_t len, int prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (addr < USER_BASE || addr >= USER_TOP) return 0;
    vmm_set_writable(me()->pd, addr, len, (prot & PROT_WRITE) != 0);
    vmm_set_user(me()->pd, addr, len, prot != 0);
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

static int do_clock_gettime(int clk, int64_t* ts) {
    UCHK(ts, 16);
    uint32_t s, ns;
    if (clk == 0 || clk == 5 || clk == 8) clock_now(&s, &ns);     /* REALTIME(_COARSE), BOOTTIME */
    else { uint32_t ms = pit_uptime_ms(); s = ms / 1000; ns = (ms % 1000) * 1000000u; }
    ts[0] = s; ts[1] = ns;
    return 0;
}

/* ---------------- poll / select ---------------- */

typedef struct { int fd; int16_t events, revents; } pollfd_t;

static int16_t ep_ready(file_t* f);

/* what poll would say about one file. epoll uses it too */
static int16_t fd_revents(file_t* f, int16_t want) {
    int16_t ev = 0;
    if (f->type == F_EPOLL) return (want & 0x1) && ep_ready(f) ? 0x1 : 0;
    if ((want & 0x1) && file_readable(f)) ev |= 0x1;
    if ((want & 0x4) && file_writable(f)) ev |= 0x4;
    if (f->type == F_PIPE_R && f->pipe->writers <= 0) ev |= 0x10; /* POLLHUP */
    if (f->type == F_SOCKET && sock_hup(f->sock)) ev |= 0x10;
    if (f->type == F_SPAIR && f->pipe->writers <= 0) ev |= 0x10;
    if (f->type == F_PIPE_W && f->pipe->readers <= 0) ev |= 0x8;  /* POLLERR */
    return ev;
}

static int poll_once(pollfd_t* fds, uint64_t n) {
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        file_t* f = getf(fds[i].fd);
        if (!f) { fds[i].revents = 0x20; ready++; continue; }        /* POLLNVAL */
        int16_t ev = fd_revents(f, fds[i].events);
        fds[i].revents = ev;
        if (ev) ready++;
    }
    return ready;
}

int file_wqs(file_t* f, wq_t** v);

/* queue the waiter on whatever wakes us for f. 1 = nothing to wait on, tick along */
static int wait_arm(file_t* f, wq_w_t* w) {
    if (f->type == F_SOCKET) return sock_wq_add(f->sock, w) ? 0 : 1;
    wq_t* v[2];
    int n = file_wqs(f, v);
    if (n < 0) return 1;
    for (int i = 0; i < n; i++) if (!wq_add(v[i], w)) return 1;
    return 0;
}

/* ms until an armed timerfd fires, 0 = not one. timers don't have a queue */
static uint32_t timer_ms(file_t* f) {
    if (f->type != F_TIMERFD || !f->t_next) return 0;
    int32_t d = (int32_t)(f->t_next - pit_uptime_ms());
    return d > 0 ? (uint32_t)d : 1;
}

/* epoll on top of the same readiness checks, level triggered (EPOLLET is
   taken as level, good enough so far). xorg's ospoll has no poll fallback */
typedef struct ep {
    int n, cap;
    uint32_t gen;
    struct { int fd; file_t* f; uint32_t ev; uint32_t d0, d1; } it[];
} ep_t;

static int16_t ep_ready(file_t* f) {
    ep_t* e = f->ep;
    for (int i = 0; e && i < e->n; i++) {
        file_t* x = getf(e->it[i].fd);
        if (x && x == e->it[i].f && x->type != F_EPOLL && fd_revents(x, (int16_t)(e->it[i].ev & 0xFF))) return 1;
    }
    return 0;
}

static int do_epoll_create(int flags) {
    file_t* f = file_new(F_EPOLL, 2);
    if (!f) return -ENOMEM;
    f->ep = kmalloc(sizeof(ep_t) + 32 * sizeof(f->ep->it[0]));
    if (!f->ep) { kfree(f); return -ENOMEM; }
    f->ep->n = 0; f->ep->cap = 32;
    return install_fd(f, 0, (flags & 02000000) != 0);
}

static int do_epoll_ctl(int epfd, int op, int fd, uint32_t* uev) {
    file_t* f = getf(epfd);
    if (!f) return -EBADF;
    if (f->type != F_EPOLL) return -EINVAL;
    file_t* x = getf(fd);
    if (!x) return -EBADF;
    if (x == f) return -EINVAL;
    ep_t* e = f->ep;
    int i = 0;
    while (i < e->n && !(e->it[i].fd == fd && e->it[i].f == x)) i++;
    e->gen++;
    wq_wake(&f->wq);
    if (op == 2) {                                                /* DEL */
        if (i == e->n) return -ENOENT;
        e->it[i] = e->it[--e->n];
        return 0;
    }
    UCHK(uev, 12);                                                /* packed: events, u64 data */
    if (op == 1) {                                                /* ADD */
        if (i < e->n) return -EEXIST;
        if (e->n == e->cap) {
            ep_t* ne = kmalloc(sizeof(ep_t) + (uint32_t)e->cap * 2 * sizeof(e->it[0]));
            if (!ne) return -ENOMEM;
            memcpy(ne, e, sizeof(ep_t) + (uint32_t)e->n * sizeof(e->it[0]));
            ne->cap = e->cap * 2;
            kfree(e);
            f->ep = e = ne;
        }
        i = e->n++;
        e->it[i].fd = fd; e->it[i].f = x;
    } else if (op == 3) {                                         /* MOD */
        if (i == e->n) return -ENOENT;
    } else return -EINVAL;
    e->it[i].ev = uev[0];
    e->it[i].d0 = uev[1]; e->it[i].d1 = uev[2];
    return 0;
}

static int do_epoll_wait(int epfd, uint32_t* out, int max, int timeout_ms) {
    file_t* f = getf(epfd);
    if (!f) return -EBADF;
    if (f->type != F_EPOLL || max <= 0) return -EINVAL;
    UCHK(out, (uint32_t)max * 12);
    uint32_t start = pit_uptime_ms();
    WQ_W(w);
    bool armed = false;
    int unk = 0;
    uint32_t gen = 0;
    for (;;) {
        sock_pump();
        ep_t* e = f->ep;
        int got = 0;
        for (int i = 0; i < e->n && got < max; i++) {
            file_t* x = getf(e->it[i].fd);
            if (!x || x != e->it[i].f) { e->it[i] = e->it[--e->n]; i--; continue; }   /* closed: gone, like linux */
            uint32_t want = e->it[i].ev;
            int16_t r = fd_revents(x, (int16_t)((want & 0xFF) | 0x18));
            r &= (int16_t)(want | 0x18);                          /* ERR and HUP always */
            if (!r) continue;
            out[got * 3] = (uint32_t)(uint16_t)r;
            out[got * 3 + 1] = e->it[i].d0;
            out[got * 3 + 2] = e->it[i].d1;
            got++;
            if (want & (1u << 30)) e->it[i].ev = 1u << 30;        /* EPOLLONESHOT: off until MOD */
        }
        if (got || timeout_ms == 0) return got;
        uint32_t el = pit_uptime_ms() - start;
        if (timeout_ms > 0 && el >= (uint32_t)timeout_ms) return 0;
        if (proc_interrupted()) return -EINTR;
        if (armed && gen != e->gen) { wq_waiter_free(w); w = NULL; armed = false; }
        if (!armed) {
            armed = true;
            gen = e->gen;
            unk = 0;
            w = wq_waiter();
            if (!w) unk = 1;
            else {
                if (!wq_add(&f->wq, w)) unk = 1;
                for (int i = 0; i < e->n; i++) unk |= wait_arm(e->it[i].f, w);
            }
            continue;
        }
        uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms - el : 0;
        for (int i = 0; i < e->n; i++) {
            uint32_t t = timer_ms(e->it[i].f);
            if (t && (!ms || t < ms)) ms = t;
        }
        if (unk) ms = 1;
        if (w) wq_sleep(w, ms);
        else task_sleep_ms(1);
    }
}

static int do_poll(pollfd_t* fds, uint64_t n, int timeout_ms) {
    if (n > MAX_FDS * 2) return -EINVAL;
    UCHK(fds, n * sizeof(pollfd_t));
    uint32_t start = pit_uptime_ms();
    WQ_W(w);
    bool armed = false;
    int unk = 0;
    for (;;) {
        sock_pump();
        int r = poll_once(fds, n);
        if (r || timeout_ms == 0) return r;
        uint32_t el = pit_uptime_ms() - start;
        if (timeout_ms > 0 && el >= (uint32_t)timeout_ms) return 0;
        if (proc_interrupted()) return -EINTR;
        if (!armed) {
            armed = true;
            w = wq_waiter();
            if (!w) unk = 1;
            else for (uint64_t i = 0; i < n; i++) {
                file_t* f = fds[i].fd < 0 ? NULL : getf(fds[i].fd);
                if (f) unk |= wait_arm(f, w);
            }
            continue;
        }
        uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms - el : 0;
        for (uint64_t i = 0; i < n; i++) {
            file_t* f = fds[i].fd < 0 ? NULL : getf(fds[i].fd);
            uint32_t t = f ? timer_ms(f) : 0;
            if (t && (!ms || t < ms)) ms = t;
        }
        if (unk) ms = 1;
        if (w) wq_sleep(w, ms);
        else task_sleep_ms(1);
    }
}

static int do_select(int n, uint32_t* rd, uint32_t* wr, uint32_t* ex, int timeout_ms) {
    if (n < 0 || n > MAX_FDS) n = n < 0 ? -1 : MAX_FDS;
    if (n < 0) return -EINVAL;
    uint32_t words = ((uint32_t)n + 31) / 32;
    if (rd) UCHK(rd, words * 4);
    if (wr) UCHK(wr, words * 4);
    if (ex) UCHK(ex, words * 4);
#define FDW (MAX_FDS / 32)
    uint32_t want_r[FDW] = {0}, want_w[FDW] = {0};
    for (uint32_t i = 0; i < words && i < FDW; i++) {
        if (rd) want_r[i] = rd[i];
        if (wr) want_w[i] = wr[i];
    }
    uint32_t start = pit_uptime_ms();
    WQ_W(w);
    bool armed = false;
    int unk = 0;
    for (;;) {
        sock_pump();
        int ready = 0;
        uint32_t got_r[FDW] = {0}, got_w[FDW] = {0};
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
            for (uint32_t i = 0; i < words && i < FDW; i++) {
                if (rd) rd[i] = got_r[i];
                if (wr) wr[i] = got_w[i];
                if (ex) ex[i] = 0;
            }
            return ready;
        }
        if (proc_interrupted()) return -EINTR;
        uint32_t el = pit_uptime_ms() - start;
        uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms - el : 0;
        if (!armed) {
            armed = true;
            w = wq_waiter();
            if (!w) unk = 1;
            for (int fd = 0; w && fd < n; fd++) {
                if (!((want_r[fd >> 5] | want_w[fd >> 5]) & (1u << (fd & 31)))) continue;
                file_t* f = getf(fd);
                if (f) unk |= wait_arm(f, w);
            }
            continue;
        }
        for (int fd = 0; fd < n; fd++) {
            if (!((want_r[fd >> 5] | want_w[fd >> 5]) & (1u << (fd & 31)))) continue;
            file_t* f = getf(fd);
            uint32_t t = f ? timer_ms(f) : 0;
            if (t && (!ms || t < ms)) ms = t;
        }
        if (unk) ms = 1;
        if (w) wq_sleep(w, ms);
        else task_sleep_ms(1);
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
    strcpy(u + 4 * 65, "x86_64");
    strcpy(u + 5 * 65, "(none)");
    return 0;
}

static int do_getcwd(char* buf, uint64_t size) {
    UCHK(buf, size);
    char path[256];
    fs_path(me()->sh->cwd, path, sizeof(path));
    uint64_t n = strlen(path) + 1;
    if (n > size) return -ERANGE;
    memcpy(buf, path, n);
    return (int)n;
}

static int do_chdir(fs_node_t* n) {
    if (n->type != FS_DIR) return -ENOTDIR;
    me()->sh->cwd = n;
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

static int do_sigprocmask(int how, const uint64_t* set, uint64_t* old, uint64_t size) {
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

static int do_statfs(uint64_t* b, fs_node_t* at) {
    UCHK(b, 120);
    memset(b, 0, 120);
    uint32_t cs, tc, fc;
    /* f_type, bsize, blocks, bfree, bavail, files, ffree, fsid, namelen, frsize */
    if (at && fatfs_statfs(fatfs_owner(at), &cs, &tc, &fc)) {
        b[0] = 0x4d44;                                  /* MSDOS_SUPER_MAGIC */
        b[1] = cs;
        b[2] = tc; b[3] = b[4] = fc;
        b[8] = 255; b[9] = cs;
        return 0;
    }
    uint64_t tb, fb;
    if (at && ext2_statfs(at, &cs, &tb, &fb)) {
        b[0] = 0xEF53;
        b[1] = cs;
        b[2] = tb; b[3] = fb; b[4] = fb;
        b[8] = 255; b[9] = cs;
        return 0;
    }
    b[0] = 0x858458f6;                                  /* RAMFS_MAGIC */
    b[1] = 4096;
    b[2] = pmm_total_frames();
    b[3] = b[4] = pmm_free_frames();
    b[8] = 255;
    b[9] = 4096;
    return 0;
}

static int do_sysinfo(uint64_t* s) {
    UCHK(s, 112);
    memset(s, 0, 112);
    s[0] = pit_uptime_ms() / 1000;
    s[4] = heap_total() + pmm_total_frames() * PAGE_SIZE;        /* totalram */
    s[5] = (heap_total() - heap_used()) + pmm_free_frames() * PAGE_SIZE;
    int n = 0;
    for (int i = 0; i < proc_count(); i++) if (proc_at(i)) n++;
    ((uint16_t*)s)[40] = (uint16_t)n;                             /* procs */
    ((uint32_t*)s)[26] = 1;                                       /* mem_unit */
    return 0;
}

static int do_rlimit(int res, uint64_t* old) {
    if (!old) return 0;
    UCHK(old, 16);
    old[0] = old[1] = ~0ull;
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
#define AF_INET6 10
typedef struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } sockaddr_in_t;
typedef struct { uint64_t name; uint32_t namelen, pad; uint64_t iov, iovlen, ctl, ctllen; uint32_t flags; } msghdr_t;
typedef struct { uint64_t len; int32_t level, type; } cmsghdr_t;
typedef struct { uint16_t family, port; uint32_t flow; uint8_t addr[16]; uint32_t scope; } sockaddr_in6_t;

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

static int read_addr(uint64_t uaddr, uint64_t len, uint8_t* ip, uint16_t* port) {
    if (len < 8) return -EINVAL;
    UCHK((void*)uaddr, 8);
    sockaddr_in_t* sa = (sockaddr_in_t*)uaddr;
    if (sa->family == AF_INET6) {
        if (len < 24) return -EINVAL;
        UCHK((void*)uaddr, 24);
        memcpy(ip, ((sockaddr_in6_t*)uaddr)->addr, 16);
        *port = nbo16(sa->port);
        return 0;
    }
    if (sa->family != AF_INET) return -97;                            /* EAFNOSUPPORT */
    uint32_t a = nbo32(sa->addr);
    memset(ip, 0, 16);
    if (a) {                                                          /* 0.0.0.0 stays any */
        ip[10] = ip[11] = 0xFF;
        memcpy(ip + 12, &sa->addr, 4);
    }
    *port = nbo16(sa->port);
    return 0;
}

static int write_addr(uint64_t uaddr, uint64_t ulen, int af, const uint8_t* ip, uint16_t port) {
    if (!uaddr || !ulen) return 0;
    UCHK((void*)ulen, 4);
    uint32_t cap = *(uint32_t*)ulen;
    sockaddr_in6_t sa;
    memset(&sa, 0, sizeof(sa));
    uint32_t sz;
    if (af == 17) {                                                   /* sockaddr_ll, mac/type/ifindex hidden in ip[] */
        uint8_t* l = (uint8_t*)&sa;
        *(uint16_t*)l = 17;
        *(uint16_t*)(l + 2) = nbo16(port);
        *(int*)(l + 4) = ip[7];
        *(uint16_t*)(l + 8) = 1;
        l[10] = ip[6]; l[11] = 6;
        memcpy(l + 12, ip, 6);
        sz = 20;
    } else if (af == AF_INET6) {
        sa.family = AF_INET6;
        sa.port = nbo16(port);
        memcpy(sa.addr, ip, 16);
        sz = sizeof(sa);
    } else {
        sockaddr_in_t* si = (sockaddr_in_t*)&sa;
        si->family = AF_INET;
        si->port = nbo16(port);
        memcpy(&si->addr, ip + 12, 4);
        sz = sizeof(*si);
    }
    uint32_t n = cap < sz ? cap : sz;
    UCHK((void*)uaddr, n);
    memcpy((void*)uaddr, &sa, n);
    *(uint32_t*)ulen = sz;
    return 0;
}

/* SIOCGIF*: busybox ifconfig, ip and if_nametoindex() want them. -1 = lo, 0.. = ethN */
static int sock_ioctl(uint32_t req, uint64_t arg) {
    static const uint8_t z6[6];
    const char* name; const uint8_t* mac; uint32_t ip, mask, gw, rx, tx;
    int n = net_ifcount();
    if (req == 0x8912) {                                      /* SIOCGIFCONF */
        UCHK((void*)arg, 16);
        int* len = (int*)arg;
        uint8_t* out = (uint8_t*)((uint64_t*)arg)[1];          // struct ifconf: int len + pad, then the pointer
        int k = 0;
        for (int i = -1; i < n && (k + 1) * 40 <= *len; i++, k++) {
            if (i < 0) { name = "lo"; ip = 0x7F000001; } else net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
            UCHK(out + k * 40, 40);
            memset(out + k * 40, 0, 40);
            strcpy((char*)out + k * 40, name);
            sockaddr_in_t* sa = (sockaddr_in_t*)(out + k * 40 + 16);
            sa->family = AF_INET; sa->addr = nbo32(ip);
        }
        *len = k * 40;
        return 0;
    }
    UCHK((void*)arg, 40);                                     // ifreq is 40 on x86_64 (ifmap in the union)
    uint8_t* ifr = (uint8_t*)arg;
    int i = -2;
    if (req == 0x8910) {                                      /* SIOCGIFNAME */
        i = *(int*)(ifr + 16) - 2;
        if (i < -1 || i >= n) return -19;
        if (i < 0) name = "lo"; else net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
        memset(ifr, 0, 16); strcpy((char*)ifr, name);
        return 0;
    }
    if (!strcmp((char*)ifr, "lo")) i = -1;
    else for (int k = 0; k < n; k++) { net_ifinfo(k, &name, &mac, &ip, &mask, &gw, &rx, &tx); if (!strcmp((char*)ifr, name)) i = k; }
    if (i == -2) return -19;                                  /* ENODEV */
    mac = z6; ip = 0x7F000001; mask = 0xFF000000;
    if (i >= 0) net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
    sockaddr_in_t* sa = (sockaddr_in_t*)(ifr + 16);
    switch (req) {
        case 0x8933: *(int*)(ifr + 16) = i + 2; return 0;      /* SIOCGIFINDEX */
        case 0x8913: *(uint16_t*)(ifr + 16) = i < 0 ? 0x49 : 0x1043; return 0;
        case 0x8915: case 0x891b: case 0x8919:                /* ADDR, NETMASK, BRDADDR */
            memset(ifr + 16, 0, 16);
            sa->family = AF_INET;
            sa->addr = nbo32(req == 0x8915 ? ip : req == 0x891b ? mask : (ip | ~mask));
            return 0;
        case 0x8927:                                          /* SIOCGIFHWADDR */
            memset(ifr + 16, 0, 16);
            *(uint16_t*)(ifr + 16) = i < 0 ? 772 : 1;
            memcpy(ifr + 18, mac, 6);
            return 0;
        case 0x8921: *(int*)(ifr + 16) = i < 0 ? 65536 : 1500; return 0;   /* MTU */
        case 0x8942: *(int*)(ifr + 16) = i < 0 ? 0 : 1000; return 0;       /* TXQLEN */
        case 0x8946: return -95;                                           /* SIOCETHTOOL, libpcap only wants a sane errno */
    }
    return -ENOTTY;
}

static int do_socketpair(uint32_t domain, uint32_t type, uint32_t proto, int* sv) {
    if (domain != 1) return -97;                                      /* AF_UNIX only */
    if ((type & 0xF) != 1) return -93;                                /* SOCK_STREAM only */
    if (proto) return -93;
    UCHK(sv, 8);
    file_t *x, *y;
    int e = spair_create(&x, &y);
    if (e < 0) return e;
    if (type & 04000) { x->flags |= O_NONBLOCK; y->flags |= O_NONBLOCK; }
    bool cx = (type & 02000000) != 0;
    int a = install_fd(x, 0, cx);
    if (a < 0) { file_close(y); return a; }
    int b = install_fd(y, 0, cx);
    if (b < 0) { file_close(me()->sh->fds[a]); me()->sh->fds[a] = NULL; return b; }
    sv[0] = a;
    sv[1] = b;
    return 0;
}

/* send/recv/shutdown/setsockopt on a socketpair end map onto the pipes.
   connected AF_UNIX sockets are the same thing (ux below). */
static int spair_call(int call, file_t* f, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    switch (call) {
        case 9: case 11: {                                            /* send / sendto */
            UCHK((void*)b, c);
            int ofl = f->flags;
            if (d & 0x40) f->flags |= O_NONBLOCK;
            int r = file_write(f, (const char*)b, c);
            f->flags = ofl;
            return r;
        }
        case 10: case 12: {                                           /* recv / recvfrom */
            UCHK((void*)b, c);
            int ofl = f->flags;
            if (d & 0x40) f->flags |= O_NONBLOCK;
            int r = file_read(f, (char*)b, c);
            f->flags = ofl;
            return r;
        }
        case 16: case 17: {                                           /* sendmsg / recvmsg: xcb lives on these */
            UCHK((void*)b, sizeof(msghdr_t));
            msghdr_t* m = (msghdr_t*)b;
            iovec_t* iov = (iovec_t*)m->iov;
            UCHK(iov, m->iovlen * sizeof(iovec_t));
            int ofl = f->flags;
            if (c & 0x40) f->flags |= O_NONBLOCK;
            int total = 0, r = 0;
            if (call == 16 && m->ctl && m->ctllen >= 16) {           /* SCM_RIGHTS out: refs ride along with the bytes */
                UCHK((void*)m->ctl, m->ctllen);
                cmsghdr_t* cm = (cmsghdr_t*)m->ctl;
                int* cfd = (int*)(cm + 1);
                if (cm->level == 1 && cm->type == 1)
                    for (uint64_t k = 0; k < (cm->len - 16) / 4 && f->pipe2->nfds < 8; k++) {
                        file_t* x = getf(cfd[k]);
                        if (x) { file_ref(x); f->pipe2->fds[f->pipe2->nfds++] = x; }
                    }
            }
            for (uint64_t i = 0; i < m->iovlen; i++) {
                if (!iov[i].len) continue;
                UCHK((void*)iov[i].base, iov[i].len);
                r = call == 16 ? file_write(f, (const char*)iov[i].base, iov[i].len)
                               : file_read(f, (char*)iov[i].base, iov[i].len);
                if (r < 0) break;
                total += r;
                if ((uint64_t)r < iov[i].len) break;
                if (call == 17) f->flags |= O_NONBLOCK;            /* rest of the iovs: only what's there */
            }
            f->flags = ofl;
            if (r < 0 && !total) return r;
            if (call == 17) {
                uint64_t room = m->ctllen;
                m->ctllen = 0; m->flags = 0;
                m->namelen = 0;
                pipe_t* q = f->pipe;
                if (q->nfds && m->ctl && room >= 16) {               /* SCM_RIGHTS in */
                    UCHK((void*)m->ctl, room);
                    cmsghdr_t* cm = (cmsghdr_t*)m->ctl;
                    int* cfd = (int*)(cm + 1);
                    uint32_t k = 0;
                    while (q->nfds && 16 + (k + 1) * 4 <= room) {
                        file_t* x = q->fds[0];
                        for (int j = 1; j < q->nfds; j++) q->fds[j - 1] = q->fds[j];
                        q->nfds--;
                        int nfd = install_fd(x, 0, (c & 0x40000000) != 0);   /* MSG_CMSG_CLOEXEC */
                        if (nfd < 0) { file_close(x); continue; }
                        cfd[k++] = nfd;
                    }
                    cm->len = 16 + k * 4; cm->level = 1; cm->type = 1;
                    m->ctllen = (16 + k * 4 + 7) & ~7ul;
                }
            }
            return total;
        }
        case 13: return spair_shutdown(f, (int)b);
        case 14: return 0;                                            /* setsockopt */
        case 15: {                                                    /* getsockopt(level b, opt c, val d, len e) */
            if (!d || !e) return -EFAULT;
            UCHK((void*)e, 4);
            uint32_t cap = *(uint32_t*)e;
            UCHK((void*)d, cap);
            if (c == 17 && cap >= 12) {                               /* SO_PEERCRED: us, root */
                ((uint32_t*)d)[0] = (uint32_t)me()->tgid; ((uint32_t*)d)[1] = 0; ((uint32_t*)d)[2] = 0;
                *(uint32_t*)e = 12;
                return 0;
            }
            if (cap < 4) return -EINVAL;
            *(uint32_t*)d = c == 3 ? 1 : c == 7 || c == 8 ? 65536 : 0;   /* SO_TYPE stream, buffers, SO_ERROR 0 */
            *(uint32_t*)e = 4;
            return 0;
        }
        case 6: case 7: {                                             /* get{sock,peer}name: AF_UNIX, no name */
            if (!b || !c) return 0;
            UCHK((void*)c, 4);
            if (*(uint32_t*)c >= 2) { UCHK((void*)b, 2); *(uint16_t*)b = 1; }
            *(uint32_t*)c = 2;
            return 0;
        }
    }
    return -95;
}

/* AF_UNIX stream sockets with names (X11 needs them). connect() makes a
   socketpair: the caller's file turns into one end, the other waits in the
   listener's queue for accept(). names live in a small table, path ones get
   a FS_DEV_SOCK node too so ls/stat see them. */
typedef struct ux {
    char name[112];          /* absolute path, or '@' + abstract name */
    int  nlen;
    bool bound, listening;
    file_t* q[16];
    int  nq;
    wq_t wq;
} ux_t;

static ux_t* ureg[32];

static int ux_name(uint64_t addr, uint64_t len, char* out, int* olen) {
    if (len < 3 || len > 110) return -EINVAL;
    UCHK((void*)addr, len);
    const char* sp = (const char*)addr + 2;
    if (*(const uint16_t*)addr != 1) return -97;
    if (!sp[0]) {                                                 /* abstract: bytes, NULs and all */
        out[0] = '@';
        memcpy(out + 1, sp + 1, len - 3);
        *olen = (int)len - 2;
        return 0;
    }
    /* the user's own sun_path: lookup_parent wants a user pointer (UCHK) */
    int k = 0;
    while (k < (int)len - 2 && sp[k]) k++;
    if (k == (int)len - 2 && sp[k]) return -EINVAL;            /* libwayland passes len without the NUL, the struct is zeroed anyway */
    int err;
    char base[FS_NAME_MAX];
    fs_node_t* par = lookup_parent(AT_FDCWD, sp, base, &err);
    if (!par) return err;
    fs_path(par, out, 100);
    int l = (int)strlen(out);
    if (l > 1) out[l++] = '/';
    int bl = (int)strlen(base);
    if (l + bl > 110) return -36;                                /* ENAMETOOLONG */
    memcpy(out + l, base, (size_t)bl);
    *olen = l + bl;
    out[*olen] = 0;
    return 0;
}

static ux_t* ux_find(const char* name, int nlen) {
    for (int i = 0; i < 32; i++)
        if (ureg[i] && ureg[i]->listening && ureg[i]->nlen == nlen && !memcmp(ureg[i]->name, name, (size_t)nlen)) return ureg[i];
    return NULL;
}

bool ux_pending(file_t* f) { return f->ux && f->ux->nq > 0; }

extern wq_t tty_wq;

/* what to sleep on for f: 0..2 queues, -1 = no idea, poll it with ticks. no queue + always ready is 0 */
int file_wqs(file_t* f, wq_t** v) {
    switch (f->type) {
        case F_PIPE_R: case F_PIPE_W: v[0] = &f->pipe->wq; return 1;
        case F_SPAIR: v[0] = &f->pipe->wq; v[1] = &f->pipe2->wq; return 2;
        case F_EVENTFD: v[0] = &f->wq; return 1;
        case F_INOTIFY: v[0] = &f->pipe->wq; return 1;
        case F_TTY: v[0] = &tty_wq; return 1;
        case F_PTM: case F_PTS: v[0] = pty_wq(f->pty); return v[0] ? 1 : 0;
        case F_ULISTEN: v[0] = &f->ux->wq; return 1;
        case F_URING: v[0] = uring_wq(f->ur); return 1;
        case F_NODE: case F_NULL: case F_ZERO: case F_RANDOM: case F_DISK: case F_FB: case F_NETLINK: return 0;
        default: return -1;
    }
}

void ux_release(file_t* f) {
    ux_t* u = f->ux;
    if (!u) return;
    for (int i = 0; i < 32; i++) if (ureg[i] == u) ureg[i] = NULL;
    for (int i = 0; i < u->nq; i++) file_close(u->q[i]);
    wq_drain(&u->wq);
    kfree(u);
    f->ux = NULL;
}

static int ux_call(int call, file_t* f, int fd, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    ux_t* u = f->ux;
    int err;
    switch (call) {
        case 2: {                                                     /* bind */
            if (u->bound) return -EINVAL;
            char nm[112]; int nl;
            if ((err = ux_name(b, c, nm, &nl)) < 0) return err;
            for (int i = 0; i < 32; i++)
                if (ureg[i] && ureg[i]->nlen == nl && !memcmp(ureg[i]->name, nm, (size_t)nl)) return -98;   /* EADDRINUSE */
            if (nm[0] == '/') {
                if (fs_peek(fs_root(), nm, false)) return -98;
                fs_node_t* n = fs_create(fs_root(), nm, FS_FILE);
                if (!n) return -ENOENT;
                n->dev = FS_DEV_SOCK;
                n->mode = 0777;
            }
            int slot = -1;
            for (int i = 0; i < 32; i++) if (!ureg[i]) { slot = i; break; }
            if (slot < 0) return -ENOMEM;
            memcpy(u->name, nm, (size_t)nl);
            u->nlen = nl;
            u->bound = true;
            ureg[slot] = u;
            return 0;
        }
        case 4:                                                       /* listen */
            if (!u->bound) return -EINVAL;
            u->listening = true;
            f->type = F_ULISTEN;
            return 0;
        case 3: {                                                     /* connect */
            if (f->type == F_ULISTEN) return -EINVAL;
            char nm[112]; int nl;
            if ((err = ux_name(b, c, nm, &nl)) < 0) return err;
            ux_t* l = ux_find(nm, nl);
            if (!l) return nm[0] == '/' && !fs_peek(fs_root(), nm, true) ? -ENOENT : -111;   /* ECONNREFUSED */
            if (l->nq >= 16) return -EAGAIN;
            file_t *x, *y;
            if ((err = spair_create(&x, &y)) < 0) return err;
            /* our file becomes end x, in place: the fd keeps pointing at it */
            ux_release(f);
            f->type = F_SPAIR;
            f->pipe = x->pipe;
            f->pipe2 = x->pipe2;
            f->shut = 0;
            kfree(x);
            l->q[l->nq++] = y;
            wq_wake(&l->wq);
            return 0;
        }
        case 5: case 18: {                                            /* accept(4) */
            if (f->type != F_ULISTEN) return -EINVAL;
            WQ_W(w);
            while (!u->nq) {
                if (f->flags & O_NONBLOCK) return -EAGAIN;
                if (proc_interrupted()) return -EINTR;
                wq_wait(&u->wq, &w, 0);
            }
            file_t* y = u->q[0];
            for (int i = 1; i < u->nq; i++) u->q[i - 1] = u->q[i];
            u->nq--;
            if (call == 18 && (d & 04000)) y->flags |= O_NONBLOCK;
            if (b && c) { UCHK((void*)c, 4); if (*(uint32_t*)c >= 2) { UCHK((void*)b, 2); *(uint16_t*)b = 1; } *(uint32_t*)c = 2; }
            int nfd = install_fd(y, 0, call == 18 && (d & 02000000));
            if (nfd < 0) file_close(y);
            return nfd;
        }
        case 6: case 7: {                                             /* getsockname / getpeername */
            if (call == 7) return -107;
            if (!b || !c) return 0;
            UCHK((void*)c, 4);
            uint32_t cap = *(uint32_t*)c, want = 2 + (u->bound ? (uint32_t)u->nlen + (u->name[0] == '/' ? 1 : 0) : 0);
            uint8_t sa[112];
            memset(sa, 0, sizeof(sa));
            *(uint16_t*)sa = 1;
            if (u->bound) {
                if (u->name[0] == '@') memcpy(sa + 3, u->name + 1, (size_t)u->nlen - 1);
                else memcpy(sa + 2, u->name, (size_t)u->nlen);
            }
            uint32_t n = cap < want ? cap : want;
            UCHK((void*)b, n);
            memcpy((void*)b, sa, n);
            *(uint32_t*)c = want;
            return 0;
        }
        case 14: return 0;                                            /* setsockopt */
        case 15: return spair_call(15, f, b, c, d, e);
        case 13: return -107;
        case 9: case 10: case 11: case 12: case 16: case 17: return -107;   /* ENOTCONN */
    }
    (void)fd;
    return -95;
}

/* netlink, just enough NETLINK_ROUTE for musl getifaddrs() (btop died on it).
   musl sends RTM_GETLINK / RTM_GETADDR and reads with MSG_DONTWAIT, so the
   whole dump is built right away in send and sits in f->pipe */
static uint8_t* nl_msg(pipe_t* q, uint16_t type, uint32_t seq, int body) {
    int len = 16 + body;
    if (q->count + len > PIPE_SZ) return NULL;
    uint8_t* m = (uint8_t*)q->buf + q->count;
    memset(m, 0, len);
    *(uint32_t*)m = len;
    *(uint16_t*)(m + 4) = type;
    *(uint16_t*)(m + 6) = 2;                   /* NLM_F_MULTI */
    *(uint32_t*)(m + 8) = seq;
    q->count += len;
    return m;
}

static void nl_attr(pipe_t* q, uint8_t* m, uint16_t type, const void* d, int n) {
    int al = (4 + n + 3) & ~3;
    if (!m || q->count + al > PIPE_SZ) return;
    uint8_t* a = (uint8_t*)q->buf + q->count;
    memset(a, 0, al);
    *(uint16_t*)a = 4 + n;
    *(uint16_t*)(a + 2) = type;
    memcpy(a + 4, d, n);
    q->count += al;
    *(uint32_t*)m += al;
}

static void nl_dump6(pipe_t* q, uint32_t seq) {
    uint8_t a[A6_MAX][16], pl[A6_MAX], sc[A6_MAX];
    for (int i = -1; i < net_ifcount(); i++) {
        int n = ip6_addrs(i, a, pl, sc, A6_MAX);
        for (int k = 0; k < n; k++) {
            uint8_t* m = nl_msg(q, 20, seq, 8);
            if (!m) return;
            m[16] = 10;
            m[17] = pl[k];
            m[18] = 0x80;                             /* IFA_F_PERMANENT */
            m[19] = sc[k] == 0x20 ? 253 : sc[k] == 0x10 ? 254 : 0;
            *(uint32_t*)(m + 20) = i + 2;
            nl_attr(q, m, 1, a[k], 16);
            uint32_t ci[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0, 0 };
            nl_attr(q, m, 6, ci, 16);
        }
    }
}

static void nl_routes(pipe_t* q, uint32_t seq, int af) {
    if (af == 0 || af == 2) {
        for (int i = 0; i < net_ifcount(); i++) {
            const char* name; const uint8_t* mac; uint32_t ip, mask, gw, rx, tx;
            net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
            for (int k = 0; k < 2; k++) {
                uint8_t* m = nl_msg(q, 24, seq, 12);
                if (!m) return;
                int pl = 0;
                for (uint32_t x = mask; x; x <<= 1) pl++;
                m[16] = 2;
                m[17] = k ? 0 : pl;
                m[20] = 254;                          /* main */
                m[21] = k ? 16 : 2;                   /* dhcp / kernel */
                m[22] = k ? 0 : 253;                  /* universe / link */
                m[23] = 1;
                uint32_t be = nbo32(k ? gw : ip & mask), pf = nbo32(ip), oif = i + 2, tb = 254;
                if (k) nl_attr(q, m, 5, &be, 4);
                else { nl_attr(q, m, 1, &be, 4); nl_attr(q, m, 7, &pf, 4); }
                nl_attr(q, m, 4, &oif, 4);
                nl_attr(q, m, 15, &tb, 4);
            }
        }
    }
    if (af == 0 || af == 10) {
        ip6_route_t r[16];
        int n = ip6_routes(r, 16);
        for (int k = 0; k < n; k++) {
            uint8_t* m = nl_msg(q, 24, seq, 12);
            if (!m) return;
            bool gwr = (r[k].flags & 2) != 0;
            m[16] = 10;
            m[17] = (uint8_t)r[k].dlen;
            m[20] = 254;
            m[21] = gwr ? 9 : 2;                      /* ra / kernel */
            m[23] = 1;
            uint32_t oif = r[k].ifi + 2, tb = 254, pr = r[k].metric;
            if (r[k].dlen) nl_attr(q, m, 1, r[k].dst, 16);
            nl_attr(q, m, 4, &oif, 4);
            if (gwr) nl_attr(q, m, 5, r[k].gw, 16);
            nl_attr(q, m, 6, &pr, 4);
            nl_attr(q, m, 15, &tb, 4);
        }
    }
}

static void nl_dump(pipe_t* q, int type, uint32_t seq, int af) {
    static const uint8_t zero[6], ff[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    int n = ((af == 10 && type == 22) || type == 26) ? -1 : net_ifcount();
    if (type == 26) nl_routes(q, seq, af);
    if (type == 22 && (af == 0 || af == 10)) nl_dump6(q, seq);
    for (int i = -1; i < n; i++) {                    /* -1 = lo */
        const char* name = "lo";
        const uint8_t* mac = zero;
        uint32_t ip = 0x7F000001, mask = 0xFF000000, gw, rx = 0, tx = 0;
        if (i >= 0) net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
        if (type == 18) {                             /* RTM_GETLINK -> NEWLINK */
            uint8_t* m = nl_msg(q, 16, seq, 16);
            if (!m) break;
            *(uint16_t*)(m + 18) = i < 0 ? 772 : 1;   /* ARPHRD_LOOPBACK / ETHER */
            *(int32_t*)(m + 20) = i + 2;
            *(uint32_t*)(m + 24) = i < 0 ? 0x49 : 0x1043;
            nl_attr(q, m, 3, name, strlen(name) + 1);
            nl_attr(q, m, 1, mac, 6);
            nl_attr(q, m, 2, i < 0 ? zero : ff, 6);
            uint32_t mtu = i < 0 ? 65536 : 1500;
            nl_attr(q, m, 4, &mtu, 4);
            uint32_t st[23] = { rx, tx };            /* rtnl_link_stats, packets only */
            nl_attr(q, m, 7, st, sizeof(st));
        } else if (type == 22 && af != 10) {          /* RTM_GETADDR -> NEWADDR */
            uint8_t* m = nl_msg(q, 20, seq, 8);
            if (!m) break;
            int pl = 0;
            for (uint32_t k = mask; k; k <<= 1) pl++;
            m[16] = 2;
            m[17] = (uint8_t)pl;
            m[19] = i < 0 ? 254 : 0;                  /* scope host / universe */
            *(uint32_t*)(m + 20) = i + 2;
            uint32_t be = nbo32(ip), bc = nbo32(ip | ~mask);
            nl_attr(q, m, 1, &be, 4);
            nl_attr(q, m, 2, &be, 4);
            if (i >= 0) nl_attr(q, m, 4, &bc, 4);
            nl_attr(q, m, 3, name, strlen(name) + 1);
        }
    }
    uint8_t* d = nl_msg(q, 3, seq, 4);               /* NLMSG_DONE */
    if (d) *(uint16_t*)(d + 6) = 0;
}

/* RTM_NEWADDR / DELADDR, only v6 for `ip -6 addr add` */
static void nl_addr(pipe_t* q, const uint8_t* h, uint32_t len) {
    int type = *(const uint16_t*)(h + 4);
    int err = 0;
    if (len >= 24 && h[16] == 10) {
        uint8_t a[16];
        bool got = false;
        for (uint32_t o = 24; o + 4 <= len; ) {
            int al = *(const uint16_t*)(h + o), at = *(const uint16_t*)(h + o + 2) & 0x3FFF;
            if (al < 4 || o + al > len) break;
            if ((at == 1 || at == 2) && al >= 20) { memcpy(a, h + o + 4, 16); got = true; }
            o += (al + 3) & ~3;
        }
        int ifi = *(const int32_t*)(h + 20) - 2;
        if (!got) err = -22;
        else err = type == 20 ? ip6_addr_add(ifi, a, h[17]) : ip6_addr_del(ifi, a);
    }
    uint8_t* m = nl_msg(q, 2, *(const uint32_t*)(h + 8), 20);
    if (m) { *(uint16_t*)(m + 6) = 0; *(int32_t*)(m + 16) = err; memcpy(m + 20, h, 16); }
}

static int64_t nl_call(int call, file_t* f, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f6) {
    pipe_t* q = f->pipe;
    switch (call) {
        case 2: case 14: return 0;                    /* bind, setsockopt */
        case 6: case 7: {                             /* getsockname: sockaddr_nl, pid 0 */
            if (!b || !c) return 0;
            UCHK((void*)c, 4);
            UCHK((void*)b, 12);
            memset((void*)b, 0, 12);
            *(uint16_t*)b = 16;
            *(uint32_t*)c = 12;
            return 0;
        }
        case 16: {                                    /* sendmsg: busybox ip, iproute2 */
            UCHK((void*)b, sizeof(msghdr_t));
            msghdr_t* mh = (msghdr_t*)b;
            if (!mh->iovlen) return 0;
            iovec_t* iov = (iovec_t*)mh->iov;
            UCHK(iov, sizeof(iovec_t));
            b = (uint64_t)iov[0].base; c = iov[0].len;
        }                                             /* fall through */
        case 9: case 11: {                            /* send(to) */
            UCHK((void*)b, c);
            if (c < 17) return -EINVAL;
            const uint8_t* h = (const uint8_t*)b;
            int type = *(const uint16_t*)(h + 4);
            if (type == 18 || type == 22 || type == 26) nl_dump(q, type, *(const uint32_t*)(h + 8), h[16]);
            else if (type == 20 || type == 21) nl_addr(q, h, c);
            else {                                    /* anything else: error 0 = ack, good enough */
                uint8_t* m = nl_msg(q, 2, *(const uint32_t*)(h + 8), 4);
                if (m) *(uint16_t*)(m + 6) = 0;
            }
            return (int64_t)c;
        }
        case 10: case 12: case 17: {                  /* recv(from/msg) */
            uint8_t* buf; uint64_t cap;
            if (call == 17) {
                UCHK((void*)b, sizeof(msghdr_t));
                msghdr_t* mh = (msghdr_t*)b;
                if (!mh->iovlen) return 0;
                iovec_t* iov = (iovec_t*)mh->iov;
                UCHK(iov, sizeof(iovec_t));
                buf = (uint8_t*)iov[0].base; cap = iov[0].len;
                uint32_t ncap = mh->namelen;
                mh->namelen = 0; mh->ctllen = 0; mh->flags = 0;
                if (mh->name && ncap >= 12) { UCHK((void*)mh->name, 12); memset((void*)mh->name, 0, 12); *(uint16_t*)mh->name = 16; mh->namelen = 12; }   // sockaddr_nl, busybox ip wants it
            } else { buf = (uint8_t*)b; cap = c; }
            if (!q->count) return -EAGAIN;
            if ((call == 17 ? c : d) & 0x20 && (call == 17 ? c : d) & 2) return q->count;   /* PEEK|TRUNC: iproute2 sizes its buffer */
            UCHK(buf, cap);
            // whole messages only, never split one
            uint32_t n = 0;
            while (n < (uint32_t)q->count) {
                uint32_t l = (*(uint32_t*)(q->buf + n) + 3) & ~3u;
                if (n + l > cap) break;
                n += l;
            }
            if (!n) return -EINVAL;
            memcpy(buf, q->buf, n);
            memmove(q->buf, q->buf + n, q->count - n);
            q->count -= n;
            if (call == 12 && e && f6) { UCHK((void*)f6, 4); *(uint32_t*)f6 = 0; }
            return (int64_t)n;
        }
    }
    return 0;
}

int nl_write(file_t* f, const char* buf, uint32_t n) { return nl_call(9, f, (uint64_t)buf, n, 0, 0, 0); }

static int64_t sys_socket_call(int call, uint64_t a, uint64_t b, uint64_t c,
                               uint64_t d, uint64_t e, uint64_t f6) {
    int err = 0;
    sock_t* s;
    uint8_t ip[16]; uint16_t port;
    file_t* fl;
    if (call != 1 && call != 8) {
        file_t* pf = getf((int)a);
        if (pf && pf->type == F_SPAIR) return spair_call(call, pf, b, c, d, e);
        if (pf && pf->type == F_NETLINK) return nl_call(call, pf, b, c, d, e, f6);
        if (pf && (pf->type == F_USOCK || pf->type == F_ULISTEN)) return ux_call(call, pf, (int)a, b, c, d, e);
    }
    switch (call) {
        case 1: {                                                     /* socket */
            if (a == 1 && (b & 0xF) == 2) b = (b & ~0xF) | 3, a = 16;   // unix dgram: dummy, musl if_nametoindex() only needs it for the ioctl
            if (a == 1) {                                             /* AF_UNIX */
                if ((b & 0xF) != 1) return -93;                       /* stream only, no dgram yet */
                fl = file_new(F_USOCK, 2 | ((b & 04000) ? O_NONBLOCK : 0));
                if (!fl) return -ENOMEM;
                fl->ux = (ux_t*)kmalloc(sizeof(ux_t));
                if (!fl->ux) { kfree(fl); return -ENOMEM; }
                memset(fl->ux, 0, sizeof(ux_t));
                return install_fd(fl, 0, (b & 02000000) != 0);
            }
            if (a == 16) {                                            /* AF_NETLINK */
                fl = file_new(F_NETLINK, 2 | ((b & 04000) ? O_NONBLOCK : 0));
                if (!fl) return -ENOMEM;
                fl->pipe = (pipe_t*)kmalloc(sizeof(pipe_t));
                memset(fl->pipe, 0, sizeof(pipe_t));
                return install_fd(fl, 0, (b & 02000000) != 0);
            }
            if (a == 17) {                                            /* AF_PACKET */
                if ((b & 0xF) != 2 && (b & 0xF) != 3) return -94;
                s = sock_create(17, (int)(b & 0xF), nbo16((uint16_t)c), &err);
                if (!s) return err;
                fl = file_new(F_SOCKET, 2 | ((b & 04000) ? O_NONBLOCK : 0));
                if (!fl) { sock_close(s); return -ENOMEM; }
                fl->sock = s;
                return install_fd(fl, 0, (b & 02000000) != 0);
            }
            if (a != AF_INET && a != AF_INET6) return -97;
            int type = (int)(b & 0xF);
            if (type == 3) { if (c != (a == AF_INET ? 1 : 58)) return -93; }
            else if (c && !((type == 1 && c == 6) || (type == 2 && (c == 17 || (c == 58 && a == AF_INET6) || (c == 1 && a == AF_INET))))) return -93;  /* EPROTONOSUPPORT */
            s = sock_create((int)a, type, (int)c, &err);
            if (!s) return err ? err : -93;
            fl = file_new(F_SOCKET, 2 | ((b & 04000) ? O_NONBLOCK : 0));
            if (!fl) { sock_close(s); return -ENOMEM; }
            fl->sock = s;
            return install_fd(fl, 0, (b & 02000000) != 0);
        }
        case 2:                                                       /* bind */
            if (!(s = getsock((int)a, &err))) return err;
            if (sock_af(s) == 17) {
                if (c < 12) return -EINVAL;
                UCHK((void*)b, 12);
                return sock_pkt_bind(s, *(int*)(b + 4), nbo16(*(uint16_t*)(b + 2)));
            }
            if ((err = read_addr(b, c, ip, &port)) < 0) return err;
            return sock_bind(s, ip, port);
        case 3:                                                       /* connect */
            if (!(s = getsock((int)a, &err))) return err;
            if ((err = read_addr(b, c, ip, &port)) < 0) return err;
            return sock_connect(s, ip, port, (getf((int)a)->flags & O_NONBLOCK) != 0);
        case 4:                                                       /* listen */
            if (!(s = getsock((int)a, &err))) return err;
            return sock_listen(s, (int)b);
        case 5: case 18: {                                            /* accept(4) */
            if (!(s = getsock((int)a, &err))) return err;
            sock_t* ns = sock_accept(s, (getf((int)a)->flags & O_NONBLOCK) != 0, &err, ip, &port);
            if (!ns) return err;
            fl = file_new(F_SOCKET, 2 | ((call == 18 && (d & 04000)) ? O_NONBLOCK : 0));
            if (!fl) { sock_close(ns); return -ENOMEM; }
            fl->sock = ns;
            write_addr(b, c, sock_af(ns), ip, port);
            return install_fd(fl, 0, call == 18 && (d & 02000000));
        }
        case 6: case 7:                                               /* getsockname / getpeername */
            if (!(s = getsock((int)a, &err))) return err;
            sock_name(s, call == 7, ip, &port);
            if (call == 7 && !port) return -107;                      /* ENOTCONN */
            return write_addr(b, c, sock_af(s), ip, port);
        case 8: return do_socketpair(a, b, c, (int*)d);               /* socketpair */
        case 9: case 11: {                                            /* send / sendto */
            if (!(s = getsock((int)a, &err))) return err;
            UCHK((void*)b, c);
            bool nb = (getf((int)a)->flags & O_NONBLOCK) || (d & 0x40);
            if (sock_af(s) == 17) {
                const uint8_t* mac = 0; int ifx = 0, pr = 0;
                if (call == 11 && e && f6 >= 20) { UCHK((void*)e, 20); ifx = *(int*)(e + 4); pr = nbo16(*(uint16_t*)(e + 2)); mac = (const uint8_t*)(e + 12); }
                return sock_pkt_send(s, (const uint8_t*)b, c, ifx, pr, mac);
            }
            if (call == 11 && e) {
                if ((err = read_addr(e, f6, ip, &port)) < 0) return err;
                return sock_send(s, (const uint8_t*)b, c, nb, ip, &port);
            }
            return sock_send(s, (const uint8_t*)b, c, nb, 0, 0);
        }
        case 10: case 12: {                                           /* recv / recvfrom */
            if (!(s = getsock((int)a, &err))) return err;
            UCHK((void*)b, c);
            bool nb = (getf((int)a)->flags & O_NONBLOCK) || (d & 0x40);
            int r = sock_recv(s, (uint8_t*)b, c, nb, (d & 2) != 0, ip, &port);
            if (r >= 0 && call == 12) write_addr(e, f6, sock_af(s), ip, port);
            return r;
        }
        case 13:                                                      /* shutdown */
            if (!(s = getsock((int)a, &err))) return err;
            return sock_shutdown(s, (int)b);
        case 14:                                                      /* setsockopt: accepted */
            if (!(s = getsock((int)a, &err))) return err;
            if (b == 41 && c == 26 && d && e >= 4) {                  /* IPV6_V6ONLY */
                UCHK((void*)d, 4);
                sock_v6only(s, 1, *(int*)d);
            } else if (b == 1 && c == 20 && d && e >= 16) {           /* SO_RCVTIMEO */
                UCHK((void*)d, 16);
                int64_t* tv = (int64_t*)d;
                sock_rcvtmo(s, (uint32_t)(tv[0] * 1000 + tv[1] / 1000));
            } else if (sock_af(s) == 17) {
                if (b == 263 && c == 5) { UCHK((void*)d, e < 28 ? 16 : 28); return sock_pkt_ring(s, (uint32_t*)d); }
                if (b == 263 && c == 10) return d ? sock_pkt_ver(s, *(int*)d) : -22;
                if (b == 263) return 0;                                   /* membership (promisc: nothing to do), reserve, ts.. */
                if (b == 1 && c == 26) return -92;                        /* no bpf, libpcap filters in userland */
                if (d && e >= 1) { UCHK((void*)d, 1); sock_opt(s, (int)b, (int)c, e >= 4 ? *(int*)d : *(uint8_t*)d); }
            } else if (b == 6 && c == 13 && d && e >= 4) {            /* TCP_CONGESTION */
                UCHK((void*)d, e);
                return sock_setcc(s, (const char*)d, (int)e);
            } else if (d && e >= 1) {
                UCHK((void*)d, 1);
                sock_opt(s, (int)b, (int)c, e >= 4 ? *(int*)d : *(uint8_t*)d);
            }
            return 0;
        case 15: {                                                    /* getsockopt */
            if (!(s = getsock((int)a, &err))) return err;
            if (!d || !e) return -EFAULT;
            UCHK((void*)e, 4);
            UCHK((void*)d, 4);
            int v = 0;
            if (sock_af(s) == 17 && b == 263) {
                if (c == 11) { int v = *(int*)d; if (v < 1 || v > 2) return -92; *(int*)d = v == 2 ? 68 : 52; *(uint32_t*)e = 4; return 0; }   /* HDRLEN, v2 */
                if (c != 6) return -EINVAL;                               /* PACKET_STATISTICS */
                uint32_t l = *(uint32_t*)e;
                UCHK((void*)d, l < 8 ? l : 8);
                memset((void*)d, 0, l < 8 ? l : 8);
                *(uint32_t*)e = l < 8 ? l : 8;
                return 0;
            }
            { uint32_t l = *(uint32_t*)e; UCHK((void*)d, l); if (sock_getopt(s, (int)b, (int)c, (uint8_t*)d, &l)) { *(uint32_t*)e = l; return 0; } }
            if (b == 1 && c == 4) v = sock_take_error(s);             /* SO_ERROR */
            else if (b == 1 && c == 3) v = sock_type(s);              /* SO_TYPE */
            else if (b == 1 && (c == 7 || c == 8)) v = 65536;         /* SO_SNDBUF/RCVBUF */
            else if (b == 41 && c == 26) v = sock_v6only(s, -1, 0);   /* IPV6_V6ONLY */
            *(int*)d = v;
            *(uint32_t*)e = 4;
            return 0;
        }
        case 16: case 17: {                                           /* sendmsg / recvmsg */
            if (!(s = getsock((int)a, &err))) return err;
            UCHK((void*)b, sizeof(msghdr_t));
            msghdr_t* m = (msghdr_t*)b;
            iovec_t* iov = (iovec_t*)m->iov;
            UCHK(iov, m->iovlen * sizeof(iovec_t));
            bool nb = (getf((int)a)->flags & O_NONBLOCK) || (c & 0x40);
            int total = 0;
            if (call == 16 && m->name) { if ((err = read_addr(m->name, m->namelen, ip, &port)) < 0) return err; }
            for (uint64_t i = 0; i < m->iovlen; i++) {
                if (!iov[i].len) continue;
                UCHK((void*)iov[i].base, iov[i].len);
                int r = call == 16
                    ? sock_send(s, (const uint8_t*)iov[i].base, iov[i].len, nb, m->name ? ip : 0, m->name ? &port : 0)
                    : sock_recv(s, (uint8_t*)iov[i].base, iov[i].len, nb || total > 0, false, ip, &port);
                if (r < 0) { if (total) break; return r; }
                total += r;
                if ((uint64_t)r < iov[i].len || sock_type(s) == 2) break;
            }
            if (call == 17) {
                /* was &len on the kernel stack, UCHK said no and the name never got written.
                   musl 1.2.5 dns drops replies without it (apk) */
                if (m->name) write_addr(m->name, (uint64_t)&m->namelen, sock_af(s), ip, port);
                m->flags = 0;
                if (m->ctl && m->ctllen) {
                    UCHK((void*)m->ctl, m->ctllen);
                    m->ctllen = total >= 0 ? sock_cmsg(s, (uint8_t*)m->ctl, (int)m->ctllen) : 0;
                } else m->ctllen = 0;
            }
            return total;
        }
    }
    return -EINVAL;
}

/* ---------------- dispatcher ---------------- */

static int do_futex(uint64_t uaddr, uint32_t op, uint32_t val, uint64_t d, uint64_t uaddr2, uint32_t val3) {
    int cmd = op & 127;
    uint32_t tmo = 0xFFFFFFFFu;
    if (d && (cmd == 0 || cmd == 9)) {
        UCHK((void*)d, 16);
        int64_t* ts = (int64_t*)d;
        uint64_t ms = (uint64_t)ts[0] * 1000 + ts[1] / 1000000;
        if (cmd == 9) {                                    /* absolute deadline */
            uint64_t now;
            if (op & 256) { uint32_t s, ns; clock_now(&s, &ns); now = (uint64_t)s * 1000 + ns / 1000000; }
            else now = pit_uptime_ms();
            ms = ms > now ? ms - now : 0;
        }
        if (ms < 0xFFFFFFF0ull) tmo = (uint32_t)ms;
    } else if (cmd == 3 || cmd == 4) tmo = (uint32_t)d;    /* val2 */
    return futex_op(uaddr, op, val, tmo, uaddr2, val3);
}

static int timeout_ms(uint64_t ts, uint32_t div) {
    if (!ts) return -1;
    UCHK((void*)ts, 16);
    return (int)(((int64_t*)ts)[0] * 1000 + ((int64_t*)ts)[1] / div);
}

static int64_t dispatch(regs_t* r) {
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8, f6 = r->r9;
    proc_t* p = me();
    int err;
    fs_node_t* n;
    switch (r->rax) {
        case 60:  pt_exit_event(p, (int)((a & 0xFF) << 8)); proc_thread_exit((int)((a & 0xFF) << 8));
        case 231: pt_exit_event(p, (int)((a & 0xFF) << 8)); proc_exit((int)((a & 0xFF) << 8));
        case 57: case 58: case 56: {
            int64_t ret = r->rax == 57 ? proc_fork(r) : r->rax == 58 ? proc_vfork(r) : proc_clone(r);
            if (ret > 0 && p->tracer) {
                int ev = r->rax == 57 ? 1 : r->rax == 58 ? 2 : (a & 0x10000) ? 3 : 1;
                int fl = ev == 1 ? 2 : ev == 2 ? 4 : 8;
                if (p->pt_opts & fl) { r->rax = (uint64_t)ret; pt_event(p, r, ev, (uint64_t)ret); }
            }
            return ret;
        }
        case 0:   return do_read((int)a, (char*)b, c);
        case 1:   return do_write((int)a, (const char*)b, c);
        case 19:  return do_rwv((int)a, (iovec_t*)b, (int)c, false);
        case 20:  return do_rwv((int)a, (iovec_t*)b, (int)c, true);
        case 17: case 18: {                                          /* pread64/pwrite64 */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -ESPIPE;
            uint64_t save = fl->off;
            fl->off = d;
            int res = r->rax == 17 ? do_read((int)a, (char*)b, c) : do_write((int)a, (const char*)b, c);
            fl->off = save;
            return res;
        }
        case 295: case 296: case 327: case 328: {                    /* preadv/pwritev (+v2) */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            bool w = r->rax == 296 || r->rax == 328;
            if (r->rax > 300 && (int64_t)d == -1) return do_rwv((int)a, (iovec_t*)b, (int)c, w);
            if (fl->type != F_NODE) return -ESPIPE;
            uint64_t save = fl->off;
            fl->off = d;
            int64_t res = do_rwv((int)a, (iovec_t*)b, (int)c, w);
            fl->off = save;
            return res;
        }
        case 2:   return do_open(AT_FDCWD, (const char*)a, (int)b, (int)c);
        case 85:  return do_open(AT_FDCWD, (const char*)a, O_CREAT | O_WRONLY | O_TRUNC, (int)b);
        case 257: return do_open((int)a, (const char*)b, (int)c, (int)d);
        case 3: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            fd_swap((int)a, NULL);
            flk_close(p->sh, fl);
            file_close(fl);
            return 0;
        }
        case 61:  return do_wait((int)a, (int*)b, (int)c);
        case 88: case 266: {                                          /* symlink / symlinkat */
            const char* tg = (const char*)a;
            bool at = r->rax == 266;
            const char* lp = at ? (const char*)c : (const char*)b;
            int dfd = at ? (int)b : AT_FDCWD;
            UCHK(tg, 1); UCHK(lp, 1);
            char name[FS_NAME_MAX];
            fs_node_t* par = lookup_parent(dfd, lp, name, &err);
            if (!par) return err;
            if (fs_child(par, name)) return -EEXIST;
            return fs_symlink(par, name, tg) ? 0 : -ENOMEM;
        }
        case 86:  return do_link(AT_FDCWD, (const char*)a, AT_FDCWD, (const char*)b, 0);
        case 265: return do_link((int)a, (const char*)b, (int)c, (const char*)d, (int)e);
        case 133: case 259: return -EPERM;                            /* mknod */
        case 87:  return do_unlink(AT_FDCWD, (const char*)a, 0);
        case 263: return do_unlink((int)a, (const char*)b, (int)c);
        case 84:  return do_unlink(AT_FDCWD, (const char*)a, AT_REMOVEDIR);
        case 59: {
            UCHK((void*)a, 1);
            if (b) UCHK((void*)b, 8);
            if (c) UCHK((void*)c, 8);
            int ret = proc_execve(r, (const char*)a, (char* const*)b, (char* const*)c);
            if (ret >= 0 && p->tracer) pt_exec(p, r);
            return ret;
        }
        case 80:
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            return n ? do_chdir(n) : err;
        case 81: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -ENOTDIR;
            return do_chdir(fl->node);
        }
        case 201: {
            uint32_t t = clock_epoch();
            if (a) { UCHK((void*)a, 8); *(int64_t*)a = t; }
            return t;
        }
        case 90: case 268:
            UCHK((void*)(r->rax == 90 ? a : b), 1);
            n = r->rax == 90 ? lookup_peek(AT_FDCWD, (const char*)a, &err, true) : lookup_peek((int)a, (const char*)b, &err, true);
            if (!n) return err;
            n->mode = (uint16_t)((r->rax == 90 ? b : c) & 07777);
            return 0;
        case 91: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type == F_NODE) fl->node->mode = (uint16_t)(b & 07777);
            return 0;
        }
        case 131:                                                    /* sigaltstack */
            if (a) UCHK((void*)a, 24);
            if (b) UCHK((void*)b, 24);
            return proc_sigaltstack((const uint64_t*)a, (uint64_t*)b, r->rsp);
        case 122: case 123: return 0;                                /* setfsuid/gid: zsh. we're root anyway */
        case 73: {                                                   /* flock */
            file_t* fl = getf((int)a);
            return fl ? flk_flock(fl, (int)b) : -EBADF;
        }
        case 74: case 75: case 306: case 277: {                      /* fsync, fdatasync, syncfs, sync_file_range */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (r->rax == 277 && (d & ~7ull)) return -EINVAL;
            if (r->rax != 306 && fl->type != F_NODE && fl->type != F_DISK) return -EINVAL;
            ext2_sync_all();
            return fatfs_sync_all();
        }
        case 162:                                                    /* sync */
            ext2_sync_all();
            return fatfs_sync_all();
        case 165: {                                                  /* mount */
            UCHK((void*)b, 1);
            if (e & 32) return 0;                                    /* MS_REMOUNT */
            if (d) {
                UCHK((void*)d, 1);
                const char* t = (const char*)d;
                if (!strcmp(t, "proc") || !strcmp(t, "ramfs") || !strcmp(t, "tmpfs") ||
                    !strcmp(t, "sysfs") || !strcmp(t, "devtmpfs")) return 0;
                if (strcmp(t, "vfat") && strcmp(t, "msdos") && strcmp(t, "fat")) return -19;  /* ENODEV */
            }
            UCHK((void*)a, 1);
            fs_node_t* src = lookup(AT_FDCWD, (const char*)a, &err);
            if (!src) return err;
            if (!FS_DEV_IS_DISK(src->dev)) return -15;                  /* ENOTBLK */
            fs_node_t* dst = lookup(AT_FDCWD, (const char*)b, &err);
            if (!dst) return err;
            return fatfs_mount(src->dev - FS_DEV_DISK, dst);
        }
        case 166: {                                                  /* umount2 */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            if (FS_DEV_IS_DISK(n->dev)) return -EINVAL;              /* give the mount point */
            for (int i = 0; i < proc_count(); i++) {
                proc_t* q = proc_at(i);
                for (fs_node_t* w = q ? q->sh->cwd : NULL; w; w = w->parent)
                    if (w == n) return -EBUSY;
            }
            return fatfs_umount(n);
        }
        case 92: case 93: case 94: case 260:                         /* chown & co */
        case 105: case 106: case 113: case 114: case 117: case 119: case 116:
        case 157: case 203: case 28: case 26: case 149: case 150: case 151: case 152:
        case 221: case 160: case 169:
            return 0;
        case 8:   return do_lseek((int)a, (int64_t)b, (int)c);
        case 39:  return p->tgid;
        case 186: return p->pid;                                     /* gettid */
        case 110: { proc_t* l = p->is_thread ? proc_by_pid(p->tgid) : p; int pp = l ? l->ppid : p->ppid; return pp ? pp : 1; }
        case 102: case 104: case 107: case 108: return 0;            /* uid, gid, euid, egid */
        case 118: case 120:                                          /* getresuid / getresgid */
            UCHK((void*)a, 4); UCHK((void*)b, 4); UCHK((void*)c, 4);
            *(int*)a = *(int*)b = *(int*)c = 0;
            return 0;
        case 115: return 0;                                          /* getgroups: none */
        case 128: {                                                  /* rt_sigtimedwait(set, info, ts, sz) */
            UCHK((void*)a, 8);
            uint64_t set = *(uint64_t*)a;
            uint32_t end = 0;
            if (c) { UCHK((void*)c, 16); int64_t* ts = (int64_t*)c; end = pit_uptime_ms() + ts[0] * 1000 + (ts[1] + 999999) / 1000000; if (!end) end = 1; }
            for (;;) {
                uint64_t hit = p->sig_pending & set;
                if (hit) {
                    int sg = __builtin_ctzll(hit) + 1;
                    p->sig_pending &= ~(1ull << (sg - 1));
                    if (b) {
                        UCHK((void*)b, 128);
                        uint32_t* si = (uint32_t*)b;
                        memset(si, 0, 128);
                        si[0] = sg;
                        if (sg == p->sq_sig) {
                            si[2] = (uint32_t)-2; si[4] = p->sq_tid; si[5] = p->sq_over;
                            *(uint64_t*)(si + 6) = p->sq_val;
                            p->sq_sig = 0;
                        }
                    }
                    return sg;
                }
                if (end && (int32_t)(pit_uptime_ms() - end) >= 0) return -11;
                if (proc_interrupted()) return -EINTR;
                task_sleep_ms(2);
            }
        }
        case 34:                                                     /* pause */
            while (!proc_interrupted()) task_sleep_ms(10);
            return -EINTR;
        case 21:  return do_access(AT_FDCWD, (const char*)a);
        case 269: case 439: return do_access((int)a, (const char*)b);
        case 62:  return do_kill((int)a, (int)b);
        case 200: case 234: {                                        /* tkill / tgkill */
            int pid = r->rax == 200 ? (int)a : (int)b, sig = r->rax == 200 ? (int)b : (int)c;
            proc_t* t = proc_by_pid(pid);
            return t ? proc_send_signal_tid(t, sig) : -ESRCH;
        }
        case 82:  return do_rename(AT_FDCWD, (const char*)a, AT_FDCWD, (const char*)b);
        case 264: case 316: return do_rename((int)a, (const char*)b, (int)c, (const char*)d);
        case 83:  return do_mkdir(AT_FDCWD, (const char*)a, (int)b);
        case 258: return do_mkdir((int)a, (const char*)b, (int)c);
        case 32:  return do_dup((int)a, 0, false);
        case 33:  return do_dup2((int)a, (int)b, false);
        case 292: return (a == b) ? -EINVAL : do_dup2((int)a, (int)b, (c & O_CLOEXEC) != 0);
        case 22:  return do_pipe((int*)a, 0);
        case 293: return do_pipe((int*)a, (int)b);
        case 100: {
            if (a) { UCHK((void*)a, 32); memset((void*)a, 0, 32); }
            return pit_uptime_ms() / 10;
        }
        case 12:  return do_brk(a);
        case 16:  return do_ioctl((int)a, (uint32_t)b, c);
        case 72:  return do_fcntl((int)a, (int)b, c);
        case 109: {                                                  /* setpgid */
            proc_t* t = a ? proc_by_pid((int)a) : p;
            if (!t) return -ESRCH;
            t->pgid = b ? (int)b : t->tgid;
            return 0;
        }
        case 111: return p->pgid;
        case 121: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->pgid : -ESRCH; }
        case 124: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->sid : -ESRCH; }
        case 112: p->sid = p->pgid = p->tgid; p->ctty = -1; return p->tgid;   /* setsid: no terminal */
        case 95:  { int old = p->sh->umask; p->sh->umask = (int)(a & 0777); return old; }
        case 13:                                                     /* rt_sigaction */
            if (b) UCHK((void*)b, 32);
            if (c) UCHK((void*)c, 32);
            return proc_sigaction((int)a, (const uint64_t*)b, (uint64_t*)c);
        case 37: {                                                   /* alarm */
            uint32_t left = p->alarm_at ? (p->alarm_at - pit_uptime_ms() + 999) / 1000 : 0;
            p->alarm_at = a ? pit_uptime_ms() + a * 1000 : 0;
            p->alarm_interval = 0;
            return left;
        }
        case 38: case 36: case 222: case 223: case 224: case 225: case 226:
            return sys_timer(r->rax, a, b, c, d);
        case 15:  return proc_sigreturn(r);
        case 14:  return do_sigprocmask((int)a, (const uint64_t*)b, (uint64_t*)c, d);
        case 127:                                                    /* rt_sigpending */
            if (a) { UCHK((void*)a, 8); uint64_t pd = p->sig_pending & p->sig_mask;
                     memcpy((void*)a, &pd, 8); }
            return 0;
        case 130: {                                                  /* rt_sigsuspend */
            UCHK((void*)a, 8);
            uint64_t m = *(uint64_t*)a;
            return proc_sigsuspend(&m);
        }
        case 98:  if (b) { UCHK((void*)b, 144); memset((void*)b, 0, 144); } return 0;
        case 96: {                                                   /* gettimeofday */
            if (a) {
                UCHK((void*)a, 16);
                uint32_t s, ns; clock_now(&s, &ns);
                ((int64_t*)a)[0] = s; ((int64_t*)a)[1] = ns / 1000;
            }
            if (b) { UCHK((void*)b, 8); memset((void*)b, 0, 8); }
            return 0;
        }
        case 89:  return do_readlink(AT_FDCWD, (const char*)a, (char*)b, c);
        case 267: return do_readlink((int)a, (const char*)b, (char*)c, d);
        case 9:   return do_mmap(a, b, (int)c, (int)d, (int)e, f6);
        case 11:  return do_munmap(a, b);
        case 10:  return do_mprotect(a, b, (int)c);
        case 25: {                                                   /* mremap: never moves, musl falls back */
            /* but musl's pthread_getattr_np walks down the main stack with it until
               the answer isn't ENOMEM. always ENOMEM = node spun through all 4 GB.
               the whole 8 MB stack window counts as mapped, it grows on demand */
            bool stk = a >= USER_STACK_TOP - USER_STACK_MAX && a < USER_STACK_TOP;
            if (!stk && !(vmm_pte(me()->pd, a & ~(PAGE_SIZE - 1)) & PTE_P)) return -EFAULT;
            return -ENOMEM;
        }
        case 76: {                                                   /* truncate */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            return node_truncate(n, b);
        }
        case 77: {                                                   /* ftruncate */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -EINVAL;
            if (is_shm(fl->node) && b > (1u << 20) && !fl->node->data) {   /* frames come at mmap, no point in a shadow copy (foot asks for 512M) */
                fl->node->size = b;
                return 0;
            }
            return node_truncate(fl->node, b);
        }
        case 137: {                                                  /* statfs */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            return do_statfs((uint64_t*)b, n);
        }
        case 138:                                                    /* fstatfs */
            if (!getf((int)a)) return -EBADF;
            return do_statfs((uint64_t*)b, getf((int)a)->type == F_NODE ? getf((int)a)->node : NULL);
        case 99:  return do_sysinfo((uint64_t*)a);
        case 63:  return do_uname((char*)a);
        case 23:  return do_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)d, timeout_ms(e, 1000));
        case 270: return do_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)d, timeout_ms(e, 1000000));
        case 7:   return do_poll((pollfd_t*)a, b, (int)c);
        case 271: return do_poll((pollfd_t*)a, b, timeout_ms(c, 1000000));
        case 24:  task_yield(); return 0;
        case SYS_SAMARA: return uwin_syscall(a, b, c, d);           /* desktop windows */
        case 35: case 230: {                                         /* nanosleep / clock_nanosleep */
            const int64_t* ts = (const int64_t*)(r->rax == 35 ? a : c);
            UCHK(ts, 16);
            if (r->rax == 230 && (b & 1)) {                          /* TIMER_ABSTIME */
                uint32_t s, ns; clock_now(&s, &ns);
                return ts[0] > s ? sleep_ms((ts[0] - s) * 1000) : 0;
            }
            return sleep_ms(ts[0] * 1000 + ts[1] / 1000000);
        }
        case 79:  return do_getcwd((char*)a, b);
        case 97:  return do_rlimit((int)a, (uint64_t*)b);
        case 302: {                                                  /* prlimit64 */
            if (d) {
                UCHK((void*)d, 16);
                uint64_t* o = (uint64_t*)d;
                o[0] = o[1] = ~0ull;
                if (b == 3) o[0] = o[1] = USER_STACK_MAX;
                if (b == 7) o[0] = o[1] = MAX_FDS;
            }
            return 0;
        }
        case 4: case 6:                                              /* stat / lstat */
            UCHK((void*)a, 1); UCHK((void*)b, sizeof(kstat64_t));
            n = lookup_peek(AT_FDCWD, (const char*)a, &err, r->rax == 4);
            if (!n) return err;
            fill_stat_node((kstat64_t*)b, n);
            return 0;
        case 5: {                                                    /* fstat */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            UCHK((void*)b, sizeof(kstat64_t));
            fill_stat_file((kstat64_t*)b, fl);
            return 0;
        }
        case 262: {                                                  /* newfstatat */
            UCHK((void*)b, 1); UCHK((void*)c, sizeof(kstat64_t));
            if (((const char*)b)[0] == 0 && (d & 0x1000)) {         /* AT_EMPTY_PATH */
                file_t* fl = getf((int)a);
                if (!fl) return -EBADF;
                fill_stat_file((kstat64_t*)c, fl);
                return 0;
            }
            n = lookup_peek((int)a, (const char*)b, &err, !(d & 0x100));   /* AT_SYMLINK_NOFOLLOW */
            if (!n) return err;
            fill_stat_node((kstat64_t*)c, n);
            return 0;
        }
        case 217: return do_getdents64((int)a, (uint8_t*)b, c);
        case 202: return do_futex(a, (uint32_t)b, (uint32_t)c, d, e, (uint32_t)f6);
        case 29:  return do_ipc(23, (int)a, b, c, 0);                /* shmget */
        case 30:  return do_ipc(21, (int)a, c, 0, b);                /* shmat */
        case 67:  return do_ipc(22, 0, 0, 0, a);                     /* shmdt */
        case 31:  return do_ipc(24, (int)a, b, 0, c);                /* shmctl */
        case 125: {                                                  /* capget: root, every cap there is */
            UCHK((void*)a, 8);
            uint32_t ver = *(uint32_t*)a;
            if (ver != 0x19980330 && ver != 0x20071026 && ver != 0x20080522) { *(uint32_t*)a = 0x20080522; return b ? -EINVAL : 0; }
            if (b) {
                int n = ver == 0x19980330 ? 1 : 2;
                UCHK((void*)b, (uint32_t)n * 12);
                for (int k = 0; k < n; k++) { ((uint32_t*)b)[k * 3] = ~0u; ((uint32_t*)b)[k * 3 + 1] = ~0u; ((uint32_t*)b)[k * 3 + 2] = 0; }
            }
            return 0;
        }
        case 126: return 0;                                          /* capset: sure */
        case 188: case 189: case 190: return -95;           /* setxattr family: no xattrs */
        case 191: case 192: case 193: return -61;                    /* getxattr: ENODATA */
        case 194: case 195: case 196: return 0;                      /* listxattr: empty (ls -l, cp -a, xbps) */
        case 197: case 198: case 199: return -95;                    /* removexattr */
        case 444: case 445: case 446: return -ENOSYS;                /* landlock, xz asks */
        case 285: {                                                  /* fallocate */
            file_t* fl = getf((int)a);
            if (fl && fl->type == F_NODE && b == 0 && is_shm(fl->node)) {   /* xwayland posix_fallocate on memfd, musl has no fallback */
                if (c + d > fl->node->size && node_truncate(fl->node, c + d) < 0) return -ENOMEM;
                return 0;
            }
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -19;
            fs_node_t* n = fl->node;
            if (n->type == FS_DIR) return -EISDIR;
            if (n->type != FS_FILE || n->dev) return -19;
            if ((fl->flags & 3) == 0) return -EBADF;
            if ((int64_t)c < 0 || (int64_t)d <= 0) return -EINVAL;
            if (b & ~(uint64_t)(1 | 2 | 16)) return -95;                 /* EOPNOTSUPP: collapse, insert */
            if ((b & 2) && !(b & 1)) return -95;                     /* punch hole needs keep size */
            uint64_t end = c + d;
            if (end > 0x7FFFFFFF) return -27;                        /* EFBIG, 32 bit sizes in the ramfs */
            fs_need(n);
            if (b & (2 | 16)) {                                      /* hole = zeros, the fs has no holes in memory */
                uint64_t z = c < n->size ? c : n->size, ze = end < n->size ? end : n->size;
                if (ze > z && n->data) { memset(n->data + z, 0, ze - z); n->mtime = fs_now(); fs_touch(n); }
            }
            if (!(b & 1) && end > n->size && node_truncate(n, (uint32_t)end) < 0) return -ENOMEM;
            return 0;
        }
        // membarrier, rseq: not here yet. glib/qemu fall back
        // to pipes and poll on ENOSYS, so just say no without spamming the log
        case 332: return sys_statx((int)a, (const char*)b, (int)c, (uint32_t)d, (void*)e);
        case 289: case 282: {                                        /* signalfd4 / signalfd */
            UCHK((void*)b, 8);
            uint64_t m = *(uint64_t*)b;
            if ((int)a >= 0) {
                file_t* f = getf((int)a);
                if (!f) return -EBADF;
                if (f->type != F_SIGNALFD) return -EINVAL;
                f->cnt = m;
                return (int)a;
            }
            int fl = r->rax == 289 ? (int)d : 0;
            file_t* f = file_new(F_SIGNALFD, 2 | ((fl & 04000) ? O_NONBLOCK : 0));
            if (!f) return -ENOMEM;
            f->cnt = m;
            return install_fd(f, 0, (fl & 02000000) != 0);
        }
        case 324: case 334: case 435:
            return -ENOSYS;
        case 253: case 294: {                                        /* inotify_init(1) */
            file_t* f = ino_new(r->rax == 294 ? (int)a : 0);
            if (!f) return -ENOMEM;
            return install_fd(f, 0, r->rax == 294 && (a & 02000000));
        }
        case 254: {                                                  /* inotify_add_watch */
            file_t* f = getf((int)a);
            if (!f || f->type != F_INOTIFY) return -EBADF;
            UCHK((void*)b, 1);
            int err;
            fs_node_t* n = lookup_ex(AT_FDCWD, (const char*)b, &err, !(c & 0x02000000));
            if (!n) return err;
            if ((c & 0x01000000) && n->type != FS_DIR) return -ENOTDIR;
            return ino_add(f, n, (uint32_t)c);
        }
        case 255: {
            file_t* f = getf((int)a);
            if (!f || f->type != F_INOTIFY) return -EBADF;
            return ino_rm(f, (int)b);
        }
        case 284: case 290: {                                        /* eventfd(2) */
            int fl = r->rax == 290 ? (int)b : 0;
            file_t* f = file_new(F_EVENTFD, 2 | ((fl & 04000) ? O_NONBLOCK : 0) | ((fl & 1) ? 0x10000000 : 0));
            if (!f) return -ENOMEM;
            f->cnt = a;
            return install_fd(f, 0, (fl & 02000000) != 0);
        }
        case 283: {                                                  /* timerfd_create */
            file_t* f = file_new(F_TIMERFD, 2 | (((int)b & 04000) ? O_NONBLOCK : 0));
            if (!f) return -ENOMEM;
            f->disk = (int)a;                                        /* clock id */
            return install_fd(f, 0, (b & 02000000) != 0);
        }
        case 286: case 287: {                                        /* timerfd_settime / gettime */
            file_t* f = getf((int)a);
            if (!f) return -EBADF;
            if (f->type != F_TIMERFD) return -EINVAL;
            bool set = r->rax == 286;
            uint32_t now = pit_uptime_ms();
            int64_t* oldp = (int64_t*)(set ? d : b);
            if (oldp) {                                              /* old / current: interval, value left */
                UCHK(oldp, 32);
                uint32_t left = f->t_next && (int32_t)(f->t_next - now) > 0 ? f->t_next - now : (f->t_next ? 1 : 0);
                oldp[0] = f->t_int / 1000; oldp[1] = (f->t_int % 1000) * 1000000u;
                oldp[2] = left / 1000; oldp[3] = (left % 1000) * 1000000u;
            }
            if (!set) return 0;
            UCHK((void*)c, 32);
            int64_t* nv = (int64_t*)c;
            int64_t is = nv[0], ins = nv[1], vs = nv[2], vns = nv[3];
            uint32_t ims = is * 1000 + ins / 1000000, vms = vs * 1000 + vns / 1000000;
            f->t_int = ims;
            if (!vs && !vns) { f->t_next = 0; return 0; }            /* disarm */
            if (b & 1) {                                             /* TFD_TIMER_ABSTIME */
                uint32_t target;
                if (f->disk == 0) {                                  /* REALTIME: from the wall clock */
                    uint32_t ws, wns; clock_now(&ws, &wns);
                    uint64_t wnow = (uint64_t)ws * 1000 + wns / 1000000, want = (uint64_t)vs * 1000 + vns / 1000000;
                    target = want > wnow ? now + (uint32_t)(want - wnow) : now;
                } else target = vms;                                 /* MONOTONIC: uptime */
                f->t_next = target ? target : 1;
            } else f->t_next = now + (vms ? vms : 1);
            return 0;
        }
        case 319: {                                                  /* memfd_create: a node nobody can find by name */
            UCHK((void*)a, 1);
            fs_node_t* n = kmalloc(sizeof(fs_node_t));
            if (!n) return -ENOMEM;
            memset(n, 0, sizeof(*n));
            strcpy(n->name, "memfd:");
            strncpy(n->name + 6, (const char*)a, FS_NAME_MAX - 8);
            n->type = FS_FILE;
            n->mode = 0600;
            n->mtime = fs_now();
            n->unlinked = true;                                      /* freed on the last close */
            n->refs = 1;
            file_t* f = file_new(F_NODE, 2);
            if (!f) { kfree(n); return -ENOMEM; }
            f->node = n;
            return install_fd(f, 0, (b & 1) != 0);                   /* MFD_CLOEXEC */
        }
        case 213: return do_epoll_create(0);                         /* epoll_create(size) */
        case 291: return do_epoll_create((int)a);                    /* epoll_create1 */
        case 233: return do_epoll_ctl((int)a, (int)b, (int)c, (uint32_t*)d);
        case 232: case 281: return do_epoll_wait((int)a, (uint32_t*)b, (int)c, (int)d);   /* pwait: mask ignored */
        case 204:                                                    /* sched_getaffinity */
            if (b < 8) return -EINVAL;
            UCHK((void*)c, b);
            memset((void*)c, 0, b);
            *(uint64_t*)c = (1ull << ncpu) - 1;
            return 8;
        case 158:                                                    /* arch_prctl */
            if (a == 0x1002) { p->tls_base = b; wrmsr_fs(b); return 0; }   /* ARCH_SET_FS */
            if (a == 0x1003) { UCHK((void*)b, 8); *(uint64_t*)b = p->tls_base; return 0; }
            return -EINVAL;
        case 218: p->clear_child_tid = a; return p->pid;             /* set_tid_address */
        case 228: return do_clock_gettime((int)a, (int64_t*)b);
        case 229:
            if (b) { UCHK((void*)b, 16); memset((void*)b, 0, 16); ((int64_t*)b)[1] = 1000000; }
            return 0;
        case 132: case 235: case 280: {                              /* utime(s)/utimensat */
            const char* path = (const char*)(r->rax == 280 ? b : a);
            int dfd = r->rax == 280 ? (int)a : AT_FDCWD;
            uint32_t mt = fs_now();
            const int64_t* ts = (const int64_t*)(r->rax == 280 ? c : r->rax == 235 ? b : 0);
            if (ts && uok(ts, 32)) {
                if (r->rax == 280 && ts[3] == ((1 << 30) - 2)) return 0;          /* UTIME_OMIT */
                if (!(r->rax == 280 && ts[3] == ((1 << 30) - 1))) mt = (uint32_t)ts[2];
            } else if (r->rax == 132 && b && uok((void*)b, 16)) mt = (uint32_t)((int64_t*)b)[1];
            if (!path) { file_t* fl = getf(dfd); if (fl && fl->type == F_NODE) fl->node->mtime = mt; return 0; }
            UCHK(path, 1);
            n = lookup_peek(dfd, path, &err, !(r->rax == 280 && (d & 0x100)));
            if (!n) return err;
            n->mtime = mt;
            return 0;
        }
        case 318: {                                                  /* getrandom */
            UCHK((void*)a, b);
            file_t tmp = { .type = F_RANDOM };
            return file_read(&tmp, (char*)a, b);
        }
        case 41:  return sys_socket_call(1, a, b, c, 0, 0, 0);        /* socket */
        case 53:  return sys_socket_call(8, a, b, c, d, 0, 0);        /* socketpair */
        case 49:  return sys_socket_call(2, a, b, c, 0, 0, 0);        /* bind */
        case 42:  return sys_socket_call(3, a, b, c, 0, 0, 0);        /* connect */
        case 50:  return sys_socket_call(4, a, b, 0, 0, 0, 0);        /* listen */
        case 43:  return sys_socket_call(5, a, b, c, 0, 0, 0);        /* accept */
        case 288: return sys_socket_call(18, a, b, c, d, 0, 0);       /* accept4 */
        case 55:  return sys_socket_call(15, a, b, c, d, e, 0);       /* getsockopt */
        case 54:  return sys_socket_call(14, a, b, c, d, e, 0);       /* setsockopt */
        case 51:  return sys_socket_call(6, a, b, c, 0, 0, 0);        /* getsockname */
        case 52:  return sys_socket_call(7, a, b, c, 0, 0, 0);        /* getpeername */
        case 44:  return sys_socket_call(11, a, b, c, d, e, f6);      /* sendto */
        case 46:  return sys_socket_call(16, a, b, c, 0, 0, 0);       /* sendmsg */
        case 45:  return sys_socket_call(12, a, b, c, d, e, f6);      /* recvfrom */
        case 47:  return sys_socket_call(17, a, b, c, 0, 0, 0);       /* recvmsg */
        case 48:  return sys_socket_call(13, a, b, 0, 0, 0, 0);       /* shutdown */
        case 40:  return do_copy((int)b, (uint64_t*)c, (int)a, NULL, d);
        case 326: return do_copy((int)a, (uint64_t*)b, (int)c, (uint64_t*)d, e);
        case 101: return sys_ptrace(a, b, c, d);
        case 135: {                                                   /* personality, gdb wants ADDR_NO_RANDOMIZE to stick */
            static uint32_t pers;
            uint32_t old = pers;
            if ((uint32_t)a != 0xffffffffu) pers = (uint32_t)a;
            return old;
        }
        case 103: return -EPERM;
        case 425: return uring_setup(a, (void*)b);
        case 426: return uring_enter((int)a, b, c, d, (const void*)e, f6);
        case 427: return uring_register((int)a, b, (void*)c, d);
    }
    klog("[sys] pid ");
    klog_num(p->pid);
    klog(" ("); klog(p->name); klog(") unimplemented syscall ");
    klog_num((int64_t)r->rax);
    klog("\r\n");
    return -ENOSYS;
}

/* for uring.c: the same things the syscalls do, for the current (maybe borrowed) process */
file_t* sys_getf(int fd) { return getf(fd); }
bool sys_uok(const void* p, uint32_t len) { return uok(p, len); }
int sys_openat(int dirfd, const char* path, int flags, int mode) { return do_open(dirfd, path, flags, mode); }
int sys_sock(int call, int fd, size_t b, size_t c, size_t d, size_t e, size_t f) {
    return sys_socket_call(call, (uint32_t)fd, b, c, d, e, f);
}
int16_t sys_revents(file_t* f, int16_t want) { return fd_revents(f, want); }

int sys_close(int fd) {
    file_t* fl = getf(fd);
    if (!fl) return -EBADF;
    fd_swap(fd, NULL);
    file_close(fl);
    return 0;
}

// for fixed files and sockets: the socket code wants an fd number
int sys_tmpfd(file_t* f) {
    file_ref(f);
    int fd = install_fd(f, 0, false);
    return fd;
}

void sys_untmpfd(int fd) { sys_close(fd); }

int sys_statx(int dirfd, const char* path, int flags, uint32_t mask, void* out) {
    kstat64_t st;
    if (!uok(path, 1) || !uok(out, 256)) return -EFAULT;
    if (!path[0] && (flags & 0x1000)) {
        file_t* fl = getf(dirfd);
        if (!fl) return -EBADF;
        fill_stat_file(&st, fl);
    } else {
        int err;
        fs_node_t* n = lookup_peek(dirfd, path, &err, !(flags & 0x100));
        if (!n) return err;
        fill_stat_node(&st, n);
    }
    uint8_t* o = out;
    memset(o, 0, 256);
    uint32_t* w = (uint32_t*)o;
    w[0] = 0x7FF;                                    /* basic stats */
    w[1] = st.st_blksize;
    w[4] = st.st_nlink; w[5] = st.st_uid; w[6] = st.st_gid;
    *(uint16_t*)(o + 28) = (uint16_t)st.st_mode;
    memcpy(o + 32, &st.st_ino, 8);
    memcpy(o + 40, &st.st_size, 8);
    memcpy(o + 48, &st.st_blocks, 8);
    w[16] = st.st_atime; w[24] = st.st_ctime; w[28] = st.st_mtime;
    w[32] = (uint32_t)(st.st_rdev >> 8) & 0xFFF; w[33] = (uint32_t)st.st_rdev & 0xFF;
    w[34] = (uint32_t)(st.st_dev >> 8) & 0xFFF; w[35] = (uint32_t)st.st_dev & 0xFF;
    return 0;
}

/* ext2 sync vs syscalls that change files. the sync walks the tree and
   reads file bytes while apk writes and unlinks under it. a writer marks
   its process, a sync waits until no live process is marked and keeps new
   writers waiting. per process, not a counter: the counter leaked when a
   process died inside a write, every sync after that hung and X sat on a
   black screen waiting in open() */
static volatile bool fs_syncing;

void fs_write_begin(void) {
    proc_t* p = proc_current();
    for (;;) {
        uint32_t f = irq_save();
        if (!fs_syncing) { if (p) p->in_fs = true; irq_restore(f); return; }
        irq_restore(f);
        task_sleep_ms(2);
    }
}

void fs_write_end(void) {
    proc_t* p = proc_current();
    if (p) p->in_fs = false;
}

static bool fs_writers(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* p = proc_at(i);
        if (p && p->state == P_ALIVE && p->in_fs) return true;
    }
    return false;
}

void fs_sync_begin(void) {
    for (;;) {
        uint32_t f = irq_save();
        if (!fs_syncing) { fs_syncing = true; irq_restore(f); break; }
        irq_restore(f);
        task_sleep_ms(2);
    }
    /* bounded: a writer stuck in a lazy read for long is still better than a hung sync */
    uint32_t t0 = pit_uptime_ms();
    while (fs_writers() && pit_uptime_ms() - t0 < 3000) task_sleep_ms(1);
}

void fs_sync_end(void) { fs_syncing = false; }

/* a write ran out of file memory. step out of the writers (the file isn't
   touched yet), let a sync run and drop clean files, step back in */
/* same for anybody (a lazy read in exec, a trigger script during apk add):
   sync so the dirty files become droppable, then drop. steps out of the
   writers if it was one */
void fs_need_room(uint32_t need) {
    proc_t* p = proc_current();
    bool was = p && p->in_fs;
    if (was) p->in_fs = false;
    ext2_make_room(need);
    if (was) fs_write_begin();
}

void fs_wait_room(uint32_t need) {
    proc_t* p = proc_current();
    if (!p || !p->in_fs) return;
    p->in_fs = false;
    ext2_make_room(need);
    fs_write_begin();
}

/* syscalls that change files or the tree. they don't run while an ext2
   sync walks it (fs_write_begin). writes only when they hit a ramfs file:
   a blocking write into a pipe would hold the sync off forever */
static bool changes_fs(uint64_t nr, uint64_t a) {
    switch (nr) {
        case 1: case 20: case 18: case 77: case 285: case 91: case 40: {
            file_t* f = getf((int)a);
            return f && f->type == F_NODE;
        }
        case 2: case 85: case 257: case 87: case 263: case 82: case 264: case 316:
        case 83: case 258: case 84: case 88: case 266: case 86: case 265:
        case 76: case 90: case 268: case 132: case 235: case 280: case 326:
            return true;
    }
    return false;
}

void syscall_dispatch(regs_t* r) {
    proc_t* tp = proc_current();
    if (tp && tp->tracer) {
        tp->pt_orig = r->rax;
        if (tp->pt_sys) { pt_syscall_stop(tp, r, false); tp->pt_orig = r->rax; }
    }
    uint64_t nr = r->rax;
    proc_check_alarm(proc_current(), false);
    bool mut = changes_fs(nr, r->rdi);
    if (mut) {
        ext2_throttle();                      /* before: the sync waits for writers */
        fs_write_begin();
    }
    int64_t ret;
    uint64_t t0 = prof_tsc();
    if (g_ftrace && nr == 59 && ustr_ok((const char*)r->rdi)) {
        klog("[x] "); klog_num(pit_uptime_ms()); klog(" exec "); klog((const char*)r->rdi); klog("\r\n");
    }
    proc_t* pc = proc_current();
    if (pc && !setjmp((void*)pc->ujb)) {
        pc->ujb_on = true;
        ret = dispatch(r);
        pc->ujb_on = false;
    } else if (pc) {
        pc = proc_current();
        pc->ujb_on = false;
        ret = -EFAULT;
    } else ret = dispatch(r);
    if (mut) fs_write_end();
    prof_sys((int)nr, prof_tsc() - t0);
    switch (nr) {      // io that can make a waiter in poll/read/write ready
        case 0: case 1: case 3: case 17: case 18: case 19: case 20: case 42: case 43: case 44: case 45:
        case 46: case 47: case 48: case 53: case 299: case 307: io_wake();
    }
    if (g_strace && g_strace_pid && proc_current() && proc_current()->pid == g_strace_pid) {
        /* buffered: record now, print when the process exits (timing stays intact) */
        static struct { int32_t nr, a, b, c, ret; } rec[4096];
        static int nrec;
        if (nrec < 4096) { rec[nrec].nr = (int32_t)nr; rec[nrec].a = (int32_t)r->rdi;
                           rec[nrec].b = (int32_t)r->rsi; rec[nrec].c = (int32_t)r->rdx; rec[nrec++].ret = (int32_t)ret; }
        if (nr == 60 || nr == 231) {                                 /* exit: print the log */
            for (int i = 0; i < nrec; i++) {
                klog("[st] "); klog_num(rec[i].nr); klog("("); klog_num(rec[i].a); klog(", ");
                klog_num(rec[i].b); klog(", "); klog_num(rec[i].c); klog(") = "); klog_num(rec[i].ret); klog("\r\n");
            }
            nrec = 0;
        }
    } else if (g_strace && !g_strace_pid) {
        proc_t* p = proc_current();
        klog("[strace] "); klog_num(pit_uptime_ms()); klog(" "); klog_num(p ? p->pid : 0);
        klog(" "); klog_num(nr);
        klog("("); klog_num(r->rdi); klog(", "); klog_num(r->rsi);
        klog(", "); klog_num(r->rdx); klog(") = "); klog_num(ret); klog("\r\n");
    }
    if (g_ftrace && ret < 0 && ret != -11 && proc_current()) {
        const char* ps = NULL;
        switch (nr) {
            case 2: case 4: case 6: case 21: case 76: case 80: case 82: case 83: case 84: case 85: case 87: case 88: case 89: case 90: case 92: case 59:
                ps = (const char*)r->rdi; break;
            case 257: case 258: case 259: case 262: case 263: case 264: case 265: case 266: case 267: case 268: case 269: case 280: case 316: case 332:
                ps = (const char*)r->rsi; break;
        }
        klog("[f] "); klog_num(proc_current()->pid); klog(" "); klog(proc_current()->name);
        klog(" nr="); klog_num(nr); klog(" ret="); klog_num(ret);
        if (ps && ustr_ok(ps)) { klog(" "); klog(ps); }
        klog("\r\n");
    }
    /* execve and sigreturn have already installed the registers to return with. */
    bool keep = (nr == 59 && ret >= 0) || (nr == 15 && ret == 0);
    if (!keep) r->rax = (uint64_t)ret;
    if (tp && tp->tracer && tp->pt_sys && tp == proc_current()) {
        pt_syscall_stop(tp, r, true);
        if (!keep) ret = (int64_t)r->rax;
    }
    proc_deliver_signal(r, keep ? -1 : (int)nr, keep ? 0 : (int32_t)ret);
}

/* syscalls that run without the big lock (see syscall_enter). everything in here
   must be fine with other cpus running in the kernel at the same time */
uint8_t nobkl_tab[512];

int64_t syscall_nobkl(regs_t* r) {
    uint64_t nr = r->rax, a = r->rdi, b = r->rsi, c = r->rdx;
    proc_t* p = proc_current();
    if (!p || g_strace || g_ftrace || p->alarm_at || p->tracer) return NB_SLOW;
    int64_t ret;
    if (setjmp((void*)p->ujb)) {
        p->ujb_on = false;
        return -EFAULT;
    }
    p->ujb_on = true;
    switch (nr) {
        case 39: ret = p->tgid; break;
        case 186: ret = p->pid; break;
        case 102: case 104: case 107: case 108: ret = 0; break;
        case 96:
            ret = 0;
            if (a) {
                UCHK2((void*)a, 16);
                uint32_t s, ns; clock_now(&s, &ns);
                ((int64_t*)a)[0] = s; ((int64_t*)a)[1] = ns / 1000;
            }
            if (b) { UCHK2((void*)b, 8); memset((void*)b, 0, 8); }
            break;
        case 228: ret = do_clock_gettime((int)a, (int64_t*)b); break;
        case 229:
            if (b) { UCHK2((void*)b, 16); memset((void*)b, 0, 16); ((int64_t*)b)[1] = 1000000; }
            ret = 0;
            break;
        case 24: task_yield_fast(); ret = 0; break;
        case 35: case 230: {
            const int64_t* ts = (const int64_t*)(nr == 35 ? a : c);
            UCHK2(ts, 16);
            uint32_t ms;
            if (nr == 230 && (b & 1)) {
                uint32_t s, ns; clock_now(&s, &ns);
                ms = ts[0] > s ? (ts[0] - s) * 1000 : 0;
            } else ms = ts[0] * 1000 + ts[1] / 1000000;
            uint32_t end = pit_uptime_ms() + ms;
            ret = 0;
            while ((int32_t)(pit_uptime_ms() - end) < 0) {
                if (proc_signal_deliverable(p)) { ret = -EINTR; break; }
                uint32_t left = end - pit_uptime_ms();
                task_sleep_ms(left > 20 ? 20 : left);
            }
            break;
        }
        case 202: ret = do_futex(a, (uint32_t)b, (uint32_t)c, r->r10, r->r8, (uint32_t)r->r9); break;
        case 0: case 1: {                            /* pipes and eventfd only */
            file_t* f = getf_ref((int)a);
            if (!f || (f->type != F_PIPE_R && f->type != F_PIPE_W && f->type != F_EVENTFD)) { file_close(f); ret = NB_SLOW; break; }
            if (nr == 0 ? f->type == F_PIPE_W : f->type == F_PIPE_R) ret = -EBADF;
            else if (!uok((void*)b, c)) ret = -EFAULT;
            else ret = nr == 0 ? file_read(f, (char*)b, c) : file_write(f, (const char*)b, c);
            file_close(f);
            break;
        }
        case 9: {                                    /* anon only, the rest keeps the lock */
            if (!(r->r10 & MAP_ANON)) { ret = NB_SLOW; break; }
            uint64_t len = (b + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (!b) { ret = -EINVAL; break; }
            bool fx = r->r10 & MAP_FIXED;
            if (fx && ((a & (PAGE_SIZE - 1)) || a < USER_BASE || a + len > USER_TOP || a + len < a)) { ret = -EINVAL; break; }
            uint64_t m = vmm_map_anon(p->pd, a, fx, USER_MMAP_BASE, USER_STACK_TOP - USER_STACK_MAX, len, (c & PROT_WRITE) != 0, c != 0);
            ret = m ? (int64_t)m : -ENOMEM;
            break;
        }
        case 11:
            if ((a & (PAGE_SIZE - 1)) || !b) ret = -EINVAL;
            else { if (a >= USER_BASE && a < USER_TOP) vmm_free_range(p->pd, a, b); ret = 0; }
            break;
        case 10:
            if (a & (PAGE_SIZE - 1)) ret = -EINVAL;
            else {
                if (a >= USER_BASE && a < USER_TOP) {
                    vmm_set_writable(p->pd, a, b, (c & PROT_WRITE) != 0);
                    vmm_set_user(p->pd, a, b, c != 0);
                }
                ret = 0;
            }
            break;
        default: ret = NB_SLOW;
    }
    p->ujb_on = false;      /* proc_current() can be NULL here when the kill got us mid syscall */
    return ret;
}

void syscall_init(void) {
    fs_free_hook = shm_drop_ino;
    static const uint16_t nb[] = { 0, 1, 9, 10, 11, 39, 186, 102, 104, 107, 108, 96, 228, 229, 24, 35, 230, 202 };
    for (unsigned i = 0; i < sizeof(nb) / sizeof(nb[0]); i++) nobkl_tab[nb[i]] = 1;
}
