/* Linux i386 system call ABI (int 0x80): eax = number, args in ebx, ecx,
   edx, esi, edi, ebp; result (or -errno) back in eax. Only what static musl
   binaries such as busybox actually need is implemented; everything else
   answers -ENOSYS, which musl and busybox handle gracefully. */

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
int  g_strace_pid;                      /* only this pid ("strace=N" on the cmdline), 0 = all */

/* ---------------- serial ---------------- */

static void com_putc(char c) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, c); }
static void klog(const char* s) { while (*s) com_putc(*s++); }
static void klog_num(int32_t v) { char b[16]; itoa(v, b, 10); klog(b); }

/* ---------------- user memory checks ---------------- */

static bool uok(const void* p, uint32_t len) {
    uint32_t a = (uint32_t)p;
    if (a < USER_BASE || a >= USER_TOP || len > USER_TOP - a) return false;
    /* pages have to be there too, otherwise teh kernel faults on them.
       the stack is grown lazily by the fault handler so skip it */
    uint32_t pd = proc_current()->pd;
    for (uint32_t pg = a & ~(PAGE_SIZE - 1); pg < a + len; pg += PAGE_SIZE) {
        if (pg >= USER_STACK_TOP - USER_STACK_MAX) break;
        if (!(vmm_pte(pd, pg) & PTE_P)) return false;
    }
    return true;
}
#define UCHK(p, n) do { if (!uok((p), (n))) return -EFAULT; } while (0)

/* user string: walk it page by page till the NUL */
static bool ustr_ok(const char* s) {
    uint32_t a = (uint32_t)s;
    for (;;) {
        if (!uok((const void*)a, 1)) return false;
        uint32_t end = (a | (PAGE_SIZE - 1)) + 1;
        for (; a < end; a++) if (!*(const char*)a) return true;
    }
}

/* ---------------- fds + paths ---------------- */

static proc_t* me(void) { return proc_current(); }

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
    uint32_t fl = irq_save();                /* threads share the table */
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
    st->st_ino = st->st_ino32 = ((uint32_t)f >> 4) & 0x0FFFFFFF;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (f->type == F_PIPE_R || f->type == F_PIPE_W) st->st_mode = S_IFIFO | 0600;
    else if (f->type == F_SOCKET || f->type == F_SPAIR || f->type == F_NETLINK || f->type == F_USOCK || f->type == F_ULISTEN) st->st_mode = 0140000 | 0777;          /* S_IFSOCK */
    else {
        st->st_mode = S_IFCHR | 0666;
        /* input: major 13, minor 64+n like /dev/input/eventN. evdev compares
           st_rdev and threw the mouse out as a duplicate of the keyboard */
        st->st_rdev = f->type == F_TTY ? (5u << 8) : f->type == F_INPUT ? (13u << 8) | (64u + (uint32_t)f->disk)
                                                                       : (1u << 8) | 3;
    }
    st->st_atime = st->st_mtime = st->st_ctime = clock_epoch();
}

/* readlink: the ramfs has no symlinks, but /proc/self/fd/N names what an
   fd refers to (musl's ttyname() relies on it; ssh servers use ttyname). */
