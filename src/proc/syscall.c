#include "core/lat.h"
#include "boot/acpi.h"
/* Linux x86_64 system call ABI (syscall insn): rax = number, args in rdi,
   rsi, rdx, r10, r8, r9; result (or -errno) back in rax. Only what static
   musl binaries such as busybox actually need is implemented; everything
   else answers -ENOSYS, which musl and busybox handle gracefully. */

#include "core/pcache.h"
#include "core/swap.h"
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
#include "fs/part.h"
#include "net/sock.h"
#include "fs/fatfs.h"
#include "fs/ext2.h"
#include "fs/mount.h"
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
#define EROFS 30
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
    if (!proc_current()) return false;      // killed under us (exit_group from another cpu)
    uint64_t pd = proc_current()->pd;
    for (uint64_t pg = a & ~(PAGE_SIZE - 1); pg < a + len; pg += PAGE_SIZE) {
        if (pg >= USER_STACK_TOP - USER_STACK_MAX) break;
        uint64_t pte = vmm_pte(pd, pg);
        if (!(pte & PTE_P) && !((pte & (PTE_LAZY | PTE_SWAP)) && (pte & PTE_US))) return false;   /* lazy: the fault fills it */
    }
    return true;
}
bool user_ok(const void* p, uint32_t len) { return uok(p, len); }
#define UCHK2(p, n) do { if (!uok((p), (n))) { if (proc_current()) proc_current()->ujb_on = false; return -EFAULT; } } while (0)
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
    if (fd < 0 || fd >= MAX_FDS || !proc_current()) return NULL;
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
    if (fd < 0 || fd >= MAX_FDS || !me() || !me()->sh) return NULL;     // killed from another cpu mid-syscall
    return me()->sh->fds[fd];
}

static int alloc_fd(int from) {
    uint64_t nf = me()->sh->rl[7][0];
    int top = nf < MAX_FDS ? (int)nf : MAX_FDS;
    for (int i = from < 0 ? 0 : from; i < top; i++)
        if (!me()->sh->fds[i]) return i;
    return -EMFILE;
}

static int install_fd(file_t* f, int from, bool cloexec) {
    uint64_t fl = spin_lock(&fdl);           /* threads share the table */
    int fd = alloc_fd(from);
    if (fd >= 0) {
        me()->sh->fds[fd] = f;
        me()->sh->cloexec[fd] = cloexec;
    }
    spin_unlock(&fdl, fl);
    if (fd < 0) file_close(f);
    return fd;
}

/* execbuffer fence fds: eventfds that go readable once the host is done */
static struct { uint64_t f; file_t* fl; } xf[16];

static void xf_fire(file_t* f) {
    __atomic_add_fetch(&f->cnt, 1, __ATOMIC_ACQ_REL);
    efd_wake();
    wq_wake(&f->wq);
}

int drm_fences(uint64_t done) {
    int left = 0;
    for (int i = 0; i < 16; i++) {
        if (!xf[i].fl) continue;
        if (xf[i].f > done) { left++; continue; }
        file_t* f = xf[i].fl;
        xf[i].fl = NULL;
        xf_fire(f);
        file_close(f);
    }
    return left;
}

static fs_node_t* dir_base(int dirfd, const char* path, int* err) {
    if (path[0] == '/' || dirfd == AT_FDCWD) return me()->sh->cwd;
    file_t* f = getf(dirfd);
    if (!f) { *err = -EBADF; return NULL; }
    if (f->type != F_NODE || f->node->type != FS_DIR) { *err = -ENOTDIR; return NULL; }
    return me()->mntns && f->dent ? fs_alias(f->dent) : f->node;
}

/* /proc is regenerated whenever a path lookup might land in it. */
static bool in_procfs(fs_node_t* n) {
    for (; n; n = n->parent)
        if (n->parent && (n->parent == fs_root() || n->parent->mode_shadow) && !strcmp(n->name, "proc")) return true;
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
    if (n->type == FS_DIR && !strcmp(n->name, "task")) {         // chromium CHECKs nlink > 2 on /proc/self/task
        st->st_nlink = 2;
        for (fs_node_t* c = fs_alias(n)->child; c; c = c->next) st->st_nlink++;   // alias: in a private mnt ns n is only the dentry
        if (st->st_nlink < 3) st->st_nlink = 3;
    }
    if (FS_DEV_IS_DISK(n->dev)) {
        int idx = n->dev - FS_DEV_DISK;
        st->st_mode = 0060000 | (n->mode & 07777);            /* S_IFBLK */
        st->st_rdev = (uint32_t)ata_rdev(idx);
        st->st_size = (int64_t)ata_drive_sectors(idx) * 512;
    } else if (n->dev == FS_DEV_SOCK) {
        st->st_mode = 0140000 | (n->mode & 07777);                 /* S_IFSOCK */
    } else if (n->dev == FS_DEV_FIFO) {
        st->st_mode = S_IFIFO | (n->mode & 07777);
    } else if (n->dev) {
        st->st_mode = S_IFCHR | (n->mode & 07777);
        st->st_rdev = n->dev == FS_DEV_TTY  ? (5u << 8) :
                      n->dev == FS_DEV_PTMX ? ((5u << 8) | 2) :
                      n->dev == FS_DEV_SNDC ? (116u << 8) : n->dev == FS_DEV_SNDP ? ((116u << 8) | 16) :
                      n->dev == FS_DEV_DRM ? (226u << 8) : n->dev == FS_DEV_DRMR ? ((226u << 8) | 128) :
                      FS_DEV_IS_EVENT(n->dev) ? ((13u << 8) | (uint32_t)(64 + n->dev - FS_DEV_EVENT)) :
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
    else if (f->type == F_DISK) {
        st->st_mode = 0060660;
        st->st_rdev = (uint32_t)ata_rdev(f->disk);
        st->st_size = (int64_t)ata_drive_sectors(f->disk) * 512;
    } else {
        st->st_mode = S_IFCHR | 0666;
        /* input: major 13, minor 64+n like /dev/input/eventN. evdev compares
           st_rdev and threw the mouse out as a duplicate of the keyboard */
        st->st_rdev = f->type == F_SND ? (116u << 8) | (f->disk ? 16u : 0) : f->type == F_DRM ? (226u << 8) | (f->disk ? 128u : 0) : f->type == F_TTY ? (5u << 8) : f->type == F_INPUT ? (13u << 8) | (f->disk >= 3 ? 61u + (uint32_t)f->disk : 96u + (uint32_t)f->disk)
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
        if (!ln && err == -ENOENT && strncmp(path, "/proc/", 6)) return -ENOENT;
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
    if (f->type != F_NODE && f->dent) { fs_path(f->dent, out, sizeof(out)); goto done; }     // O_PATH device
    switch (f->type) {
        case F_NODE: if (f->fpath && me()->mntns) strcpy(out, f->fpath); else fs_path(f->node, out, sizeof(out)); break;
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
    if (me()->mntns && out[0] == '/') {                      /* /dev/null and co have no node here; asks the tree how it looks from our root */
        fs_node_t* dn = fs_peek(fs_root(), out + 1, true);
        if (dn) fs_path(dn, out, sizeof(out));
    }
done:;
    uint64_t l = strlen(out);
    if (l > n) l = n;
    memcpy(buf, out, l);
    return (int)l;
}

/* ---------------- file syscalls ---------------- */

/* open("/proc/x/ns/mnt"): a throwaway node that remembers which ns it is, for setns */
static int open_ns(const char* path, int flags) {
    if (strncmp(path, "/proc/", 6)) return 0x7fffffff;
    const char* q = path + 6;
    proc_t* t;
    if (!strncmp(q, "self/", 5)) { t = me(); q += 5; }
    else {
        int id = atoi(q);
        while (*q >= '0' && *q <= '9') q++;
        if (*q++ != '/') return 0x7fffffff;
        t = ns_pid_find(me(), id);
    }
    if (strncmp(q, "ns/", 3)) return 0x7fffffff;
    q += 3;
    if (!t) return -ENOENT;
    for (int k = 0; k < 6; k++) {
        if (strcmp(q, ns_type_name(k))) continue;
        char nm[32] = "nsfd:";
        nm[5] = (char)('0' + k); nm[6] = ':';
        itoa(ns_idx(t, k), nm + 7, 10);
        fs_node_t* nd = fs_new_file(nm);
        if (!nd) return -ENOMEM;
        nd->unlinked = true;
        file_t* f = file_open_node(nd, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
        return f ? install_fd(f, 0, (flags & O_CLOEXEC) != 0) : -ENOMEM;
    }
    return -ENOENT;
}

static int do_setns(int fd, int type) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if (f->type != F_NODE || strncmp(f->node->name, "nsfd:", 5)) return -EINVAL;
    return ns_join(f->node->name[5] - '0', atoi(f->node->name + 7));
}

/* /proc/self/fd/N as a mount source or target (bwrap) */
static fs_node_t* fd_node(const char* path, bool dent) {
    if (!ustr_ok(path) || strncmp(path, "/proc/self/fd/", 14)) return NULL;
    file_t* f = getf(atoi(path + 14));
    if (!f) return NULL;
    if (f->type != F_NODE) return f->dent;      // O_PATH on a device, see do_open
    return dent && f->dent ? f->dent : f->node;
}

static int do_open(int dirfd, const char* path, int flags, int mode) {
    UCHK(path, 1);
    int err;
    if (!ustr_ok(path)) return -EFAULT;
    { int r = open_ns(path, flags); if (r != 0x7fffffff) return r; }
    if (!strncmp(path, "/proc/self/fd/", 14)) {            /* reopen: firefox does this to memfds for the ro copy */
        int fd = atoi(path + 14);
        file_t* o = getf(fd);
        if (!o) return -ENOENT;
        file_t* nf;
        if (o->type == F_NODE) nf = file_open_node(o->node, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
        else { file_ref(o); nf = o; }
        if (!nf) return -ENOMEM;
        return install_fd(nf, 0, (flags & O_CLOEXEC) != 0);
    }
    fs_node_t* n = lookup(dirfd, path, &err);
    if (n && (flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (n && n->lazy) return -ENOMEM;                 /* ext2 couldn't read it in, no room */
    if (!n) {
        if (!(flags & O_CREAT) || err != -ENOENT) return err;
        char name[FS_NAME_MAX];
        fs_node_t* parent = lookup_parent(dirfd, path, name, &err);
        if (!parent) return err;
        // dangling symlink: O_CREAT makes the target (runit-init stopit -> /run/runit/stopit)
        for (int k = 0; k < 8; k++) {
            fs_node_t* ex = fs_child(parent, name);
            if (!ex || ex->type != FS_LINK) break;
            char full[256];
            const char* tg = (const char*)ex->data;
            if (tg[0] == '/') strncpy(full, tg, sizeof(full) - 1);
            else {
                fs_path(parent, full, 128);
                if (strlen(tg) > 120) return -ENAMETOOLONG;
                strcat(full, "/"); strcat(full, tg);
            }
            char* sl = full + strlen(full);
            while (*sl != '/') sl--;
            if (strlen(sl + 1) >= FS_NAME_MAX) return -ENAMETOOLONG;
            strcpy(name, sl + 1);
            if (sl == full) parent = fs_root();
            else { *sl = 0; parent = fs_resolve(fs_root(), full); }
            if (!parent || parent->type != FS_DIR) return -ENOENT;
        }
        if (mnt_ro(parent)) return -EROFS;
        if ((err = mnt_newnode(parent)) < 0) return err;
        n = fs_create(parent, name, FS_FILE);
        if (!n) return -EACCES;
        n->mode = (uint16_t)(mode & ~me()->sh->umask & 07777);
        ino_node(n, 0x100);
    }
    if ((flags & O_DIRECTORY) && n->type != FS_DIR) return -ENOTDIR;
    if (n->type == FS_DIR && (flags & O_ACCMODE) != 0) return -EISDIR;
    if (!n->dev && n->type == FS_FILE && ((flags & O_ACCMODE) || (flags & O_TRUNC)) && mnt_ro(n)) return -EROFS;
    if ((flags & O_TRUNC) && n->type == FS_FILE && !n->dev && (flags & O_ACCMODE)) { uint32_t os = n->size; node_truncate(n, 0); if (os) ino_node(n, 2); }
    file_t* f;
    if (n->dev == FS_DEV_FIFO) {
        int fr = fifo_open(n, flags & ~(O_CREAT | O_EXCL | O_TRUNC), &f);
        if (fr < 0) return fr;
        return install_fd(f, 0, (flags & O_CLOEXEC) != 0);
    }
    f = file_open_node(n, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
    if (!f) return n->dev == FS_DEV_TTY ? -ENXIO : -ENOMEM;     // xterm dies on ENOMEM here
    ino_node(n, 0x20);
    if (me()->mntns && (flags & 0x200000) && n->type != FS_DIR) f->dent = n;
    if (me()->mntns && n->type == FS_DIR) {
        file_t* df = dirfd == AT_FDCWD || path[0] == '/' ? NULL : getf(dirfd);
        fs_node_t* bs = df ? df->node : me()->sh->cwd;
        f->dent = fs_dent(bs, path, df ? df->dent : NULL, (flags & 0x200000) != 0);
        char full[256], out[256];
        if (path[0] == '/') strcpy(full, path);
        else {
            if (df && df->fpath) strcpy(full, df->fpath); else fs_path(bs, full, sizeof(full));
            if (strlen(full) + strlen(path) < 250) { strcat(full, "/"); strcat(full, path); }
        }
        int ol = 0;                                                   /* squash //, . and .. */
        for (char* q = full; *q; ) {
            while (*q == '/') q++;
            char* e = q; while (*e && *e != '/') e++;
            int l = (int)(e - q);
            if (l == 2 && q[0] == '.' && q[1] == '.') { while (ol > 0 && out[ol - 1] != '/') ol--; if (ol > 0) ol--; }
            else if (l && !(l == 1 && q[0] == '.')) { out[ol++] = '/'; memcpy(out + ol, q, l); ol += l; }
            q = e;
        }
        if (!ol) out[ol++] = '/';
        out[ol] = 0;
        f->fpath = (char*)kmalloc(ol + 1);
        if (f->fpath) strcpy(f->fpath, out);
    }
    return install_fd(f, 0, (flags & O_CLOEXEC) != 0);
}

static int do_read(int fd, char* buf, uint64_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    UCHK(buf, n);
    return file_read(f, buf, n);
}

static bool is_idmap(fs_node_t* nd) {
    return in_procfs(nd) && (!strcmp(nd->name, "uid_map") || !strcmp(nd->name, "gid_map") || !strcmp(nd->name, "setgroups"));
}

static int idmap_write(fs_node_t* nd, const char* buf, uint64_t n) {
    char tmp[256];
    if (n > 255) return -EINVAL;
    memcpy(tmp, buf, n);
    tmp[n] = 0;
    const char* dn = nd->parent->name;
    proc_t* t = !strcmp(dn, "self") ? me() : ns_pid_find(me(), atoi(dn));
    if (!t) return -ESRCH;
    int what = !strcmp(nd->name, "uid_map") ? 0 : !strcmp(nd->name, "gid_map") ? 1 : 2;
    int e = ns_idmap(t, what, tmp, (int)n);
    return e < 0 ? e : (int)n;
}

static int do_write(int fd, const char* buf, uint64_t n) {
    file_t* f = getf(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == 0 && f->type == F_NODE) return -EBADF;
    UCHK(buf, n);
    if (f->type == F_NODE && !strcmp(f->node->name, "prof")) { prof_cmd(buf, n); return n; }
    if (f->type == F_NODE && f->node->parent && !strcmp(f->node->parent->name, "samara")) {
        if (!strcmp(f->node->name, "heap")) heap_cmd(buf, n);
        else if (!strcmp(f->node->name, "lat")) lat_cmd(buf, n);
        return n;
    }
    if (f->type == F_NODE && is_idmap(f->node)) return idmap_write(f->node, buf, n);
    return file_write(f, buf, n);
}

typedef struct { uint64_t base, len; } iovec_t;
int pt_mem_read(proc_t* p, uint64_t va, void* dst, uint64_t len);
int pt_mem_write(proc_t* p, uint64_t va, const void* src, uint64_t len);
int sys_close(int fd);

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
    if (f->type == F_PMEM) { f->off = whence == 1 ? (int64_t)f->off + off : off; return f->off; }
    if (f->type != F_NODE && f->type != F_DISK) return f->type == F_NULL || f->type == F_ZERO ? 0 : -ESPIPE;
    uint64_t end = f->type == F_DISK ? file_disk_size(f) : f->node->size;
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

static int64_t do_splice(int ifd, uint64_t* ioff, int ofd, uint64_t* ooff, uint64_t len) {
    file_t* fi = getf(ifd);
    file_t* fo = getf(ofd);
    if (!fi || !fo) return -EBADF;
    bool ip = fi->type == F_PIPE_R, op = fo->type == F_PIPE_W;
    if (!ip && !op) return -EINVAL;
    if ((ip && ioff) || (op && ooff)) return -ESPIPE;
    if (ioff) UCHK(ioff, 8);
    if (ooff) UCHK(ooff, 8);
    if (len > 65536) len = 65536;
    if (!len) return 0;
    char* kb = kmalloc(len);
    if (!kb) return -ENOMEM;
    uint64_t si = fi->off, so = fo->off;
    if (ioff) fi->off = *ioff;
    if (ooff) fo->off = *ooff;
    int64_t tot = file_read(fi, kb, len);
    if (tot > 0) {
        int64_t w = 0;
        while (w < tot) {
            int r = file_write(fo, kb + w, tot - w);
            if (r <= 0) { if (!w) tot = r; else tot = w; break; }
            w += r;
        }
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
    if (mnt_ro(parent)) return -EROFS;
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
    if (mnt_ro(parent)) return -EROFS;
    if ((err = mnt_newnode(parent)) < 0) return err;
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
    if (mnt_ro(parent) || (src->parent && mnt_ro(src->parent))) return -EROFS;
    if (fs_owner(src->parent) != fs_owner(parent) && (err = mnt_newnode(parent)) < 0) return err;
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
    if (mnt_ro(par)) return -EROFS;
    if (ow->mount_id && ow->mount_id < 8) return -EPERM;       /* fat */
    if (src->xl > 60000) return -EMLINK;
    if (fs_hlink(src, par, name) < 0) return -ENOMEM;
    ino_ev(par, 0x100, name, 0);
    return 0;
}

static int do_access(int dirfd, const char* path) {
    UCHK(path, 1);
    int err;
    if (lookup_peek(dirfd, path, &err, true)) return 0;
    // ns/* are magic links in linux, ours dangle
    if (strstr(path, "/ns/") && lookup_peek(dirfd, path, &err, false)) return 0;
    return err;
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
    if (nfd < 0 || nfd >= MAX_FDS || (uint64_t)nfd >= me()->sh->rl[7][0]) return -EBADF;
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
        case 1033: case 1034:                                   /* seals: memfd only, shm files get EINVAL like tmpfs does */
            if (f->type != F_NODE || strncmp(f->node->name, "memfd:", 6)) return -EINVAL;
            if (cmd == 1034) return f->node->seals;
            if (f->node->seals & 1) return -EPERM;
            f->node->seals |= (uint8_t)arg;
            return 0;
        case 1031: return PIPE_SZ;                               /* F_SETPIPE_SZ */
        case 1032: return PIPE_SZ;                               /* F_GETPIPE_SZ */
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
    if (f->type == F_DRM && (req & 0xffff) == 0x642d) {            /* prime handle -> fd */
        UCHK((void*)arg, 12);
        uint32_t* pa = (uint32_t*)arg;
        struct drm_fd* nd = drm_prime_export(f->drm, pa[0]);
        if (!nd) return -ENOENT;
        file_t* nf = file_new(F_DRM, 2);
        if (!nf) { drm_close(nd); return -ENOMEM; }
        nf->drm = nd; nf->disk = 1;
        int fd = install_fd(nf, 0, (pa[1] & 02000000) != 0);
        if (fd < 0) return fd;
        pa[2] = (uint32_t)fd;
        return 0;
    }
    if (f->type == F_DRM && (req & 0xffff) == 0x642e) {            /* prime fd -> handle */
        UCHK((void*)arg, 12);
        uint32_t* pa = (uint32_t*)arg;
        file_t* sf = getf((int)pa[2]);
        if (!sf || sf->type != F_DRM) return -EBADF;
        return drm_prime_import(f->drm, sf->drm, &pa[0]);
    }
    if (f->type == F_RTC) {
        switch (req) {
            case 0x80247009: UCHK((void*)arg, 36); clock_rtc_get((int*)arg); return 0;    /* RTC_RD_TIME */
            case 0x4024700a: UCHK((void*)arg, 36); return clock_rtc_set((const int*)arg) ? -EINVAL : 0;   /* RTC_SET_TIME */
            case 0x7003: f->cnt = clock_rtc_ups(); clock_rtc_uie(1); return 0;             /* UIE_ON */
            case 0x7004: clock_rtc_uie(0); return 0;
            case 0x7001: case 0x7002: case 0x7005: case 0x7006: return 0;                  /* AIE, PIE: nothing */
            case 0x8008700b: UCHK((void*)arg, 8); *(uint64_t*)arg = 1900; return 0;        /* RTC_EPOCH_READ */
        }
        return -ENOTTY;
    }
    if (f->type == F_DRM && (req & 0xffff) == 0x6442 && uok((void*)arg, 32) && (((uint32_t*)arg)[0] & 2)) {   /* virtgpu execbuffer wants a fence fd */
        int r = drm_ioctl(f->drm, req, (void*)arg);
        if (r < 0) return r;
        file_t* nf = file_new(F_EVENTFD, 2 | O_NONBLOCK);
        if (!nf) return -ENOMEM;
        int fd = install_fd(nf, 0, true);
        if (fd < 0) return fd;
        file_ref(nf);
        int i = 0;
        while (i < 16 && xf[i].fl) i++;
        if (i == 16) { drm_wait_fence(drm_exec_fence()); xf_fire(nf); file_close(nf); }
        else { xf[i].fl = nf; xf[i].f = drm_exec_fence(); drm_fence_wake(); }
        ((int32_t*)arg)[7] = fd;
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
        if (req == 0x125F) {                                  /* BLKRRPART, installer wrote a new table */
            if (f->disk >= DISK_PART_BASE) return -EINVAL;
            ata_part_clear(f->disk);
            part_scan(f->disk);
            fs_add_disk_nodes();
            return 0;
        }
        if (req == 0x1260) {                                  /* BLKGETSIZE (sectors) */
            UCHK((void*)arg, 8); *(uint64_t*)arg = ata_drive_sectors(f->disk); return 0;      // unsigned long
        }
        if (req == 0x125E || req == 0x1261) return 0;         /* BLKROGET, BLKFLSBUF */
        if (req == 0x127B) { UCHK((void*)arg, 4); *(int*)arg = 512; return 0; }   /* BLKPBSZGET */
        if (req == 0x1278 || req == 0x1279 || req == 0x127A) { UCHK((void*)arg, 4); *(int*)arg = 0; return 0; }   /* io min/opt, align */
        if (req == 0x80041272 || req == 0x80081272) {                              /* BLKGETSIZE64 */
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

static bool vm_over(proc_t* p, uint64_t add, bool data);

static spin_t brkl;

static uint64_t do_brk(uint64_t want) {
    proc_t* p = me();
    if (want < p->sh->brk_start || want >= USER_MMAP_BASE) return p->sh->brk;
    uint64_t fl = spin_lock(&brkl);          /* threads of one process, mostly no bkl here */
    uint64_t ret = p->sh->brk;
    uint64_t old_top = (ret + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_top = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (new_top > old_top) {
        if (vm_over(p, new_top - old_top, true)) goto out;
        int r = vmm_alloc_free(p->pd, old_top, new_top - old_top, true);
        if (r) {
            if (r < 0) { vmm_free_range(p->pd, old_top, new_top - old_top); vmm_flush(); }
            goto out;
        }
    } else if (new_top < old_top) {
        vmm_free_range(p->pd, new_top, old_top - new_top);
        vmm_flush();
    }
    p->sh->brk = ret = want;
out:
    spin_unlock(&brkl, fl);
    return ret;
}

/* /dev/shm files: frames live here, every MAP_SHARED of the node maps the
   same ones. the file itself stays empty, read()/write() don't see it.
   TODO: fork copies these pages instead of sharing */
#define SHM_MAX 128
static struct { fs_node_t* node; uint64_t* fr; uint32_t n; int nlz; bool dead; } shm[SHM_MAX];
static spin_t shml;

static bool is_shm(fs_node_t* n) {
    if (!n->parent && !strncmp(n->name, "memfd:", 6)) return true;    /* memfd_create: shared like /dev/shm */
    return n->parent && !strcmp(n->parent->name, "shm") && n->parent->parent &&
           !strcmp(n->parent->parent->name, "dev");
}

static void shm_free_slot(int i) {
    for (uint32_t k = 0; k < shm[i].n; k++) if (shm[i].fr[k]) pmm_unref(shm[i].fr[k]);
    kfree(shm[i].fr);
    shm[i].node = NULL;
    shm[i].n = 0;
    shm[i].dead = false;
}

/* frames are per page, 0 = not touched yet. lazy ptes (PTE_SHL) in page tables keep the slot
   alive after the node is gone, the last one out frees it */
static void shm_drop(fs_node_t* n) {
    uint64_t f = spin_lock(&shml);
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm[i].node != n) continue;
        if (shm[i].nlz > 0) { shm[i].node = NULL; shm[i].dead = true; continue; }
        shm_free_slot(i);
    }
    spin_unlock(&shml, f);
}

void shm_lz_put(uint64_t e) {
    int i = (e >> 40) & 0xFF;
    uint64_t f = spin_lock(&shml);
    if (--shm[i].nlz <= 0 && shm[i].dead) shm_free_slot(i);
    spin_unlock(&shml, f);
}

void shm_lz_dup(uint64_t e) {
    uint64_t f = spin_lock(&shml);
    shm[(e >> 40) & 0xFF].nlz++;
    spin_unlock(&shml, f);
}

// frame of a lazy page, spare goes in if nobody had one yet
uint64_t shm_lz_frame(uint64_t e, uint64_t spare) {
    int i = (e >> 40) & 0xFF;
    uint32_t k = (e >> 12) & 0xFFFFFFF;
    uint64_t f = spin_lock(&shml);
    if (!shm[i].fr[k]) shm[i].fr[k] = spare;
    uint64_t r = shm[i].fr[k];
    spin_unlock(&shml, f);
    return r;
}

// table for n, grown to pages entries. nothing allocated per page. returns slot or -1
static int shm_slot(fs_node_t* n, uint32_t pages) {
    uint64_t f = spin_lock(&shml);
    int s = -1;
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm[i].node == n) { s = i; break; }
        if (s < 0 && !shm[i].node && !shm[i].dead) s = i;
    }
    if (s >= 0 && !(shm[s].node == n && shm[s].n >= pages)) {
        uint64_t* fr = (uint64_t*)kmalloc(pages * 8);
        if (!fr) { spin_unlock(&shml, f); return -1; }
        memset(fr, 0, pages * 8);
        uint32_t had = shm[s].node == n ? shm[s].n : 0;
        if (had) { memcpy(fr, shm[s].fr, had * 8); kfree(shm[s].fr); }
        shm[s].node = n;
        shm[s].fr = fr;
        shm[s].n = pages;
    }
    spin_unlock(&shml, f);
    return s;
}

// real frame of page k, allocated on the first touch
static uint64_t shm_pg(int s, uint32_t k) {
    uint64_t fr = pmm_alloc();
    if (!fr) return 0;
    uint64_t f = spin_lock(&shml);
    if (shm[s].fr[k]) { pmm_unref(fr); fr = shm[s].fr[k]; }
    else shm[s].fr[k] = fr;
    spin_unlock(&shml, f);
    return fr;
}

/* read()/write() on shm and memfd go to the same frames mmap hands out */
int shm_rw(fs_node_t* n, uint64_t off, char* buf, uint32_t len, bool wr) {
    if (!is_shm(n)) return -1;
    if (!wr) {
        if (off >= n->size) return 0;
        if (len > n->size - off) len = n->size - off;
    }
    if (!len) return 0;
    int s = shm_slot(n, (off + len + PAGE_SIZE - 1) / PAGE_SIZE);
    if (s < 0) return -ENOMEM;
    for (uint32_t d = 0; d < len; ) {
        uint64_t o = off + d;
        uint32_t k = PAGE_SIZE - (o & (PAGE_SIZE - 1));
        if (k > len - d) k = len - d;
        uint64_t fr = shm[s].fr[o / PAGE_SIZE];
        if (!fr && !wr) memset(buf + d, 0, k);
        else {
            if (!fr) fr = shm_pg(s, o / PAGE_SIZE);
            if (!fr) return -ENOMEM;
            char* pg = (char*)P2V(fr) + (o & (PAGE_SIZE - 1));
            if (wr) memcpy(pg, buf + d, k); else memcpy(buf + d, pg, k);
        }
        d += k;
    }
    if (wr && off + len > n->size) n->size = off + len;
    return len;
}

/* SysV shm, the four calls end up in do_ipc. a segment is a
   dummy node in the shm[] table above, so every shmat maps the same frames.
   for MIT-SHM: glxgears sent every frame down the socket without it */
#define SV_MAX 64
static struct { int key; uint32_t size; fs_node_t* node; int nattch; bool rm; int cpid; uint16_t mode; uint8_t ns; } sv[SV_MAX];
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
                if (sv[i].node && sv[i].key == key && !sv[i].rm && sv[i].ns == p->ipcns) {
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
                if (shm_slot(n, pages) < 0) { kfree(n); sv[i].node = NULL; return -ENOMEM; }
                sv[i].key = key; sv[i].size = size; sv[i].nattch = 0; sv[i].rm = false;
                sv[i].cpid = p->tgid; sv[i].mode = (uint16_t)(flg & 0777); sv[i].ns = p->ipcns;
                return i + 1;
            }
            return -28;                                           /* ENOSPC */
        }
        case 21: {                                                    /* shmat(id, flags, -, addr) */
            int i = first - 1;
            if (i < 0 || i >= SV_MAX || !sv[i].node || sv[i].ns != p->ipcns) return -EINVAL;
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
            int ss = shm_slot(sv[i].node, len / PAGE_SIZE);
            if (ss < 0) return -ENOMEM;
            vmm_lazy_shm(p->pd, addr, len, !(second & 010000), true, ss, 0);   /* SHM_RDONLY */
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
    if (vm_over(p, len, !f && (prot & PROT_WRITE) && !(flags & MAP_SHARED))) return -ENOMEM;
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
        if ((prot & PROT_WRITE) && (f->node->seals & 8) && !f->node->parent) { vmm_free_range(p->pd, addr, len); return -EPERM; }
        int ss = shm_slot(f->node, first + np);
        if (ss < 0 || vmm_lazy_shm(p->pd, addr, len, (prot & PROT_WRITE) != 0, prot != 0, ss, first) < 0) { vmm_free_range(p->pd, addr, len); vmm_flush(); return -ENOMEM; }
        vmm_flush();
        return (int64_t)addr;
    }
    if (f && f->type == F_NODE && is_shm(f->node) && !(off & (PAGE_SIZE - 1))) {
        /* private map of a memfd: same frames, copied on the first write. wine maps its
           server session like this read only and expects to see the server's updates */
        uint64_t first = off / PAGE_SIZE, np = len / PAGE_SIZE;
        int ss = shm_slot(f->node, first + np);
        if (ss < 0) { vmm_free_range(p->pd, addr, len); return -ENOMEM; }
        for (uint64_t k = 0; k < np; k++) {
            uint64_t fr = shm_pg(ss, first + k);
            if (!fr) { vmm_free_range(p->pd, addr, len); vmm_flush(); return -ENOMEM; }
            vmm_map_cache(p->pd, addr + k * PAGE_SIZE, fr, (prot & PROT_WRITE) != 0, false);
        }
        if (!prot) vmm_set_user(p->pd, addr, len, false);
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
    if (f && f->type == F_NODE && f->node->type == FS_FILE && !(off & (PAGE_SIZE - 1)) && !is_shm(f->node)) {
        /* file pages come from the page cache, shared by everybody who maps this file */
        int md = ((flags & MAP_SHARED) ? PCM_SHARED : 0) | ((prot & PROT_WRITE) ? PCM_W : 0);
        if (pc_map(p->pd, addr, f->node, off, len, md) < 0) {
            vmm_free_range(p->pd, addr, len);
            vmm_flush();
            return -ENOMEM;
        }
        if (!prot) vmm_set_user(p->pd, addr, len, false);
        vmm_flush();
        return (int64_t)addr;
    }
    if (vmm_alloc_range(p->pd, addr, len, true) < 0) {
        vmm_free_range(p->pd, addr, len);
        vmm_flush();
        return -ENOMEM;
    }
    if (f && f->type == F_NODE && is_shm(f->node)) {
        /* private map of a memfd: its bytes live in the shm frames, not in node->data.
           the wayland keymap goes this way, foot died on the last page */
        char* tmp = kmalloc(PAGE_SIZE);
        for (uint64_t d = 0; tmp && d < len; d += PAGE_SIZE) {
            int got = shm_rw(f->node, off + d, tmp, PAGE_SIZE, false);
            if (got <= 0) break;
            vmm_copy_to(p->pd, addr + d, tmp, (uint64_t)got);
        }
        kfree(tmp);
    } else if (f && f->type == F_NODE && f->node->type == FS_FILE && off < f->node->size) {
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
    pc_sync_all();
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

/* files whose readiness we can look at without the bkl, the rest wants it (sockets, ttys, ...) */
static bool nb_safe(file_t* f) {
    switch (f->type) {
        case F_PIPE_R: case F_PIPE_W: case F_SPAIR: case F_EVENTFD: case F_TIMERFD: case F_NODE:
        case F_NULL: case F_ZERO: case F_RANDOM: case F_USOCK:
            return true;
        default: return false;
    }
}

/* last ref on a socket or tty drops with the lock, whoever we are */
static void fput(file_t* f) {
    if (!f) return;
    if (nb_safe(f)) { file_close(f); return; }
    int tk = bkl_enter();
    file_close(f);
    bkl_leave(tk);
}

/* tk/bk: bkl taken by us for this pass / some fd needed it. dropped before any sleep */
static int poll_once(pollfd_t* fds, uint64_t n, int* tk, int* bk) {
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        file_t* f = getf_ref(fds[i].fd);
        if (!f) { fds[i].revents = 0x20; ready++; continue; }        /* POLLNVAL */
        if (!nb_safe(f)) { *bk = 1; if (!*tk) *tk = bkl_enter(); }
        int16_t ev = fd_revents(f, fds[i].events);
        fput(f);
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
    spin_t lk;                  /* n, cap, it[] and the pointer in the file. leaf lock, no kmalloc under it */
    int n, cap;
    uint32_t gen;
    struct epi { int fd; file_t* f; uint32_t ev; uint32_t d0, d1; } *it;
} ep_t;

void ep_free(file_t* f) {
    kfree(f->ep->it);
    kfree(f->ep);
}

static int16_t ep_ready(file_t* f) {
    ep_t* e = f->ep;
    uint64_t fl = spin_lock(&e->lk);
    int r = 0;
    // only called with the bkl (an epoll in a poll set is not nb_safe), and closing needs it too
    for (int i = 0; i < e->n && !r; i++) {
        file_t* x = getf(e->it[i].fd);
        if (x && x == e->it[i].f && x->type != F_EPOLL && fd_revents(x, (int16_t)(e->it[i].ev & 0xFF))) r = 1;
    }
    spin_unlock(&e->lk, fl);
    return r;
}

static int do_epoll_create(int flags) {
    file_t* f = file_new(F_EPOLL, 2);
    if (!f) return -ENOMEM;
    f->ep = kmalloc(sizeof(ep_t));
    if (!f->ep) { kfree(f); return -ENOMEM; }
    memset(f->ep, 0, sizeof(ep_t));
    f->ep->it = kmalloc(32 * sizeof(struct epi));
    if (!f->ep->it) { kfree(f->ep); kfree(f); return -ENOMEM; }
    f->ep->cap = 32;
    return install_fd(f, 0, (flags & 02000000) != 0);
}

static int do_epoll_ctl(int epfd, int op, int fd, uint32_t* uev) {
    file_t* f = getf_ref(epfd);
    file_t* x = getf_ref(fd);
    int ret = 0;
    ep_t* e;
    struct epi *ne = NULL, *old = NULL;
    uint32_t ev[3] = {0};
    int ncap = 0;
    if (!f || !x) { ret = -EBADF; goto out; }
    if (f->type != F_EPOLL || x == f) { ret = -EINVAL; goto out; }
    if (op != 2) {
        if (!uok(uev, 12)) { ret = -EFAULT; goto out; }          /* packed: events, u64 data */
        memcpy(ev, uev, 12);
    }
    for (;;) {
        uint64_t fl = spin_lock(&f->ep->lk);
        e = f->ep;
        int i = 0;
        while (i < e->n && !(e->it[i].fd == fd && e->it[i].f == x)) i++;
        e->gen++;
        if (op == 2) {                                            /* DEL */
            if (i == e->n) ret = -ENOENT;
            else e->it[i] = e->it[--e->n];
        } else if (op == 1) {                                     /* ADD */
            if (i < e->n) ret = -EEXIST;
            else if (e->n == e->cap) {
                if (ne && ncap == e->cap * 2) {
                    memcpy(ne, e->it, (uint32_t)e->n * sizeof(*ne));
                    old = e->it; e->it = ne; e->cap = ncap; ne = NULL;
                } else {
                    int cap = e->cap;
                    spin_unlock(&e->lk, fl);
                    if (ne) kfree(ne);
                    ne = kmalloc((uint32_t)cap * 2 * sizeof(*ne));
                    if (!ne) { ret = -ENOMEM; goto out; }
                    ncap = cap * 2;
                    continue;
                }
            }
            if (!ret) {
                i = e->n++;
                e->it[i].fd = fd; e->it[i].f = x;
                e->it[i].ev = ev[0]; e->it[i].d0 = ev[1]; e->it[i].d1 = ev[2];
            }
        } else if (op == 3) {                                     /* MOD */
            if (i == e->n) ret = -ENOENT;
            else { e->it[i].ev = ev[0]; e->it[i].d0 = ev[1]; e->it[i].d1 = ev[2]; }
        } else ret = -EINVAL;
        spin_unlock(&e->lk, fl);
        break;
    }
out:
    if (old) kfree(old);
    if (ne) kfree(ne);
    if (f && f->type == F_EPOLL) wq_wake(&f->wq);
    fput(x);
    fput(f);
    return ret;
}

static int do_epoll_wait_f(file_t* f, uint32_t* out, int max, int timeout_ms);
static int do_epoll_wait(int epfd, uint32_t* out, int max, int timeout_ms) {
    file_t* f = getf_ref(epfd);
    if (!f) return -EBADF;
    int r = do_epoll_wait_f(f, out, max, timeout_ms);
    fput(f);      // exit_group closed the epfd under a sleeping thread and f->ep was gone (chromium)
    return r;
}

typedef struct { int fd; file_t* f; uint32_t ev, d0, d1; } epit_t;

static int do_epoll_wait_f(file_t* f, uint32_t* out, int max, int timeout_ms) {
    if (f->type != F_EPOLL || max <= 0) return -EINVAL;
    UCHK(out, (uint32_t)max * 12);
    uint32_t start = pit_uptime_ms();
    WQ_W(w);
    bool armed = false;
    int unk = 0, bk = 0, tk = 0, ret;
    uint32_t gen = 0;
    for (;;) {
        if (!me()) return -EINTR;
        if (bk) { tk = bkl_enter(); sock_pump(); }
        int got = 0, pos = 0;
        epit_t sn[32];
        uint32_t ob[32 * 3];
        for (;;) {
            // copy a chunk out under the lock, look at the files without it
            int k = 0, total;
            uint64_t fl = spin_lock(&f->ep->lk);
            ep_t* e = f->ep;
            total = e->n;
            gen = e->gen;
            for (; k < 32 && pos + k < e->n; k++) {
                sn[k].fd = e->it[pos + k].fd; sn[k].f = e->it[pos + k].f; sn[k].ev = e->it[pos + k].ev;
                sn[k].d0 = e->it[pos + k].d0; sn[k].d1 = e->it[pos + k].d1;
            }
            spin_unlock(&e->lk, fl);
            if (!k) break;
            int ng = 0, dead[32], nd = 0, one[32], no = 0;
            for (int i = 0; i < k && got + ng < max; i++) {
                file_t* x = getf_ref(sn[i].fd);
                if (!x || x != sn[i].f) { fput(x); dead[nd++] = i; continue; }   /* closed: gone, like linux */
                if (!nb_safe(x)) { bk = 1; if (!tk) tk = bkl_enter(); }
                int16_t r = fd_revents(x, (int16_t)((sn[i].ev & 0xFF) | 0x18));
                fput(x);
                r &= (int16_t)(sn[i].ev | 0x18);              /* ERR and HUP always */
                if (!r) continue;
                ob[ng * 3] = (uint32_t)(uint16_t)r; ob[ng * 3 + 1] = sn[i].d0; ob[ng * 3 + 2] = sn[i].d1;
                ng++;
                if (sn[i].ev & (1u << 30)) one[no++] = i;      /* EPOLLONESHOT: off until MOD */
            }
            if (nd || no) {
                fl = spin_lock(&f->ep->lk);
                e = f->ep;
                for (int j = 0; j < nd + no; j++) {
                    int i = j < nd ? dead[j] : one[j - nd];
                    int m = 0;
                    while (m < e->n && !(e->it[m].fd == sn[i].fd && e->it[m].f == sn[i].f)) m++;
                    if (m == e->n) continue;
                    if (j < nd) e->it[m] = e->it[--e->n];
                    else e->it[m].ev = 1u << 30;
                }
                spin_unlock(&e->lk, fl);
            }
            memcpy(out + got * 3, ob, (uint32_t)ng * 12);    // may fault, no lock but maybe the bkl
            got += ng;
            pos += k - nd;
            if (got >= max || pos >= total) break;
        }
        if (got || timeout_ms == 0) { ret = got; goto out; }
        uint32_t el = pit_uptime_ms() - start;
        if (timeout_ms > 0 && el >= (uint32_t)timeout_ms) { ret = 0; goto out; }
        if (proc_interrupted()) { ret = -EINTR; goto out; }
        if (armed && gen != f->ep->gen) { wq_waiter_free(w); w = NULL; armed = false; }
        if (!armed) {
            armed = true;
            unk = 0;
            w = wq_waiter();
            if (!w) unk = 1;
            else {
                if (!wq_add(&f->wq, w)) unk = 1;
                int pos2 = 0;
                for (;;) {
                    int k = 0;
                    uint64_t fl = spin_lock(&f->ep->lk);
                    ep_t* e = f->ep;
                    gen = e->gen;
                    for (; k < 32 && pos2 + k < e->n; k++) { sn[k].fd = e->it[pos2 + k].fd; sn[k].f = e->it[pos2 + k].f; }
                    spin_unlock(&e->lk, fl);
                    if (!k) break;
                    for (int i = 0; i < k; i++) {
                        file_t* x = getf_ref(sn[i].fd);
                        if (!x) continue;
                        if (x == sn[i].f) { if (!nb_safe(x) && !tk) tk = bkl_enter(); unk |= wait_arm(x, w); }
                        fput(x);
                    }
                    pos2 += k;
                }
            }
            bkl_leave(tk); tk = 0;
            continue;
        }
        uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms - el : 0;
        int pos3 = 0;
        for (;;) {
            int k = 0;
            uint64_t fl = spin_lock(&f->ep->lk);
            ep_t* e = f->ep;
            for (; k < 32 && pos3 + k < e->n; k++) { sn[k].fd = e->it[pos3 + k].fd; sn[k].f = e->it[pos3 + k].f; }
            spin_unlock(&e->lk, fl);
            if (!k) break;
            for (int i = 0; i < k; i++) {
                file_t* x = getf_ref(sn[i].fd);
                uint32_t t = x && x == sn[i].f ? timer_ms(x) : 0;
                fput(x);
                if (t && (!ms || t < ms)) ms = t;
            }
            pos3 += k;
        }
        if (unk) ms = 1;
        bkl_leave(tk); tk = 0;
        if (w) wq_sleep(w, ms);
        else task_sleep_ms(1);
    }
out:
    bkl_leave(tk);
    return ret;
}

static int do_poll(pollfd_t* fds, uint64_t n, int timeout_ms) {
    if (n > MAX_FDS * 2) return -EINVAL;
    // poll(NULL, 0, ms) is a plain sleep. runit does that, EFAULT here had runsvdir spinning on the bkl
    if (n) UCHK(fds, n * sizeof(pollfd_t));
    uint32_t start = pit_uptime_ms();
    WQ_W(w);
    bool armed = false;
    int unk = 0, bk = 0, tk = 0, r;
    for (;;) {
        if (!me()) return -EINTR;
        if (bk) { tk = bkl_enter(); sock_pump(); }
        r = poll_once(fds, n, &tk, &bk);
        if (r || timeout_ms == 0) goto out;
        uint32_t el = pit_uptime_ms() - start;
        if (timeout_ms > 0 && el >= (uint32_t)timeout_ms) { r = 0; goto out; }
        if (proc_interrupted()) { r = -EINTR; goto out; }
        if (!armed) {
            armed = true;
            w = wq_waiter();
            if (!w) unk = 1;
            else for (uint64_t i = 0; i < n; i++) {
                file_t* f = fds[i].fd < 0 ? NULL : getf_ref(fds[i].fd);
                if (f) { if (!nb_safe(f) && !tk) tk = bkl_enter(); unk |= wait_arm(f, w); fput(f); }
            }
            bkl_leave(tk); tk = 0;
            continue;
        }
        uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms - el : 0;
        for (uint64_t i = 0; i < n; i++) {
            file_t* f = fds[i].fd < 0 ? NULL : getf_ref(fds[i].fd);
            uint32_t t = f ? timer_ms(f) : 0;
            fput(f);
            if (t && (!ms || t < ms)) ms = t;
        }
        if (unk) ms = 1;
        bkl_leave(tk); tk = 0;
        if (w) wq_sleep(w, ms);
        else task_sleep_ms(1);
    }
out:
    bkl_leave(tk);
    return r;
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
    int unk = 0, bk = 0, tk = 0, ret;
    for (;;) {
        if (!me()) return -EINTR;
        if (bk) { tk = bkl_enter(); sock_pump(); }
        int ready = 0;
        uint32_t got_r[FDW] = {0}, got_w[FDW] = {0};
        for (int fd = 0; fd < n; fd++) {
            uint32_t bit = 1u << (fd & 31);
            bool wr_ = want_w[fd >> 5] & bit, rd_ = want_r[fd >> 5] & bit;
            if (!rd_ && !wr_) continue;
            file_t* f = getf_ref(fd);
            if (!f) { ret = -EBADF; goto out; }
            if (!nb_safe(f)) { bk = 1; if (!tk) tk = bkl_enter(); }
            if (rd_ && file_readable(f)) { got_r[fd >> 5] |= bit; ready++; }
            if (wr_ && file_writable(f)) { got_w[fd >> 5] |= bit; ready++; }
            fput(f);
        }
        if (ready || timeout_ms == 0 ||
            (timeout_ms > 0 && pit_uptime_ms() - start >= (uint32_t)timeout_ms)) {
            bkl_leave(tk); tk = 0;
            for (uint32_t i = 0; i < words && i < FDW; i++) {
                if (rd) rd[i] = got_r[i];
                if (wr) wr[i] = got_w[i];
                if (ex) ex[i] = 0;
            }
            return ready;
        }
        if (proc_interrupted()) { ret = -EINTR; goto out; }
        uint32_t el = pit_uptime_ms() - start;
        uint32_t ms = timeout_ms > 0 ? (uint32_t)timeout_ms - el : 0;
        if (!armed) {
            armed = true;
            w = wq_waiter();
            if (!w) unk = 1;
            for (int fd = 0; w && fd < n; fd++) {
                if (!((want_r[fd >> 5] | want_w[fd >> 5]) & (1u << (fd & 31)))) continue;
                file_t* f = getf_ref(fd);
                if (f) { if (!nb_safe(f) && !tk) tk = bkl_enter(); unk |= wait_arm(f, w); fput(f); }
            }
            bkl_leave(tk); tk = 0;
            continue;
        }
        for (int fd = 0; fd < n; fd++) {
            if (!((want_r[fd >> 5] | want_w[fd >> 5]) & (1u << (fd & 31)))) continue;
            file_t* f = getf_ref(fd);
            uint32_t t = f ? timer_ms(f) : 0;
            fput(f);
            if (t && (!ms || t < ms)) ms = t;
        }
        if (unk) ms = 1;
        bkl_leave(tk); tk = 0;
        if (w) wq_sleep(w, ms);
        else task_sleep_ms(1);
    }
out:
    bkl_leave(tk);
    return ret;
}

/* ---------------- misc ---------------- */

static int do_uname(char* u) {
    UCHK(u, 65 * 6);
    memset(u, 0, 65 * 6);
    strcpy(u + 0 * 65, "Linux");                 /* what the ABI looks like to userland */
    fs_node_t* hn = fs_resolve(fs_root(), "/etc/hostname");
    const char* host = "samara";
    char tmp[65], dom[65];
    if (ns_uname(me(), tmp, dom)) host = tmp;
    else if (hn && hn->data && hn->size) {
        int k = 0;
        while (k < 63 && k < (int)hn->size && hn->data[k] != '\n') { tmp[k] = hn->data[k]; k++; }
        tmp[k] = 0;
        if (k) host = tmp;
    }
    strcpy(u + 1 * 65, host);
    strcpy(u + 2 * 65, "5.0.0-samara");
    strcpy(u + 3 * 65, "#1 SamaraOS 0.5");
    strcpy(u + 4 * 65, "x86_64");
    strcpy(u + 5 * 65, ns_uname(me(), tmp, dom) ? dom : "(none)");
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
    if (me()->fsp) { proc_t* q = proc_by_pid(me()->ppid); if (q) q->sh->cwd = n; }
    return 0;
}

static int do_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG_MAX) return -EINVAL;
    if (pid > 0) {
        proc_t* p = proc_by_pid(ns_lpid(me(), pid));
        if (!p || p->state != P_ALIVE) return -ESRCH;
        return proc_send_signal(p, sig);
    }
    if (pid == 0) { proc_signal_group(me()->pgid, sig); return 0; }
    if (pid < -1) { proc_signal_group(ns_lpid(me(), -pid), sig); return 0; }
    /* -1: everyone but ourselves */
    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (p && p != me() && p->state == P_ALIVE && ns_gpid(me(), p->pid)) proc_send_signal(p, sig);
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
    return mnt_statfs(at, b);
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

static int do_rlimit(proc_t* q, int res, const uint64_t* nw, uint64_t* old) {
    if (res < 0 || res > 15) return -EINVAL;
    if (nw && (nw[0] > nw[1])) return -EINVAL;
    if (old) { old[0] = q->sh->rl[res][0]; old[1] = q->sh->rl[res][1]; }
    if (nw) {
        q->sh->rl[res][0] = nw[0]; q->sh->rl[res][1] = nw[1];
    }
    return 0;
}

// total vm after growing by add bytes would go over RLIMIT_AS (or DATA for brk/anon)
static bool vm_over(proc_t* p, uint64_t add, bool data) {
    uint64_t lim = p->sh->rl[9][0];
    if (data && p->sh->rl[2][0] < lim) lim = p->sh->rl[2][0];
    if (lim == ~0ull) return false;
    return (vmm_count_virt(p->pd) + 2048) * PAGE_SIZE + add > lim;   // + stack window
}

static int do_wait(int pid, int* status, int options) {
    if (status) UCHK(status, 4);
    int st = 0;
    proc_t* me_ = me();
    if (pid > 0) pid = ns_lpid(me_, pid);
    else if (pid < -1) pid = -ns_lpid(me_, -pid);
    int r = proc_wait(pid, &st, options);
    if (r > 0 && status) *status = st;
    return r;
}

static int do_waitid(int type, int id, int* si, int options) {
    int st = 0, r;
    if (si) UCHK(si, 128);
    if (!(options & 4)) return -EINVAL;
    if (si) memset(si, 0, 128);
    if ((options & 0x01000000) && type == 1) {               /* WNOWAIT: just look */
        proc_t* q = proc_by_pid(ns_lpid(me(), id));
        if (!q || q->ppid != me()->tgid) return -ECHILD;
        if (q->state != P_ZOMBIE) {
            if (!(options & 1)) { while (q->state != P_ZOMBIE && !proc_interrupted()) task_sleep_ms(1); if (q->state != P_ZOMBIE) return -EINTR; }
            else return 0;
        }
        st = q->exit_status;
        r = id;
    } else {
        r = proc_wait(type == 0 ? -1 : type == 1 ? ns_lpid(me(), id) : -ns_lpid(me(), id), &st, options & 1);
        if (r <= 0) return r < 0 ? r : 0;
    }
    if (si) {
        si[0] = 17;
        si[2] = (st & 0x7F) == 0 ? 1 : (st & 0x80) ? 3 : 2;
        si[4] = r;
        si[6] = (st & 0x7F) == 0 ? (st >> 8) & 0xFF : st & 0x7F;
    }
    return 0;
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
    if ((type & 0xF) != 1 && (type & 0xF) != 5 && (type & 0xF) != 2) return -93;   /* stream, seqpacket and dgram all act as stream, boundaries are lost */
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
    file_t tf = *f;          /* nonblock tricks go on a copy, other threads use the real one without the bkl */
    switch (call) {
        case 9: case 11: {                                            /* send / sendto */
            UCHK((void*)b, c);
            if (d & 0x40) tf.flags |= O_NONBLOCK;
            return file_write(&tf, (const char*)b, c);
        }
        case 10: case 12: {                                           /* recv / recvfrom */
            UCHK((void*)b, c);
            if (d & 0x40) tf.flags |= O_NONBLOCK;
            return file_read(&tf, (char*)b, c);
        }
        case 16: case 17: {                                           /* sendmsg / recvmsg: xcb lives on these */
            UCHK((void*)b, sizeof(msghdr_t));
            msghdr_t* m = (msghdr_t*)b;
            iovec_t* iov = (iovec_t*)m->iov;
            UCHK(iov, m->iovlen * sizeof(iovec_t));
            if (c & 0x40) tf.flags |= O_NONBLOCK;
            int total = 0, r = 0;
            if (call == 16 && f->pipe2) f->pipe2->spid = me()->tgid;
            if (call == 16 && m->ctl && m->ctllen >= 16) {           /* SCM_RIGHTS out: refs ride along with the bytes */
                UCHK((void*)m->ctl, m->ctllen);
                cmsghdr_t* cm = (cmsghdr_t*)m->ctl;
                int* cfd = (int*)(cm + 1);
                if (cm->level == 1 && cm->type == 1)
                    for (uint64_t k = 0; k < (cm->len - 16) / 4; k++) {
                        file_t* x = getf_ref(cfd[k]);        /* the ref goes into the queue */
                        if (!x) continue;
                        pipe_t* q = f->pipe2;
                        uint64_t fl = spin_lock(&q->lk);
                        if (q->nfds < 32) { q->fpos[q->nfds] = q->wtot; q->fds[q->nfds++] = x; x = NULL; }
                        spin_unlock(&q->lk, fl);
                        if (x) file_close(x);
                    }
            }
            for (uint64_t i = 0; i < m->iovlen; i++) {
                if (!iov[i].len) continue;
                UCHK((void*)iov[i].base, iov[i].len);
                r = call == 16 ? file_write(&tf, (const char*)iov[i].base, iov[i].len)
                               : file_read(&tf, (char*)iov[i].base, iov[i].len);
                if (r < 0) break;
                total += r;
                if ((uint64_t)r < iov[i].len) break;
                if (call == 17) tf.flags |= O_NONBLOCK;            /* rest of the iovs: only what's there */
            }
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
                    for (;;) {
                        uint64_t fl = spin_lock(&q->lk);
                        if (!(q->nfds && q->fpos[0] < q->rtot && 16 + (k + 1) * 4 <= room)) { spin_unlock(&q->lk, fl); break; }
                        file_t* x = q->fds[0];
                        for (int j = 1; j < q->nfds; j++) { q->fds[j - 1] = q->fds[j]; q->fpos[j - 1] = q->fpos[j]; }
                        q->nfds--;
                        spin_unlock(&q->lk, fl);
                        int nfd = install_fd(x, 0, (c & 0x40000000) != 0);   /* MSG_CMSG_CLOEXEC */
                        if (nfd < 0) { file_close(x); continue; }
                        cfd[k++] = nfd;
                    }
                    cm->len = 16 + k * 4; cm->level = 1; cm->type = 1;
                    m->ctllen = (16 + k * 4 + 7) & ~7ul;
                }
                if (q->passcred && total > 0 && m->ctl && room >= m->ctllen + 32) {
                    UCHK((void*)m->ctl, room);   /* SCM_CREDENTIALS: pid, uid 0, gid 0 */
                    cmsghdr_t* cm = (cmsghdr_t*)((char*)m->ctl + m->ctllen);
                    uint32_t* u = (uint32_t*)(cm + 1);
                    cm->len = 28; cm->level = 1; cm->type = 2;
                    u[0] = q->spid ? q->spid : 1; u[1] = 0; u[2] = 0;
                    m->ctllen += 32;
                }
            }
            return total;
        }
        case 13: return spair_shutdown(f, (int)b);
        case 14:
            if (b == 1 && c == 16 && d && f->pipe) { UCHK((void*)d, 4); f->pipe->passcred = *(int*)d; }   /* SO_PASSCRED */
            return 0;                                                 /* setsockopt */
        case 15: {                                                    /* getsockopt(level b, opt c, val d, len e) */
            if (!d || !e) return -EFAULT;
            UCHK((void*)e, 4);
            uint32_t cap = *(uint32_t*)e;
            UCHK((void*)d, cap);
            if (b == 1 && c == 16 && cap >= 4) { *(int*)d = f->pipe ? f->pipe->passcred : 0; *(uint32_t*)e = 4; return 0; }
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
        case F_RTC: { extern wq_t rtc_wq; v[0] = &rtc_wq; return 1; }
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
            f->pipe = x->pipe;
            f->pipe2 = x->pipe2;
            f->shut = 0;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);        // nobkl pollers look at type first
            f->type = F_SPAIR;
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
    if (m) { *(uint16_t*)(m + 6) = 0; *(uint32_t*)(m + 12) = *(const uint32_t*)(h + 12); *(int32_t*)(m + 16) = err; memcpy(m + 20, h, 16); }
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
                if (m) { *(uint16_t*)(m + 6) = 0; *(uint32_t*)(m + 12) = *(const uint32_t*)(h + 12); }
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
                if ((b & 0xF) != 1 && (b & 0xF) != 5) return -93;    /* stream only (seqpacket too, no boundaries) */
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

static int do_eventfd(uint64_t nr, uint64_t a, uint64_t b) {
    int fl = nr == 290 ? (int)b : 0;
    if (fl & ~(1 | 04000 | 02000000)) return -EINVAL;           // mojo probes with junk flags
    file_t* f = file_new(F_EVENTFD, 2 | ((fl & 04000) ? O_NONBLOCK : 0) | ((fl & 1) ? 0x10000000 : 0));
    if (!f) return -ENOMEM;
    f->cnt = a;
    return install_fd(f, 0, (fl & 02000000) != 0);
}

static int do_timerfd_create(uint64_t a, uint64_t b) {
    file_t* f = file_new(F_TIMERFD, 2 | (((int)b & 04000) ? O_NONBLOCK : 0));
    if (!f) return -ENOMEM;
    f->disk = (int)a;                                        /* clock id */
    return install_fd(f, 0, (b & 02000000) != 0);
}

static int do_timerfd(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    file_t* f = getf_ref((int)a);
    int ret = 0;
    if (!f) return -EBADF;
    if (f->type != F_TIMERFD) { ret = -EINVAL; goto out; }
    bool set = nr == 286;
    uint32_t now = pit_uptime_ms();
    int64_t* oldp = (int64_t*)(set ? d : b);
    if (oldp) {                                              /* old / current: interval, value left */
        if (!uok(oldp, 32)) { ret = -EFAULT; goto out; }
        uint32_t left = f->t_next && (int32_t)(f->t_next - now) > 0 ? f->t_next - now : (f->t_next ? 1 : 0);
        oldp[0] = f->t_int / 1000; oldp[1] = (f->t_int % 1000) * 1000000u;
        oldp[2] = left / 1000; oldp[3] = (left % 1000) * 1000000u;
    }
    if (!set) goto out;
    if (!uok((void*)c, 32)) { ret = -EFAULT; goto out; }
    int64_t* nv = (int64_t*)c;
    int64_t is = nv[0], ins = nv[1], vs = nv[2], vns = nv[3];
    uint32_t ims = is * 1000 + ins / 1000000, vms = vs * 1000 + vns / 1000000;
    f->t_int = ims;
    if (!vs && !vns) { f->t_next = 0; goto out; }            /* disarm */
    if (b & 1) {                                             /* TFD_TIMER_ABSTIME */
        uint32_t target;
        if (f->disk == 0) {                                  /* REALTIME: from the wall clock */
            uint32_t ws, wns; clock_now(&ws, &wns);
            uint64_t wnow = (uint64_t)ws * 1000 + wns / 1000000, want = (uint64_t)vs * 1000 + vns / 1000000;
            target = want > wnow ? now + (uint32_t)(want - wnow) : now;
        } else target = vms;                                 /* MONOTONIC: uptime */
        f->t_next = target ? target : 1;
    } else f->t_next = now + (vms ? vms : 1);
out:
    file_close(f);
    return ret;
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
            int64_t gret = ret;
            if (ret > 0 && p->pidns) ret = ns_gpid(p, (int)ret);
            if (ret > 0 && p->tracer) {
                int ev = r->rax == 57 ? 1 : r->rax == 58 ? 2 : (a & 0x10000) ? 3 : 1;
                int fl = ev == 1 ? 2 : ev == 2 ? 4 : 8;
                if (p->pt_opts & fl) { r->rax = (uint64_t)gret; pt_event(p, r, ev, (uint64_t)gret); }
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
            if (fl->type != F_NODE && fl->type != F_PMEM && fl->type != F_DISK) return -ESPIPE;
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
            if (fl->type != F_NODE && fl->type != F_DISK) return -ESPIPE;
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
        case 247: return do_waitid((int)a, (int)b, (int*)c, (int)d);
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
            if (mnt_ro(par)) return -EROFS;
            if ((err = mnt_newnode(par)) < 0) return err;
            fs_node_t* sn = fs_symlink(par, name, tg);
            if (sn) ino_node(sn, 0x100);
            return sn ? 0 : -ENOMEM;
        }
        case 86:  return do_link(AT_FDCWD, (const char*)a, AT_FDCWD, (const char*)b, 0);
        case 265: return do_link((int)a, (const char*)b, (int)c, (const char*)d, (int)e);
        case 133: case 259: {                                         /* mknod: fifos only */
            bool at = r->rax == 259;
            const char* pt = (const char*)(at ? b : a);
            int md = (int)(at ? c : b);
            UCHK(pt, 1);
            if ((md & 0170000) != 0010000) return -EPERM;
            char name[FS_NAME_MAX];
            fs_node_t* par = lookup_parent(at ? (int)a : AT_FDCWD, pt, name, &err);
            if (!par) return err;
            if (fs_child(par, name)) return -EEXIST;
            if (mnt_ro(par)) return -EROFS;
            if ((err = mnt_newnode(par)) < 0) return err;
            fs_node_t* nn = fs_create(par, name, FS_FILE);
            if (!nn) return -ENOMEM;
            nn->dev = FS_DEV_FIFO;
            nn->mode = (uint16_t)(md & ~me()->sh->umask & 0777);
            ino_node(nn, 0x100);
            return 0;
        }
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
        case 90: case 268: case 452:
            UCHK((void*)(r->rax == 90 ? a : b), 1);
            n = r->rax == 90 ? lookup_peek(AT_FDCWD, (const char*)a, &err, true) : lookup_peek((int)a, (const char*)b, &err, true);
            if (!n) return err;
            n->mode = (uint16_t)((r->rax == 90 ? b : c) & 07777);
            ino_node(n, 4);
            return 0;
        case 91: {
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type == F_NODE) { fl->node->mode = (uint16_t)(b & 07777); ino_node(fl->node, 4); }
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
            {   // sqlite in chromium fsyncs every few ms and each one flushes the whole cache under the bkl, once a sec is enough
                static uint32_t last;
                uint32_t now = pit_uptime_ms();
                if (r->rax != 306 && last && now - last < 1000) return 0;
                last = now ? now : 1;
            }
            pc_sync_all();
            ext2_sync_all();
            return fatfs_sync_all();
        }
        case 162:                                                    /* sync */
            ext2_sync_all();
            return fatfs_sync_all();
        case 161: {                                                  /* chroot */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            if (n->type != FS_DIR) return -ENOTDIR;
            me()->sh->root = n;
            if (me()->fsp) { proc_t* q = proc_by_pid(me()->ppid); if (q) q->sh->root = n; }
            return 0;
        }
        case 155: UCHK((void*)a, 1); UCHK((void*)b, 1); return ns_pivot((const char*)a, (const char*)b);       /* pivot_root */
        case 272: return ns_unshare(a);
        case 308: return do_setns((int)a, (int)b);
        case 317: return sc_seccomp(a, b, c);
        case 165: {                                                  /* mount */
            UCHK((void*)b, 1);
            const char* t = NULL;
            const char* dt = NULL;
            fs_node_t* src = NULL;
            if (c) { UCHK((void*)c, 1); t = (const char*)c; }
            if (e) { UCHK((void*)e, 1); dt = (const char*)e; }
            if (a) { UCHK((void*)a, 1); src = fd_node((const char*)a, false); if (!src) src = lookup(AT_FDCWD, (const char*)a, &err); }
            fs_node_t* dst = fd_node((const char*)b, true);
            if (!dst) dst = p->mntns ? fs_mp(p->sh->cwd, (const char*)b) : lookup(AT_FDCWD, (const char*)b, &err);
            if (!dst) return p->mntns ? -ENOENT : err;
            return mnt_mount(src, dst, t, d, dt);
        }
        case 167: case 168: {                                        /* swapon, swapoff */
            UCHK((void*)a, 1);
            fs_node_t* src = lookup(AT_FDCWD, (const char*)a, &err);
            if (!src) return err;
            if (!FS_DEV_IS_DISK(src->dev)) return -EINVAL;
            if (r->rax == 168) return swap_off(src->dev - FS_DEV_DISK);
            return swap_on(src->dev - FS_DEV_DISK, (b & 0x8000) ? (int)(b & 0x7FFF) : -2);
        }
        case 166: {                                                  /* umount2 */
            UCHK((void*)a, 1);
            if (p->mntns) {                                          /* private ns: drop the alias, whatever it was */
                n = fs_mp(p->sh->cwd, (const char*)a);
                if (n) fs_unbind(n, p->mntns);
                return 0;
            }
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            for (int i = 0; i < proc_count(); i++) {
                proc_t* q = proc_at(i);
                for (fs_node_t* w = q ? q->sh->cwd : NULL; w; w = w->parent)
                    if (w == n && !FS_DEV_IS_DISK(n->dev)) return -EBUSY;
            }
            return mnt_umount(n);
        }
        case 140: {                                                  /* getpriority */
            proc_t* t = b ? proc_by_pid((int)b) : p;
            return t ? 20 - t->nice : -ESRCH;
        }
        case 141: {                                                  /* setpriority(which, who, prio) */
            proc_t* t = b ? proc_by_pid((int)b) : p;
            if (!t) return -ESRCH;
            int nv = (int)c;
            t->nice = nv < -20 ? -20 : nv > 19 ? 19 : nv;
            if (t->task >= 0) task_set_sched(t->task, t->policy, t->rtprio, t->nice);
            return 0;
        }
        case 142: case 144: {                                        /* sched_setparam / sched_setscheduler(pid, policy, param) */
            proc_t* t = a ? proc_by_pid((int)a) : p;
            if (!t) return -ESRCH;
            int pol = r->rax == 144 ? (int)b : t->policy, pr;
            UCHK((void*)(r->rax == 144 ? c : b), 4);
            pr = *(int*)(r->rax == 144 ? c : b);
            if (pol & 0x40000000) pol &= ~0x40000000;                /* RESET_ON_FORK */
            if (pol < 0 || pol > 5 || pol == 4) return -EINVAL;
            if ((pol == 1 || pol == 2) ? (pr < 1 || pr > 99) : pr != 0) return -EINVAL;
            t->policy = pol; t->rtprio = pr;
            if (t->task >= 0) task_set_sched(t->task, pol, pr, t->nice);
            return 0;
        }
        case 143: {                                                  /* sched_getparam */
            proc_t* t = a ? proc_by_pid((int)a) : p;
            if (!t) return -ESRCH;
            UCHK((void*)b, 4);
            *(int*)b = t->rtprio;
            return 0;
        }
        case 145: { proc_t* t = a ? proc_by_pid((int)a) : p; return t ? t->policy : -ESRCH; }
        case 146: return (a == 1 || a == 2) ? 99 : 0;                /* sched_get_priority_max */
        case 147: return (a == 1 || a == 2) ? 1 : 0;
        case 148:                                                    /* sched_rr_get_interval: 10 ms */
            UCHK((void*)b, 16);
            ((uint64_t*)b)[0] = 0; ((uint64_t*)b)[1] = 10000000;
            return 0;
        case 92: case 93: case 94: case 260:                         /* chown & co */
        case 105: case 106: case 113: case 114: case 117: case 119: case 116:
        case 26: pc_sync_all(); return 0;                            /* msync */
        case 28:                                                     /* madvise */
            if (c == 4 && !(a & (PAGE_SIZE - 1)) && a >= USER_BASE && a < USER_TOP) { vmm_discard(me()->pd, a, b); vmm_flush(); }
            return 0;
        case 157: return sc_prctl(a, b, c);
        case 442: return 0;                                          /* mount_setattr: ro is not enforced anyway */
        case 203: case 149: case 150: case 151: case 152:
        case 221: case 187: case 251: case 252:
            return 0;
        case 170: case 171: UCHK((void*)a, 1); return ns_sethost(p, (const char*)a, (int)b, r->rax == 171);
        case 169:                                                    /* reboot */
            if (a != 0xfee1dead) return -EINVAL;
            switch ((uint32_t)c) {
                case 0x01234567: case 0xA1B2C3D4: acpi_reboot(); break;
                case 0x4321FEDC: case 0xCDEF0123: acpi_poweroff();    /* halt = off too */
            }
            return 0;                                                /* cad on/off */
        case 8:   return do_lseek((int)a, (int64_t)b, (int)c);
        case 39:  return ns_gpid(p, p->tgid);
        case 186: return ns_gpid(p, p->pid);                         /* gettid */
        case 110: { proc_t* l = p->is_thread ? proc_by_pid(p->tgid) : p; int pp = l ? l->ppid : p->ppid; return p->pidns ? ns_gpid(p, pp) : pp ? pp : 1; }
        case 102: case 107: return ns_uid(p, 0);                     /* uid, euid */
        case 104: case 108: return ns_uid(p, 1);
        case 118: case 120:                                          /* getresuid / getresgid */
            UCHK((void*)a, 4); UCHK((void*)b, 4); UCHK((void*)c, 4);
            *(int*)a = *(int*)b = *(int*)c = ns_uid(p, r->rax == 120);
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
        case 200: case 234: case 297: {                              /* tkill / tgkill / rt_tgsigqueueinfo (info dropped) */
            int pid = r->rax == 200 ? (int)a : (int)b, sig = r->rax == 200 ? (int)b : (int)c;
            proc_t* t = proc_by_pid(ns_lpid(p, pid));
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
            proc_t* t = a ? proc_by_pid(ns_lpid(p, (int)a)) : p;
            if (!t) return -ESRCH;
            t->pgid = b ? ns_lpid(p, (int)b) : t->tgid;
            return 0;
        }
        case 111: return ns_gpid(p, p->pgid);
        case 121: { proc_t* t = a ? proc_by_pid(ns_lpid(p, (int)a)) : p; return t ? ns_gpid(p, t->pgid) : -ESRCH; }
        case 124: { proc_t* t = a ? proc_by_pid(ns_lpid(p, (int)a)) : p; return t ? ns_gpid(p, t->sid) : -ESRCH; }
        case 112: p->sid = p->pgid = p->tgid; p->ctty = -1; return ns_gpid(p, p->tgid);   /* setsid: no terminal */
        case 95:  { int old = p->sh->umask; p->sh->umask = (int)(a & 0777); return old; }
        case 13:                                                     /* rt_sigaction */
            if (b) UCHK((void*)b, 32);
            if (c) UCHK((void*)c, 32);
            return proc_sigaction((int)a, (const uint64_t*)b, (uint64_t*)c);
        case 37: {                                                   /* alarm */
            uint32_t left = p->alarm_at ? (p->alarm_at - pit_uptime_ms() + 999) / 1000 : 0;
            p->alarm_at = a ? pit_uptime_ms() + a * 1000 : 0;
            if (a) proc_timer_hint(p->alarm_at);
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
            if (!stk && !(vmm_pte(me()->pd, a & ~(PAGE_SIZE - 1)) & (PTE_P | PTE_SWAP))) return -EFAULT;
            return -ENOMEM;
        }
        case 76: {                                                   /* truncate */
            UCHK((void*)a, 1);
            n = lookup(AT_FDCWD, (const char*)a, &err);
            if (!n) return err;
            int tr = node_truncate(n, b);
            if (!tr) ino_node(n, 2);
            return tr;
        }
        case 77: {                                                   /* ftruncate */
            file_t* fl = getf((int)a);
            if (!fl) return -EBADF;
            if (fl->type != F_NODE) return -EINVAL;
            if (!strncmp(fl->node->name, "memfd:", 6) && !fl->node->parent) {
                uint8_t sl = fl->node->seals;
                if (((sl & 2) && b < fl->node->size) || ((sl & 4) && b > fl->node->size)) return -EPERM;
            }
            if (is_shm(fl->node) && b > (1u << 20) && !fl->node->data) {   /* frames come at mmap, no point in a shadow copy (foot asks for 512M) */
                fl->node->size = b;
                return 0;
            }
            int tr = node_truncate(fl->node, b);
            if (!tr) ino_node(fl->node, 2);
            return tr;
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
        case SYS_SAMARA:
            if (a == SM_OP_SCANOUT) {                                /* samara-wl, fullscreen gl straight to the screen */
                if ((int)b < 0) { drm_direct_off(); return 0; }
                file_t* sf = getf((int)b);
                if (!sf || sf->type != F_DRM) return -EBADF;
                return drm_direct(sf->drm, c >> 16, c & 0xffff);
            }
            return uwin_syscall(a, b, c, d);           /* desktop windows */
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
        case 97:  UCHK((void*)b, 16); return do_rlimit(p, (int)a, NULL, (uint64_t*)b);
        case 160: UCHK((void*)b, 16); return do_rlimit(p, (int)a, (uint64_t*)b, NULL);
        case 302: {                                                  /* prlimit64 */
            proc_t* q = a ? proc_by_pid((int)a) : p;
            if (!q || q->state != P_ALIVE) return -ESRCH;
            if (c) UCHK((void*)c, 16);
            if (d) UCHK((void*)d, 16);
            return do_rlimit(q, (int)b, (uint64_t*)c, (uint64_t*)d);
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
                for (int k = 0; k < n; k++) {
                    uint64_t e = ~0ull, pm = ~0ull, in = 0;
                    if (p->capx) { e = p->cap[0]; pm = p->cap[1]; in = p->cap[2]; }
                    int sh = k * 32;
                    ((uint32_t*)b)[k * 3] = (uint32_t)(e >> sh); ((uint32_t*)b)[k * 3 + 1] = (uint32_t)(pm >> sh); ((uint32_t*)b)[k * 3 + 2] = (uint32_t)(in >> sh);
                }
            }
            return 0;
        }
        case 126: {                                                  /* capset: remembered, nothing checks it */
            UCHK((void*)a, 8);
            uint32_t ver = *(uint32_t*)a;
            if (ver != 0x19980330 && ver != 0x20071026 && ver != 0x20080522) { *(uint32_t*)a = 0x20080522; return -EINVAL; }
            int n = ver == 0x19980330 ? 1 : 2;
            UCHK((void*)b, (uint32_t)n * 12);
            for (int j = 0; j < 3; j++) p->cap[j] = 0;
            for (int k = 0; k < n; k++)
                for (int j = 0; j < 3; j++) p->cap[j] |= (uint64_t)((uint32_t*)b)[k * 3 + j] << (k * 32);
            p->capx = true;
            return 0;
        }
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
        case 273: return 0;                                          /* set_robust_list, glibc does it for every thread */
        case 324: case 334: case 435: case 428: case 429: case 430: case 431: case 432: case 433:
            return -ENOSYS;                                          /* membarrier, rseq, clone3, new mount api: quiet */
        case 434: {                                                  /* pidfd_open */
            proc_t* q = proc_by_pid((int)a);
            if (!q || q->state != P_ALIVE || b) return q ? -EINVAL : -ESRCH;
            file_t* f = file_new(F_PIDFD, 0);
            if (!f) return -ENOMEM;
            f->cnt = q->pid;
            return install_fd(f, 0, true);
        }
        case 424: {                                                  /* pidfd_send_signal */
            file_t* f = getf((int)a);
            if (!f) return -EBADF;
            if (f->type != F_PIDFD) return -EINVAL;
            proc_t* q = proc_by_pid((int)f->cnt);
            if (!q || q->state != P_ALIVE) return -ESRCH;
            if (b >= NSIG_MAX) return -EINVAL;
            return proc_send_signal(q, (int)b);
        }
        case 438: {                                                  /* pidfd_getfd */
            file_t* f = getf((int)a);
            if (!f) return -EBADF;
            if (f->type != F_PIDFD) return -EINVAL;
            proc_t* q = proc_by_pid((int)f->cnt);
            if (!q || q->state != P_ALIVE) return -ESRCH;
            if (b >= MAX_FDS || !q->sh->fds[b]) return -EBADF;
            file_ref(q->sh->fds[b]);
            return install_fd(q->sh->fds[b], 0, true);
        }
        case 27: {                                                   /* mincore: all resident, whatever */
            if (a & (PAGE_SIZE - 1)) return -EINVAL;
            uint64_t np = (b + PAGE_SIZE - 1) / PAGE_SIZE;
            UCHK((void*)c, np);
            for (uint64_t i = 0; i < np; i++)       // mesa probes pointers with this, unmapped must be ENOMEM
                if (!(vmm_pte(me()->pd, a + i * PAGE_SIZE) & (PTE_P | PTE_LAZY | PTE_SWAP))) return -ENOMEM;
            memset((void*)c, 1, np);
            return 0;
        }
        case 436: {                                                  /* close_range */
            if (a > b || c & ~6ull) return -EINVAL;
            if (b >= MAX_FDS) b = MAX_FDS - 1;
            for (uint64_t i = a; i <= b; i++) {
                if (!getf((int)i)) continue;
                if (c & 4) p->sh->cloexec[i] = 1;                    /* CLOSE_RANGE_CLOEXEC */
                else sys_close((int)i);
            }
            return 0;
        }
        case 441: return do_epoll_wait((int)a, (uint32_t*)b, (int)c, timeout_ms(d, 1000000));
        case 310: case 311: {                                        /* process_vm_readv / writev */
            proc_t* q = proc_by_pid((int)a);
            if (!q || q->state != P_ALIVE) return -ESRCH;
            if (e > 1024 || c > 1024) return -EINVAL;
            UCHK((void*)b, c * 16);
            UCHK((void*)d, e * 16);
            iovec_t* li = (iovec_t*)b;
            iovec_t* ri = (iovec_t*)d;
            uint64_t lo = 0, ro = 0, tot = 0;
            uint32_t lk = 0, rk = 0;
            static uint8_t pvb[4096];
            while (lk < c && rk < e) {
                uint64_t n2 = li[lk].len - lo;
                if (ri[rk].len - ro < n2) n2 = ri[rk].len - ro;
                if (n2 > sizeof(pvb)) n2 = sizeof(pvb);
                if (n2) {
                    UCHK((void*)(li[lk].base + lo), n2);
                    if (r->rax == 310) {
                        if (pt_mem_read(q, ri[rk].base + ro, pvb, n2)) return tot ? (int64_t)tot : -EFAULT;
                        memcpy((void*)(li[lk].base + lo), pvb, n2);
                    } else {
                        memcpy(pvb, (void*)(li[lk].base + lo), n2);
                        if (pt_mem_write(q, ri[rk].base + ro, pvb, n2)) return tot ? (int64_t)tot : -EFAULT;
                    }
                    tot += n2; lo += n2; ro += n2;
                }
                if (lo >= li[lk].len) { lk++; lo = 0; }
                if (ro >= ri[rk].len) { rk++; ro = 0; }
            }
            return (int64_t)tot;
        }
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
        case 284: case 290: return do_eventfd(r->rax, a, b);        /* eventfd(2) */
        case 283: return do_timerfd_create(a, b);
        case 286: case 287: return do_timerfd(r->rax, a, b, c, d);  /* timerfd_settime / gettime */
        case 319: {                                                  /* memfd_create: a node nobody can find by name */
            if (b & 0x3ffffe0) return -EINVAL;                       /* mojo probes with junk flags to see if the kernel checks */
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
            n->seals = ((b & 2) ? 0 : 1) | ((b & 8) ? 0x20 : 0);       /* !ALLOW_SEALING: F_SEAL_SEAL, NOEXEC_SEAL: F_SEAL_EXEC */
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
            if (a == 0x1001) { p->gs_base = b; wrmsr_ugs(b); return 0; }   /* ARCH_SET_GS, user gs lives in KERNEL_GS_BASE here */
            if (a == 0x1004) { UCHK((void*)b, 8); *(uint64_t*)b = p->gs_base; return 0; }
            if (a == 0x1003) { UCHK((void*)b, 8); *(uint64_t*)b = p->tls_base; return 0; }
            return -EINVAL;
        case 218: p->clear_child_tid = a; return ns_gpid(p, p->pid);             /* set_tid_address */
        case 228: return do_clock_gettime((int)a, (int64_t*)b);
        case 229:
            if (b) { UCHK((void*)b, 16); memset((void*)b, 0, 16); ((int64_t*)b)[1] = 1000000; }
            return 0;
        case 164: {                                                  /* settimeofday */
            if (!a) return 0;
            UCHK((void*)a, 16);
            clock_set((uint32_t)((int64_t*)a)[0], (uint32_t)((int64_t*)a)[1] * 1000);
            return 0;
        }
        case 227:                                                    /* clock_settime */
            if (a != 0) return -EINVAL;
            UCHK((void*)b, 16);
            clock_set((uint32_t)((int64_t*)b)[0], (uint32_t)((int64_t*)b)[1]);
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
            if (!path) { file_t* fl = getf(dfd); if (fl && fl->type == F_NODE) { fl->node->mtime = mt; ino_node(fl->node, 4); } return 0; }
            UCHK(path, 1);
            n = lookup_peek(dfd, path, &err, !(r->rax == 280 && (d & 0x100)));
            if (!n) return err;
            n->mtime = mt;
            ino_node(n, 4);
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
        case 275: return do_splice((int)a, (uint64_t*)b, (int)c, (uint64_t*)d, e);
        case 276: {                                                  /* tee */
            file_t* fa = getf((int)a);
            file_t* fb = getf((int)b);
            if (!fa || !fb) return -EBADF;
            if (fa->type != F_PIPE_R || fb->type != F_PIPE_W || fa->pipe == fb->pipe) return -EINVAL;
            return pipe_tee(fa, fb, c > 0x7fffffff ? 0x7fffffff : (uint32_t)c);
        }
        case 278: {                                                  /* vmsplice */
            file_t* fv = getf((int)a);
            if (!fv) return -EBADF;
            if (fv->type != F_PIPE_R && fv->type != F_PIPE_W) return -EBADF;
            return do_rwv((int)a, (iovec_t*)b, (int)c, fv->type == F_PIPE_W);
        }
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
bool fs_syncing_now(void) { return fs_syncing; }

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
        case 76: case 90: case 268: case 452: case 132: case 235: case 280: case 326: case 275:
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
    mem_check();
    proc_check_alarm(proc_current(), false);
    bool mut = changes_fs(nr, r->rdi);
    if (mut) {
        ext2_throttle();                      /* before: the sync waits for writers */
        fs_write_begin();
        nb_tree_close();
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
    if (mut) { nb_tree_open(); fs_write_end(); }
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

/* /proc, /sys and /dev change behind the tree gate's back, they keep the bkl */
static bool nb_odd(fs_node_t* n) {
    for (; n; n = n->parent)
        if (n->parent == fs_root() && (!strcmp(n->name, "proc") || !strcmp(n->name, "sys") || !strcmp(n->name, "dev"))) return true;
    return false;
}

static int64_t nb_getdents(file_t* f, uint8_t* ub, uint64_t n) {
    fs_node_t* dir = f->node;
    uint8_t tmp[2048];
    if (dir->type != FS_DIR || nb_odd(dir) || dir->dev) return NB_SLOW;
    if (n > sizeof(tmp)) n = sizeof(tmp);
    uint64_t fl = irq_save();
    if (!nb_tree_enter()) { irq_restore(fl); return NB_SLOW; }
    uint64_t o = f->off;
    uint32_t pos = 0, idx = 0;
    fs_node_t* c = dir->child;
    int64_t ret = 0;
    for (;; idx++) {
        const char* name;
        fs_node_t* node;
        if (idx == 0)      { name = ".";  node = dir; }
        else if (idx == 1) { name = ".."; node = dir->parent ? dir->parent : dir; }
        else {
            if (!c) break;
            name = c->name; node = c->hl ? c->hl : c; c = c->next;
        }
        if (idx < o) continue;
        uint32_t nl = strlen(name);
        uint32_t reclen = (19 + nl + 1 + 7) & ~7u;
        if (pos + reclen > n) { if (pos == 0) ret = -EINVAL; break; }
        uint8_t* d = tmp + pos;
        uint64_t ino = ((uint64_t)node >> 4) & 0x0FFFFFFF;
        int64_t next = idx + 1;
        memcpy(d, &ino, 8);
        memcpy(d + 8, &next, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(d + 16, &rl, 2);
        d[18] = node->dev ? 2 : node->type == FS_DIR ? 4 : node->type == FS_LINK ? 10 : 8;
        memcpy(d + 19, name, nl + 1);
        pos += reclen;
        o = idx + 1;
    }
    nb_tree_leave();
    irq_restore(fl);
    if (ret) return ret;
    f->off = o;
    memcpy(ub, tmp, pos);
    return pos;
}

static int64_t nb_statat(proc_t* p, int dirfd, const char* path, kstat64_t* ust, int flags) {
    char pb[256];
    kstat64_t st;
    if (!ustr_ok(path)) return -EFAULT;
    int i = 0;
    while (i < 255 && path[i]) { pb[i] = path[i]; i++; }
    if (path[i] || !i) return NB_SLOW;
    pb[i] = 0;
    for (i = 0; pb[i]; i++) if (pb[i] == '.' && (i == 0 || pb[i - 1] == '/')) return NB_SLOW;
    fs_node_t* base = p->sh->cwd;
    file_t* df = NULL;
    if (pb[0] != '/' && dirfd != -100) {
        df = getf_ref(dirfd);
        if (!df) return NB_SLOW;
        if (df->type != F_NODE || df->node->type != FS_DIR) { file_close(df); return NB_SLOW; }
        base = df->node;
    }
    int64_t ret = NB_SLOW;
    if (pb[0] == '/' ? strncmp(pb, "/proc", 5) && strncmp(pb, "/sys", 4) && strncmp(pb, "/dev", 4) : !nb_odd(base)) {
        uint64_t fl = irq_save();
        if (nb_tree_enter()) {
            fs_node_t* n = fs_peek(base, pb, !(flags & 0x100));
            if (!n) ret = -ENOENT;
            else if (!nb_odd(n)) { fill_stat_node(&st, n); ret = 0; }
            nb_tree_leave();
        }
        irq_restore(fl);
    }
    if (df) file_close(df);
    if (ret == 0) memcpy(ust, &st, sizeof(st));
    return ret;
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
        case 39: ret = p->pidns ? NB_SLOW : p->tgid; break;
        case 186: ret = p->pidns ? NB_SLOW : p->pid; break;
        case 102: case 104: case 107: case 108: ret = p->userns ? NB_SLOW : 0; break;
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
        case 0: case 1: {                            /* pipes, eventfd, cached files */
            file_t* f = getf_ref((int)a);
            if (f && f->type == F_NODE) {
                if (!uok((void*)b, c)) ret = -EFAULT;
                else if ((f->flags & O_ACCMODE) == (nr == 0 ? O_WRONLY : 0)) ret = -EBADF;
                else ret = node_nb_rw(f, (char*)b, c, -1, nr == 1);
                file_close(f);
                break;
            }
            if (!f || (f->type != F_PIPE_R && f->type != F_PIPE_W && f->type != F_EVENTFD && f->type != F_SPAIR)) { file_close(f); ret = NB_SLOW; break; }
            if (nr == 0 ? f->type == F_PIPE_W : f->type == F_PIPE_R) ret = -EBADF;
            else if (!uok((void*)b, c)) ret = -EFAULT;
            else ret = nr == 0 ? file_read(f, (char*)b, c) : file_write(f, (const char*)b, c);
            file_close(f);
            break;
        }
        case 44: case 45: case 46: case 47: {         /* unix socketpair ends: sockets proper stay on the lock */
            file_t* f = getf_ref((int)a);
            if (!f || f->type != F_SPAIR) { file_close(f); ret = NB_SLOW; break; }
            ret = spair_call(nr == 44 ? 11 : nr == 45 ? 12 : nr == 46 ? 16 : 17, f, b, c, r->r10, r->r8);
            file_close(f);
            break;
        }
        case 284: case 290: ret = do_eventfd(nr, a, b); break;
        case 283: ret = do_timerfd_create(a, b); break;
        case 286: case 287: ret = do_timerfd(nr, a, b, c, r->r10); break;
        case 16: {                                   /* drm render nodes, drm.c has its own lock */
            file_t* f = getf_ref((int)a);
            if (!f || f->type != F_DRM || !f->disk || ((b >> 8) & 0xff) != 'd' || (b & 0xffff) == 0x642d || (b & 0xffff) == 0x642e
                || ((b & 0xffff) == 0x6442 && (!uok((void*)c, 32) || (((uint32_t*)c)[0] & 2)))) { file_close(f); ret = NB_SLOW; break; }
            ret = drm_ioctl(f->drm, (uint32_t)b, (void*)c);
            file_close(f);
            break;
        }
        case 7: ret = do_poll((pollfd_t*)a, b, (int)c); break;
        case 271: ret = do_poll((pollfd_t*)a, b, timeout_ms(c, 1000000)); break;
        case 23: ret = do_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)r->r10, timeout_ms(r->r8, 1000)); break;
        case 270: ret = do_select((int)a, (uint32_t*)b, (uint32_t*)c, (uint32_t*)r->r10, timeout_ms(r->r8, 1000000)); break;
        case 232: case 281: ret = do_epoll_wait((int)a, (uint32_t*)b, (int)c, (int)r->r10); break;
        case 441: ret = do_epoll_wait((int)a, (uint32_t*)b, (int)c, timeout_ms(r->r10, 1000000)); break;
        case 233: ret = do_epoll_ctl((int)a, (int)b, (int)c, (uint32_t*)r->r10); break;
        case 17: case 18: {
            file_t* f = getf_ref((int)a);
            if (!f || f->type != F_NODE || (int64_t)r->r10 < 0) { file_close(f); ret = NB_SLOW; break; }
            if (!uok((void*)b, c)) ret = -EFAULT;
            else if ((f->flags & O_ACCMODE) == (nr == 17 ? O_WRONLY : 0)) ret = -EBADF;
            else ret = node_nb_rw(f, (char*)b, c, r->r10, nr == 18);
            file_close(f);
            break;
        }
        case 8: {
            file_t* f = getf_ref((int)a);
            if (!f || f->type != F_NODE || c > 2) { file_close(f); ret = NB_SLOW; break; }
            int64_t base = c == 0 ? 0 : c == 1 ? (int64_t)f->off : (int64_t)f->node->size;
            ret = base + (int64_t)b < 0 ? -EINVAL : base + (int64_t)b;
            if (ret >= 0) f->off = ret;
            file_close(f);
            break;
        }
        case 5: {
            file_t* f = getf_ref((int)a);
            if (!f || f->type != F_NODE || nb_odd(f->node)) { file_close(f); ret = NB_SLOW; break; }
            kstat64_t st;
            UCHK2((void*)b, sizeof(st));
            fill_stat_node(&st, f->node);
            file_close(f);
            memcpy((void*)b, &st, sizeof(st));
            ret = 0;
            break;
        }
        case 262: {
            UCHK2((void*)r->rdx, sizeof(kstat64_t));
            if (r->r10 & 0x1000) ret = NB_SLOW;              /* AT_EMPTY_PATH: rare, bkl */
            else ret = nb_statat(p, (int)a, (const char*)b, (kstat64_t*)c, (int)r->r10);
            break;
        }
        case 217: {
            file_t* f = getf_ref((int)a);
            if (!f || f->type != F_NODE) { file_close(f); ret = NB_SLOW; break; }
            if (!uok((void*)b, c)) ret = -EFAULT;
            else ret = nb_getdents(f, (uint8_t*)b, c);
            file_close(f);
            break;
        }
        case 9: {                                    /* anon only, the rest keeps the lock */
            if (!(r->r10 & MAP_ANON) || p->sh->rl[9][0] != ~0ull || p->sh->rl[2][0] != ~0ull) { ret = NB_SLOW; break; }
            uint64_t len = (b + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (!b) { ret = -EINVAL; break; }
            bool fx = r->r10 & MAP_FIXED;
            if (fx && ((a & (PAGE_SIZE - 1)) || a < USER_BASE || a + len > USER_TOP || a + len < a)) { ret = -EINVAL; break; }
            uint64_t m = vmm_map_anon(p->pd, a, fx, USER_MMAP_BASE, USER_STACK_TOP - USER_STACK_MAX, len, (c & PROT_WRITE) != 0, c != 0);
            ret = m ? (int64_t)m : -ENOMEM;
            break;
        }
        case 12:
            if (p->sh->rl[9][0] != ~0ull || p->sh->rl[2][0] != ~0ull) { ret = NB_SLOW; break; }
            ret = do_brk(a);
            break;
        case 28:        // DONTNEED has to zero, this ate it before the slow path saw it (chromium xkb assert)
            if (c == 4 && !(a & (PAGE_SIZE - 1)) && a >= USER_BASE && a < USER_TOP) { vmm_discard(p->pd, a, b); vmm_flush(); }
            ret = 0;
            break;
        case 149: case 150: case 151: case 152: ret = 0; break;     /* mlock&co do nothing here */
        case 25: {
            bool stk = a >= USER_STACK_TOP - USER_STACK_MAX && a < USER_STACK_TOP;
            if (!stk && !(vmm_pte(p->pd, a & ~(PAGE_SIZE - 1)) & (PTE_P | PTE_SWAP))) ret = -EFAULT;
            else ret = -ENOMEM;
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
    static const uint16_t nb[] = { 0, 1, 5, 8, 17, 18, 217, 262, 9, 10, 11, 39, 186, 102, 104, 107, 108, 96, 228, 229, 24, 35, 230, 202,
                                     44, 45, 46, 47, 7, 271, 23, 270, 232, 281, 441, 233, 12, 28, 149, 150, 151, 152, 25, 284, 290, 283, 286, 287, 16 };
    for (unsigned i = 0; i < sizeof(nb) / sizeof(nb[0]); i++) nobkl_tab[nb[i]] = 1;
}