static int do_readlink(const char* path, char* buf, uint32_t n) {
    UCHK(path, 1);
    UCHK(buf, n);
    {
        int err;
        fs_node_t* ln = lookup_peek(AT_FDCWD, path, &err, false);
        if (ln && ln->type == FS_LINK) {
            uint32_t k = ln->size < n ? (uint32_t)ln->size : n;
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
        uint32_t k = (uint32_t)strlen(x);
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
    uint32_t l = strlen(out);
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
        d[18] = node->dev ? 2 : node->type == FS_DIR ? 4 : node->type == FS_LINK ? 10 : 8;
        memcpy(d + 19, name, nl + 1);
        pos += reclen;
        f->off = idx + 1;
    }
    return (int)pos;
}

static bool is_shm(fs_node_t* n);
static void shm_drop(fs_node_t* n);

static int do_unlink(int dirfd, const char* path, int flags) {
    UCHK(path, 1);
    int err;
    fs_node_t* n = lookup_peek(dirfd, path, &err, false);
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
    if (is_shm(n)) shm_drop(n);       // mappings keep their own refs
    fs_detach(n);
    if (n->refs > 0) n->unlinked = true;
    else { fs_data_free(n); kfree(n); }
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
    return 0;
}

static bool is_ancestor(fs_node_t* a, fs_node_t* n) {
    for (; n; n = n->parent) if (n == a) return true;
    return false;
}

static int do_rename(int ofd, const char* from, int nfd, const char* to) {
    UCHK(from, 1); UCHK(to, 1);
    int err;
    fs_node_t* src = lookup_peek(ofd, from, &err, false);
    if (!src) return err;
    char name[FS_NAME_MAX];
    fs_node_t* parent = lookup_parent(nfd, to, name, &err);
    if (!parent) return err;
    // to another volume: lazy ext2 files have to come along in memory, fat sync reads ->data
    if (fs_owner(src->parent) != fs_owner(parent)) fs_need_tree(src);
    if (src->type == FS_DIR && is_ancestor(src, parent)) return -EINVAL;
    fs_node_t* dst = fs_child(parent, name);
    if (dst == src) return 0;
    if (dst) {
        if (dst->type == FS_DIR && src->type != FS_DIR) return -EISDIR;
        if (dst->type != FS_DIR && src->type == FS_DIR) return -ENOTDIR;
        if (dst->type == FS_DIR && dst->child) return -ENOTEMPTY;
        // was AT_FDCWD: apk renames relative to a dirfd of /, from /root that missed
        int r = do_unlink(nfd, to, dst->type == FS_DIR ? AT_REMOVEDIR : 0);
        if (r < 0) return r;
    }
    fs_detach(src);
    strncpy(src->name, name, FS_NAME_MAX - 1);
    src->name[FS_NAME_MAX - 1] = 0;
    fs_attach(parent, src);
    return 0;
}

/* no real hard links, a node has one parent. so link = copy. apk wants it for
   terminfo (vt220 -> vt220-am and co), nobody here cares the inode differs */
static int do_link(int ofd, const char* from, int nfd, const char* to, int flags) {
    UCHK(from, 1); UCHK(to, 1);
    int err;
    fs_node_t* src = lookup_ex(ofd, from, &err, (flags & 0x400) != 0);   /* AT_SYMLINK_FOLLOW */
    if (!src) return err;
    if (src->type == FS_DIR) return -EPERM;
    char name[FS_NAME_MAX];
    fs_node_t* par = lookup_parent(nfd, to, name, &err);
    if (!par) return err;
    if (fs_child(par, name)) return -EEXIST;
    if (src->type == FS_LINK) return fs_symlink(par, name, src->data) ? 0 : -ENOMEM;
    fs_node_t* n = fs_create(par, name, FS_FILE);
    if (!n) return -ENOMEM;
    if (src->size && fs_write(n, src->data, src->size) < 0) return -ENOMEM;
    n->mode = src->mode;
    n->dev = src->dev;
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
    if (me()->sh->fds[nfd]) file_close(me()->sh->fds[nfd]);
    file_ref(f);
    me()->sh->fds[nfd] = f;
    me()->sh->cloexec[nfd] = cloexec;
    return nfd;
}

static int do_fcntl(int fd, int cmd, uint32_t arg) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    switch (cmd) {
        case 0:    return do_dup(fd, (int)arg, false);          /* F_DUPFD */
        case 1030: return do_dup(fd, (int)arg, true);           /* F_DUPFD_CLOEXEC */
        case 1:    return me()->sh->cloexec[fd] ? 1 : 0;            /* F_GETFD */
        case 2:    me()->sh->cloexec[fd] = arg & 1; return 0;       /* F_SETFD */
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
    if (b < 0) { file_close(me()->sh->fds[a]); me()->sh->fds[a] = NULL; return b; }
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
        if (f->type == F_PIPE_R || f->type == F_SPAIR) n = f->pipe->count;
        else if (f->type == F_NODE && f->node->type == FS_FILE && f->off < f->node->size)
            n = (int)(f->node->size - f->off);
        else if (f->type == F_TTY) n = tty_readable() ? 1 : 0;
        else if (f->type == F_PTM || f->type == F_PTS) n = pty_pending(f->pty, f->type == F_PTM);
        *(int*)arg = n;
        return 0;
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

static uint32_t do_brk(uint32_t want) {
    proc_t* p = me();
    if (want < p->sh->brk_start || want >= USER_MMAP_BASE) return p->sh->brk;
    uint32_t old_top = (p->sh->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_top = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
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
static struct { fs_node_t* node; uint32_t* fr; uint32_t n; } shm[SHM_MAX];

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

static uint32_t* shm_frames(fs_node_t* n, uint32_t pages) {
    int s = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm[i].node == n) { s = i; break; }
        if (s < 0 && !shm[i].node) s = i;
    }
    if (s < 0) return NULL;
    if (shm[s].node == n && shm[s].n >= pages) return shm[s].fr;
    uint32_t* fr = (uint32_t*)kmalloc(pages * 4);
    if (!fr) return NULL;
    uint32_t had = shm[s].node == n ? shm[s].n : 0;
    if (had) memcpy(fr, shm[s].fr, had * 4);
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

/* SysV shm through ipc(117), musl on i386 goes that way. a segment is a
   dummy node in the shm[] table above, so every shmat maps the same frames.
   for MIT-SHM: glxgears sent every frame down the socket without it */
#define SV_MAX 64
static struct { int key; uint32_t size; fs_node_t* node; int nattch; bool rm; int cpid; uint16_t mode; } sv[SV_MAX];
static struct { int tgid; uint32_t addr, len; int seg; } sva[128];

static void sv_free(int i) {
    shm_drop(sv[i].node);
    kfree(sv[i].node);
    sv[i].node = NULL;
}

static int do_ipc(uint32_t call, int first, uint32_t second, uint32_t third, uint32_t ptr) {
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
        case 21: {                                                    /* shmat(id, flags, &ret, addr) */
            int i = first - 1;
            if (i < 0 || i >= SV_MAX || !sv[i].node) return -EINVAL;
            UCHK((void*)third, 4);
            uint32_t len = (sv[i].size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), addr = ptr;
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
            uint32_t* fr = shm_frames(sv[i].node, len / PAGE_SIZE);
            if (!fr) return -ENOMEM;
            for (uint32_t k = 0; k < len / PAGE_SIZE; k++)
                vmm_map_frame(p->pd, addr + k * PAGE_SIZE, fr[k], !(second & 010000));   /* SHM_RDONLY */
            vmm_flush();
            sva[slot].tgid = p->tgid; sva[slot].addr = addr; sva[slot].len = len; sva[slot].seg = i;
            sv[i].nattch++;
            *(uint32_t*)third = addr;
            return 0;
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
            if (cmd == 2 || cmd == 13) {                              /* IPC_STAT, SHM_STAT: shmid64_ds */
                UCHK((void*)ptr, 84);
                uint8_t* b = (uint8_t*)ptr;
                memset(b, 0, 84);
                *(int*)b = sv[i].key;
                *(uint16_t*)(b + 20) = sv[i].mode;                    /* uid/gid/cuid/cgid 0 */
                *(uint32_t*)(b + 36) = sv[i].size;
                *(uint32_t*)(b + 64) = (uint32_t)sv[i].cpid;
                *(uint32_t*)(b + 72) = (uint32_t)sv[i].nattch;
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

static int32_t do_mmap(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t off) {
    proc_t* p = me();
    if (!len) return -EINVAL;
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    file_t* f = NULL;
    if (!(flags & MAP_ANON)) {
        f = getf(fd);
        if (!f) return -EBADF;
        if (f->type != F_NODE && f->type != F_ZERO && f->type != F_FB) return -EACCES;
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
    if (f && f->type == F_NODE && (flags & MAP_SHARED) && is_shm(f->node)) {
        uint32_t first = off / PAGE_SIZE, np = len / PAGE_SIZE;
        uint32_t* fr = shm_frames(f->node, first + np);
        if (!fr) return -ENOMEM;
        for (uint32_t k = 0; k < np; k++)
            vmm_map_frame(p->pd, addr + k * PAGE_SIZE, fr[first + k], (prot & PROT_WRITE) != 0);
        vmm_flush();
        return (int32_t)addr;
    }
    if (f && f->type == F_FB) {
        /* the real lfb pages, shared. pmm_ref/unref skip frames outside the
           pool, so munmap/exit can't hand video memory out as ram. xorg fbdev */
        int pitch, bpp;
        uint32_t fb = (uint32_t)gfx_front_fb(&pitch, &bpp);
        uint32_t size = (uint32_t)pitch * (uint32_t)gfx_h();
        if (!fb || off >= size) return -EINVAL;
        for (uint32_t k = 0; k < len / PAGE_SIZE && off + k * PAGE_SIZE < size; k++)
            vmm_map_frame(p->pd, addr + k * PAGE_SIZE, fb + off + k * PAGE_SIZE, (prot & PROT_WRITE) != 0);
        vmm_flush();
        return (int32_t)addr;
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
    if (!prot) vmm_set_user(p->pd, addr, len, false);
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

static int poll_once(pollfd_t* fds, uint32_t n) {
    int ready = 0;
    for (uint32_t i = 0; i < n; i++) {
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

/* epoll on top of the same readiness checks, level triggered (EPOLLET is
   taken as level, good enough so far). xorg's ospoll has no poll fallback */
typedef struct ep {
    int n, cap;
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
    for (;;) {
        net_poll();
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
        if (timeout_ms > 0 && pit_uptime_ms() - start >= (uint32_t)timeout_ms) return 0;
        if (proc_interrupted()) return -EINTR;
        task_sleep_ms(1);
    }
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
        task_sleep_ms(1);
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
    for (;;) {
        net_poll();
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
        task_sleep_ms(1);
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
    fs_path(me()->sh->cwd, path, sizeof(path));
    uint32_t n = strlen(path) + 1;
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
static int spair_call(int call, file_t* f, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
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
            UCHK((void*)b, 28);
            uint32_t* m = (uint32_t*)b;              /* name, namelen, iov, iovlen, ctl, ctllen, flags */
            iovec_t* iov = (iovec_t*)m[2];
            UCHK(iov, m[3] * sizeof(iovec_t));
            int ofl = f->flags;
            if (c & 0x40) f->flags |= O_NONBLOCK;
            int total = 0, r = 0;
            if (call == 16 && m[4] && m[5] >= 16) {                  /* SCM_RIGHTS out: refs ride along with the bytes */
                UCHK((void*)m[4], m[5]);
                uint32_t* cm = (uint32_t*)m[4];
                if (cm[1] == 1 && cm[2] == 1)
                    for (uint32_t k = 0; k < (cm[0] - 12) / 4 && f->pipe2->nfds < 8; k++) {
                        file_t* x = getf((int)cm[3 + k]);
                        if (x) { file_ref(x); f->pipe2->fds[f->pipe2->nfds++] = x; }
                    }
            }
            for (uint32_t i = 0; i < m[3]; i++) {
                if (!iov[i].len) continue;
                UCHK((void*)iov[i].base, iov[i].len);
                r = call == 16 ? file_write(f, (const char*)iov[i].base, iov[i].len)
                               : file_read(f, (char*)iov[i].base, iov[i].len);
                if (r < 0) break;
                total += r;
                if ((uint32_t)r < iov[i].len) break;
                if (call == 17) f->flags |= O_NONBLOCK;            /* rest of the iovs: only what's there */
            }
            f->flags = ofl;
            if (r < 0 && !total) return r;
            if (call == 17) {
                uint32_t room = m[5];
                m[5] = 0; m[6] = 0;
                if (m[1]) m[1] = 0;
                pipe_t* q = f->pipe;
                if (q->nfds && m[4] && room >= 16) {                 /* SCM_RIGHTS in */
                    UCHK((void*)m[4], room);
                    uint32_t* cm = (uint32_t*)m[4];
                    uint32_t k = 0;
                    while (q->nfds && 12 + (k + 1) * 4 <= room) {
                        file_t* x = q->fds[0];
                        for (int j = 1; j < q->nfds; j++) q->fds[j - 1] = q->fds[j];
                        q->nfds--;
                        int nfd = install_fd(x, 0, (c & 0x40000000) != 0);   /* MSG_CMSG_CLOEXEC */
                        if (nfd < 0) { file_close(x); continue; }
                        cm[3 + k++] = (uint32_t)nfd;
                    }
                    cm[0] = 12 + k * 4; cm[1] = 1; cm[2] = 1;
                    m[5] = (12 + k * 4 + 3) & ~3u;
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
} ux_t;

static ux_t* ureg[32];

static int ux_name(uint32_t addr, uint32_t len, char* out, int* olen) {
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
    if (k == (int)len - 2) return -EINVAL;                     /* no NUL inside */
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

void ux_release(file_t* f) {
    ux_t* u = f->ux;
    if (!u) return;
    for (int i = 0; i < 32; i++) if (ureg[i] == u) ureg[i] = NULL;
    for (int i = 0; i < u->nq; i++) file_close(u->q[i]);
    kfree(u);
    f->ux = NULL;
}

static int ux_call(int call, file_t* f, int fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
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
            return 0;
        }
        case 5: case 18: {                                            /* accept(4) */
            if (f->type != F_ULISTEN) return -EINVAL;
            while (!u->nq) {
                if (f->flags & O_NONBLOCK) return -EAGAIN;
                if (proc_interrupted()) return -EINTR;
                task_yield();
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

static void nl_dump(pipe_t* q, int type, uint32_t seq, int af) {
    static const uint8_t zero[6], ff[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    int n = af == 10 ? -1 : net_ifcount();            /* no ipv6 here */
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
        } else if (type == 22) {                      /* RTM_GETADDR -> NEWADDR */
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

static int32_t nl_call(int call, file_t* f, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f6) {
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
        case 9: case 11: {                            /* send(to) */
            UCHK((void*)b, c);
            if (c < 17) return -EINVAL;
            const uint8_t* h = (const uint8_t*)b;
            int type = *(const uint16_t*)(h + 4);
            if (type == 18 || type == 22) nl_dump(q, type, *(const uint32_t*)(h + 8), h[16]);
            else {                                    /* anything else: error 0 = ack, good enough */
                uint8_t* m = nl_msg(q, 2, *(const uint32_t*)(h + 8), 4);
                if (m) *(uint16_t*)(m + 6) = 0;
            }
            return (int32_t)c;
        }
        case 10: case 12: case 17: {                  /* recv(from/msg) */
            uint8_t* buf; uint32_t cap;
            if (call == 17) {
                UCHK((void*)b, 28);
                uint32_t* mh = (uint32_t*)b;
                if (!mh[3]) return 0;
                iovec_t* iov = (iovec_t*)mh[2];
                UCHK(iov, sizeof(iovec_t));
                buf = (uint8_t*)iov[0].base; cap = iov[0].len;
                mh[1] = 0; mh[5] = 0; mh[6] = 0;
            } else { buf = (uint8_t*)b; cap = c; }
            if (!q->count) return -EAGAIN;
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
            return (int32_t)n;
        }
    }
    return 0;
}

static int32_t sys_socket_call(int call, uint32_t a, uint32_t b, uint32_t c,
                               uint32_t d, uint32_t e, uint32_t f6) {
    int err = 0;
    sock_t* s;
    uint32_t ip; uint16_t port;
    file_t* fl;
    if (call != 1 && call != 8) {
        file_t* pf = getf((int)a);
        if (pf && pf->type == F_SPAIR) return spair_call(call, pf, b, c, d, e);
        if (pf && pf->type == F_NETLINK) return nl_call(call, pf, b, c, d, e, f6);
        if (pf && (pf->type == F_USOCK || pf->type == F_ULISTEN)) return ux_call(call, pf, (int)a, b, c, d, e);
    }
    switch (call) {
        case 1: {                                                     /* socket */
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
        case 8: return do_socketpair(a, b, c, (int*)d);               /* socketpair */
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
                /* was &len on the kernel stack, UCHK said no and the name never got written.
                   musl 1.2.5 dns drops replies without it (apk) */
                if (m[0]) write_addr(m[0], (uint32_t)&m[1], ip, port);
                m[5] = 0; m[6] = 0;
            }
            return total;
        }
    }
    return -EINVAL;
}

/* ---------------- dispatcher ---------------- */

static int do_futex(uint32_t uaddr, uint32_t op, uint32_t val, uint32_t d, uint32_t uaddr2, uint32_t val3, bool t64) {
    int cmd = op & 127;
    uint32_t tmo = 0xFFFFFFFFu;
    if (d && (cmd == 0 || cmd == 9)) {
        UCHK((void*)d, t64 ? 16 : 8);
        uint32_t* ts = (uint32_t*)d;
        uint64_t ms = (uint64_t)ts[0] * 1000 + (t64 ? ts[2] : ts[1]) / 1000000;
        if (cmd == 9) {                                    /* absolute deadline */
            uint64_t now;
            if (op & 256) { uint32_t s, ns; clock_now(&s, &ns); now = (uint64_t)s * 1000 + ns / 1000000; }
            else now = pit_uptime_ms();
            ms = ms > now ? ms - now : 0;
        }
        if (ms < 0xFFFFFFF0ull) tmo = (uint32_t)ms;
    } else if (cmd == 3 || cmd == 4) tmo = d;              /* val2 */
    return futex_op(uaddr, op, val, tmo, uaddr2, val3);
}

static int32_t dispatch(regs_t* r) {
    uint32_t a = r->ebx, b = r->ecx, c = r->edx, d = r->esi, e = r->edi, f6 = r->ebp;
    proc_t* p = me();
    int err;
    fs_node_t* n;
    switch (r->eax) {
        case 1:   proc_thread_exit((int)((a & 0xFF) << 8));
        case 252: proc_exit((int)((a & 0xFF) << 8));
        case 2:   return proc_fork(r);
        case 190: return proc_vfork(r);
        case 120: return proc_clone(r);
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
            p->sh->fds[a] = NULL;
            file_close(fl);
            return 0;
        }
        case 7:   return do_wait((int)a, (int*)b, (int)c);
        case 114: return do_wait((int)a, (int*)b, (int)c);
        case 83: case 304: {                                          /* symlink / symlinkat */
            const char* tg = (const char*)a;
            bool at = r->eax == 304;
            const char* lp = at ? (const char*)c : (const char*)b;
            int dfd = at ? (int)b : AT_FDCWD;
            UCHK(tg, 1); UCHK(lp, 1);
            char name[FS_NAME_MAX];
            fs_node_t* par = lookup_parent(dfd, lp, name, &err);
            if (!par) return err;
            if (fs_child(par, name)) return -EEXIST;
            return fs_symlink(par, name, tg) ? 0 : -ENOMEM;
        }
        case 9:   return do_link(AT_FDCWD, (const char*)a, AT_FDCWD, (const char*)b, 0);
        case 303: return do_link((int)a, (const char*)b, (int)c, (const char*)d, (int)e);
        case 14: case 297: return -EPERM;                             /* mknod */
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
            n = r->eax == 15 ? lookup_peek(AT_FDCWD, (const char*)a, &err, true) : lookup_peek((int)a, (const char*)b, &err, true);
            if (!n) return err;
            n->mode = (uint16_t)((r->eax == 15 ? b : c) & 07777);
            return 0;
        case 94: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type == F_NODE) fl->node->mode = (uint16_t)(b & 07777);
            return 0;
        }
        case 186:                                                    /* sigaltstack */
            if (a) UCHK((void*)a, 12);
            if (b) UCHK((void*)b, 12);
            return proc_sigaltstack((const uint32_t*)a, (uint32_t*)b, r->useresp);
        case 215: case 216: return 0;                                /* setfsuid32/gid32: zsh. we're root anyway */
        case 143: return 0;                                          /* flock, apk wants it. nobody fights for locks here */
        case 36: case 118: case 148: case 344:                       /* sync, fsync, fdatasync, syncfs */
            ext2_sync_all();
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
            if (!FS_DEV_IS_DISK(src->dev)) return -15;                  /* ENOTBLK */
            fs_node_t* dst = lookup(AT_FDCWD, (const char*)b, &err);
            if (!dst) return err;
            return fatfs_mount(src->dev - FS_DEV_DISK, dst);
        }
        case 22: case 52: {                                          /* umount / umount2 */
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
        case 20:  return p->tgid;
        case 224: return p->pid;                                     /* gettid */
        case 64: { proc_t* l = p->is_thread ? proc_by_pid(p->tgid) : p; int pp = l ? l->ppid : p->ppid; return pp ? pp : 1; }
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
            return t ? proc_send_signal_tid(t, sig) : -ESRCH;
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
            t->pgid = b ? (int)b : t->tgid;
            return 0;
        }
        case 65:  return p->pgid;
        case 132: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->pgid : -ESRCH; }
        case 147: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->sid : -ESRCH; }
        case 66:  p->sid = p->pgid = p->tgid; p->ctty = -1; return p->tgid;   /* setsid: no terminal */
        case 60:  { int old = p->sh->umask; p->sh->umask = (int)(a & 0777); return old; }
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
        case 85:  return do_readlink((const char*)a, (char*)b, c);
        case 305: return do_readlink((const char*)b, (char*)c, d);   /* readlinkat (absolute) */
        case 80: case 205: return 0;                                 /* getgroups: none */
        case 90: {                                                   /* old mmap(struct*) */
            UCHK((void*)a, 24);
            uint32_t* m = (uint32_t*)a;
            if (m[5] & (PAGE_SIZE - 1)) return -EINVAL;
            return do_mmap(m[0], m[1], (int)m[2], (int)m[3], (int)m[4], m[5]);
        }
        case 192: return do_mmap(a, b, (int)c, (int)d, (int)e, f6 * PAGE_SIZE);
        case 91:  return do_munmap(a, b);
        case 125: return do_mprotect(a, b, (int)c);
        case 163: {                                                  /* mremap: never moves, musl falls back */
            /* but musl's pthread_getattr_np walks down the main stack with it until
               the answer isn't ENOMEM. always ENOMEM = node spun through all 4 GB.
               the whole 8 MB stack window counts as mapped, it grows on demand */
            bool stk = a >= USER_STACK_TOP - USER_STACK_MAX && a < USER_STACK_TOP;
            if (!stk && !(vmm_pte(me()->pd, a & ~(PAGE_SIZE - 1)) & PTE_P)) return -EFAULT;
            return -ENOMEM;
        }
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
                uint32_t lim[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };   // do_rlimit UCHKs, a kernel stack ptr fails it
                if (b == 3) lim[0] = lim[1] = USER_STACK_MAX;
                if (b == 7) lim[0] = lim[1] = MAX_FDS;
                uint32_t* o = (uint32_t*)d;
                o[0] = lim[0]; o[1] = lim[0] == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0;
                o[2] = lim[1]; o[3] = lim[1] == 0xFFFFFFFFu ? 0xFFFFFFFFu : 0;
            }
            return 0;
        }
        case 195: case 196:                                          /* stat64 / lstat64 */
            UCHK((void*)a, 1); UCHK((void*)b, sizeof(kstat64_t));
            n = lookup_peek(AT_FDCWD, (const char*)a, &err, r->eax == 195);
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
            n = lookup_peek((int)a, (const char*)b, &err, !(d & 0x100));   /* AT_SYMLINK_NOFOLLOW */
            if (!n) return err;
            fill_stat_node((kstat64_t*)c, n);
            return 0;
        }
        case 220: return do_getdents64((int)a, (uint8_t*)b, c);
        case 240: return do_futex(a, b, c, d, e, f6, false);
        case 422: return do_futex(a, b, c, d, e, f6, true);       /* futex_time64 */
        case 219: return 0;                                          /* madvise: advisory */
        case 117: return do_ipc(a, (int)b, c, d, e);                 /* ipc: shm* */
        case 184: {                                                  /* capget: root, every cap there is */
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
        case 185: return 0;                                          /* capset: sure */
        case 272: case 250: return 0;                                /* fadvise64(_64): advice taken, ignored */
        case 324: return -95;                                        /* fallocate, apk asks. EOPNOTSUPP and it just writes */
        // eventfd(2), timerfd, signalfd(4), epoll*, memfd: not here yet. glib/qemu fall back
        // to pipes and poll on ENOSYS, so just say no without spamming the log
        case 321: case 327: case 375:                                /* signalfd(4), membarrier: not yet */
            return -ENOSYS;
        case 323: case 328: {                                        /* eventfd(2) */
            int fl = r->eax == 328 ? (int)b : 0;
            file_t* f = file_new(F_EVENTFD, 2 | ((fl & 04000) ? O_NONBLOCK : 0) | ((fl & 1) ? 0x10000000 : 0));
            if (!f) return -ENOMEM;
            f->cnt = a;
            return install_fd(f, 0, (fl & 02000000) != 0);
        }
        case 322: {                                                  /* timerfd_create */
            file_t* f = file_new(F_TIMERFD, 2 | (((int)b & 04000) ? O_NONBLOCK : 0));
            if (!f) return -ENOMEM;
            f->disk = (int)a;                                        /* clock id */
            return install_fd(f, 0, (b & 02000000) != 0);
        }
        case 325: case 411: case 326: case 410: {                   /* timerfd_settime(64) / gettime(64) */
            file_t* f = getf((int)a);
            if (!f) return -EBADF;
            if (f->type != F_TIMERFD) return -EINVAL;
            bool t64 = r->eax == 411 || r->eax == 410, set = r->eax == 325 || r->eax == 411;
            uint32_t sz = t64 ? 32 : 16, now = pit_uptime_ms();
            uint32_t* oldp = (uint32_t*)(set ? d : b);
            if (oldp) {                                              /* old / current: interval, value left */
                UCHK(oldp, sz);
                uint32_t left = f->t_next && (int32_t)(f->t_next - now) > 0 ? f->t_next - now : (f->t_next ? 1 : 0);
                uint32_t v[4] = { f->t_int / 1000, (f->t_int % 1000) * 1000000u, left / 1000, (left % 1000) * 1000000u };
                if (t64) { for (int k = 0; k < 4; k++) { oldp[k * 2] = v[k]; oldp[k * 2 + 1] = 0; } }
                else memcpy(oldp, v, 16);
            }
            if (!set) return 0;
            UCHK((void*)c, sz);
            uint32_t* nv = (uint32_t*)c;
            uint32_t is = nv[0], ins = nv[t64 ? 2 : 1], vs = nv[t64 ? 4 : 2], vns = nv[t64 ? 6 : 3];
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
        case 356: {                                                  /* memfd_create: a node nobody can find by name */
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
        case 254: return do_epoll_create(0);                         /* epoll_create(size) */
        case 329: return do_epoll_create((int)a);                    /* epoll_create1 */
        case 255: return do_epoll_ctl((int)a, (int)b, (int)c, (uint32_t*)d);
        case 256: case 319: return do_epoll_wait((int)a, (uint32_t*)b, (int)c, (int)d);   /* pwait: mask ignored */
        case 242:                                                    /* sched_getaffinity: one cpu, no smp yet */
            if (c < 4) return -EINVAL;
            UCHK((void*)d, c);
            memset((void*)d, 0, c);
            *(uint32_t*)d = 1;
            return 4;
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
            n = lookup_peek(dfd, path, &err, true);
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
static bool changes_fs(uint32_t nr, uint32_t a) {
    switch (nr) {
        case 4: case 146: case 181: case 334: case 93: case 194: case 324: case 94: {
            file_t* f = getf((int)a);
            return f && f->type == F_NODE;
        }
        case 5: case 8: case 295: case 10: case 301: case 38: case 302: case 353:
        case 39: case 296: case 40: case 83: case 304: case 9: case 303:
        case 92: case 193: case 15: case 306: case 30: case 271: case 320: case 412:
            return true;
    }
    return false;
}

void syscall_dispatch(regs_t* r) {
    uint32_t nr = r->eax;
    proc_check_alarm(proc_current(), false);
    bool mut = changes_fs(nr, r->ebx);
    if (mut) {
        ext2_throttle();                      /* before: the sync waits for writers */
        fs_write_begin();
    }
    int32_t ret = dispatch(r);
    if (mut) fs_write_end();
    if (g_strace && g_strace_pid && proc_current() && proc_current()->pid == g_strace_pid) {
        /* buffered: record now, print when the process exits (timing stays intact) */
        static struct { int32_t nr, a, b, c, ret; } rec[4096];
        static int nrec;
        if (nrec < 4096) { rec[nrec].nr = (int32_t)nr; rec[nrec].a = (int32_t)r->ebx;
                           rec[nrec].b = (int32_t)r->ecx; rec[nrec].c = (int32_t)r->edx; rec[nrec++].ret = ret; }
        if (nr == 1 || nr == 252) {                                  /* exit: print the log */
            for (int i = 0; i < nrec; i++) {
                klog("[st] "); klog_num(rec[i].nr); klog("("); klog_num(rec[i].a); klog(", ");
                klog_num(rec[i].b); klog(", "); klog_num(rec[i].c); klog(") = "); klog_num(rec[i].ret); klog("\r\n");
            }
            nrec = 0;
        }
    } else if (g_strace && !g_strace_pid) {
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
    fs_free_hook = shm_drop;
}
