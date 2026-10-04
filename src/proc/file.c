#include "proc/file.h"
#include "proc/pty.h"
#include "proc/proc.h"
#include "proc/uring.h"
#include "proc/tty.h"
#include "proc/proc.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"
#include "drivers/ata.h"
#include "net/sock.h"
#include "drivers/fbdev.h"
#include "drivers/input.h"

#define O_ACCMODE  3
#define O_WRONLY   1
#define O_APPEND   02000
#define O_NONBLOCK 04000

#define EAGAIN 11
#define EINTR  4
#define EPIPE  32
#define EISDIR 21
#define ENOMEM 12
#define EBADF  9

file_t* file_new(ftype_t type, int flags) {
    file_t* f = (file_t*)kmalloc(sizeof(file_t));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->type = type;
    f->refs = 1;
    f->flags = flags;
    return f;
}

static file_t* open_pty_slave(int i, int flags) {
    if (pty_slave_open(i, flags) < 0) return NULL;
    file_t* f = file_new(F_PTS, flags);
    if (!f) { pty_slave_close(i); return NULL; }
    f->pty = i;
    return f;
}

file_t* file_open_node(fs_node_t* n, int flags) {
    static const ftype_t dev_type[] = { 0, F_NULL, F_ZERO, F_TTY, F_RANDOM, F_FB, F_INPUT };
    if (n->dev == FS_DEV_SOCK) return NULL;                   /* sockets get connect(), not open() */
    if (n->dev == FS_DEV_EVKBD || n->dev == FS_DEV_EVMOUSE) {
        file_t* f = file_new(F_INPUT, flags);
        if (!f) return NULL;
        f->disk = n->dev == FS_DEV_EVKBD ? INPUT_KBD : INPUT_MOUSE;   /* which ring */
        input_open(f->disk);
        return f;
    }
    if (n->dev == FS_DEV_PTMX) {                               /* new pty pair */
        int i = pty_alloc();
        if (i < 0) return NULL;
        file_t* f = file_new(F_PTM, flags);
        if (!f) { pty_master_close(i); return NULL; }
        f->pty = i;
        return f;
    }
    if (FS_DEV_IS_PTS(n->dev)) return open_pty_slave(n->dev - FS_DEV_PTS, flags);
    if (n->dev == FS_DEV_TTY) {                                /* /dev/tty: the process's terminal */
        proc_t* me = proc_current();
        if (me && me->ctty > 0) return open_pty_slave(me->ctty - 1, flags | 0x100);
        if (me && me->ctty < 0) return NULL;                  /* no terminal (setsid, daemons) */
    }
    if (FS_DEV_IS_DISK(n->dev)) {
        file_t* f = file_new(F_DISK, flags);
        if (f) f->disk = n->dev - FS_DEV_DISK;
        return f;
    }
    if (n->dev && n->dev < sizeof(dev_type) / sizeof(dev_type[0])) {
        ftype_t t = dev_type[n->dev];
        if (t == F_FB && !fbdev_open()) return NULL;
        file_t* f = file_new(t, flags);
        if (t == F_INPUT && f) { f->disk = INPUT_ALL; input_open(INPUT_ALL); }
        else if (t == F_FB && !f) fbdev_close();
        return f;
    }
    file_t* f = file_new(F_NODE, flags);
    if (!f) return NULL;
    f->node = n;
    n->refs++;
    return f;
}

void file_ref(file_t* f) { if (f) f->refs++; }

void file_close(file_t* f) {
    if (!f || --f->refs > 0) return;
    if (f->type == F_NODE && f->node) fs_release(f->node);
    if (f->type == F_SOCKET && f->sock) sock_close(f->sock);
    if (f->type == F_FB) fbdev_close();
    if (f->type == F_INPUT) input_close(f->disk);
    if (f->type == F_PTM) pty_master_close(f->pty);
    if (f->type == F_PTS) pty_slave_close(f->pty);
    if (f->type == F_USOCK || f->type == F_ULISTEN) ux_release(f);
    if (f->type == F_EPOLL && f->ep) kfree(f->ep);
    if (f->type == F_URING && f->ur) uring_release(f->ur);
    if (f->type == F_SPAIR) {
        spair_shutdown(f, 2);
        if (f->pipe->readers <= 0 && f->pipe->writers <= 0) kfree(f->pipe);
        if (f->pipe2->readers <= 0 && f->pipe2->writers <= 0) kfree(f->pipe2);
    } else if (f->pipe) {
        if (f->type == F_PIPE_R) f->pipe->readers--;
        else                     f->pipe->writers--;
        if (f->pipe->readers <= 0 && f->pipe->writers <= 0) kfree(f->pipe);
    }
    kfree(f);
}

int pipe_create(file_t** rd, file_t** wr) {
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    memset(p, 0, sizeof(*p));
    *rd = file_new(F_PIPE_R, 0);
    *wr = file_new(F_PIPE_W, O_WRONLY);
    if (!*rd || !*wr) { kfree(p); if (*rd) kfree(*rd); if (*wr) kfree(*wr); return -ENOMEM; }
    (*rd)->pipe = (*wr)->pipe = p;
    p->readers = p->writers = 1;
    return 0;
}

/* AF_UNIX SOCK_STREAM pair: two pipes, one per direction. */
int spair_create(file_t** a, file_t** b) {
    pipe_t* p1 = (pipe_t*)kmalloc(sizeof(pipe_t));
    pipe_t* p2 = (pipe_t*)kmalloc(sizeof(pipe_t));
    *a = file_new(F_SPAIR, 2);
    *b = file_new(F_SPAIR, 2);
    if (!p1 || !p2 || !*a || !*b) {
        if (p1) kfree(p1);
        if (p2) kfree(p2);
        if (*a) kfree(*a);
        if (*b) kfree(*b);
        return -ENOMEM;
    }
    memset(p1, 0, sizeof(*p1));
    memset(p2, 0, sizeof(*p2));
    (*a)->pipe = p1; (*a)->pipe2 = p2;          /* a reads p1, writes p2 */
    (*b)->pipe = p2; (*b)->pipe2 = p1;          /* b reads p2, writes p1 */
    p1->readers = p1->writers = 1;
    p2->readers = p2->writers = 1;
    return 0;
}

/* how: 0 = SHUT_RD, 1 = SHUT_WR, 2 = both. */
int spair_shutdown(file_t* f, int how) {
    if (how < 0 || how > 2) return -22;             /* EINVAL */
    if ((how == 0 || how == 2) && !(f->shut & 1)) { f->shut |= 1; f->pipe->readers--; }
    if ((how == 1 || how == 2) && !(f->shut & 2)) { f->shut |= 2; f->pipe2->writers--; }
    return 0;
}

/* ---------------- ramfs data ---------------- */

/* Room for `need` bytes of owned, writable contents (borrowed boot-archive
   data is copied here first). */
static int node_reserve(fs_node_t* n, uint32_t need) {
    if (n->cap >= need + 1) return 0;
    uint32_t cap = n->cap ? n->cap : 64;
    if (cap < n->size + 1) cap = n->size + 1;
    while (cap < need + 1) cap = cap < (1u << 20) ? cap * 2 : cap + cap / 4;   /* big files: gentle growth */
    if (n->data && n->cap && kgrow(n->data, cap)) { n->cap = cap; return 0; }   /* no copy, no second buffer */
    char* nb = (char*)kmalloc_big(cap);
    for (int t = 0; !nb && t < 3; t++) {          /* dirty files fill the arena until they're on disk */
        fs_wait_room(cap);
        if (n->data && n->cap && kgrow(n->data, cap)) { n->cap = cap; return 0; }
        nb = (char*)kmalloc_big(cap);
    }
    if (!nb) return -ENOMEM;
    if (n->data) memcpy(nb, n->data, n->size);
    uint32_t size = n->size;
    fs_data_free(n);
    n->data = nb;
    n->size = size;
    n->cap = cap;
    return 0;
}

int node_write_at(fs_node_t* n, uint32_t off, const char* buf, uint32_t len) {
    if (n->type != FS_FILE) return -EISDIR;
    uint32_t end = off + len;
    if (node_reserve(n, end > n->size ? end : n->size) < 0) return -ENOMEM;
    if (off > n->size) memset(n->data + n->size, 0, off - n->size);
    memcpy(n->data + off, buf, len);
    if (end > n->size) n->size = end;
    n->data[n->size] = 0;
    n->mtime = fs_now();
    fs_touch(n);
    return (int)len;
}

int node_truncate(fs_node_t* n, uint32_t len) {
    if (n->type != FS_FILE) return -EISDIR;
    if (n->data && !n->cap && node_reserve(n, n->size) < 0) return -ENOMEM;   /* borrowed */
    if (len > n->size) {
        if (node_reserve(n, len) < 0) return -ENOMEM;
        memset(n->data + n->size, 0, len - n->size);
    }
    n->size = len;
    if (n->data) n->data[len] = 0;
    n->mtime = fs_now();
    fs_touch(n);
    return 0;
}

/* ---------------- block devices ---------------- */

uint32_t file_disk_size(file_t* f) {
    uint32_t s = ata_drive_sectors(f->disk);
    return s >= 0x800000u ? 0xFFFFFE00u : s * 512;           /* clamp to 4 GiB */
}

/* Byte-granular access through a one-sector bounce buffer. */
static int disk_rw(file_t* f, char* buf, uint32_t n, bool write) {
    uint32_t size = file_disk_size(f);
    if (f->off >= size) return 0;
    if (n > size - f->off) n = size - f->off;
    uint8_t* sec = (uint8_t*)kmalloc(512);
    if (!sec) return -ENOMEM;
    uint32_t done = 0;
    while (done < n) {
        uint32_t lba = f->off / 512, in = f->off % 512;
        uint32_t k = 512 - in;
        if (k > n - done) k = n - done;
        if (ata_read(f->disk, lba, 1, sec) < 0) break;
        if (write) {
            memcpy(sec + in, buf + done, k);
            if (ata_write(f->disk, lba, 1, sec) < 0) break;
        } else {
            memcpy(buf + done, sec + in, k);
        }
        done += k;
        f->off += k;
    }
    kfree(sec);
    return done ? (int)done : -5;                             /* EIO */
}

/* ---------------- read / write ---------------- */

static uint32_t rng_state = 0x2545F491;
static uint8_t rnd8(void) {
    rng_state ^= pit_ticks() + 0x9E3779B9u;
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return (uint8_t)rng_state;
}

bool file_readable(file_t* f) {
    switch (f->type) {
        case F_TTY:    return tty_readable();
        case F_PIPE_R: return f->pipe->count > 0 || f->pipe->writers <= 0;
        case F_PIPE_W: return false;
        case F_SPAIR:  return f->pipe->count > 0 || f->pipe->writers <= 0 || (f->shut & 1);
        case F_SOCKET: return sock_readable(f->sock);
        case F_ULISTEN: return ux_pending(f);
        case F_EVENTFD: return f->cnt > 0;
        case F_URING:  return uring_readable(f->ur);
        case F_TIMERFD: return f->t_next && (int32_t)(pit_uptime_ms() - f->t_next) >= 0;
        case F_USOCK:  return false;
        case F_INPUT:  return input_pending(f->disk);
        case F_PTM:    return pty_readable(f->pty, true);
        case F_PTS:    return pty_readable(f->pty, false);
        default:       return true;
    }
}

bool file_writable(file_t* f) {
    if (f->type == F_PIPE_W) return f->pipe->count < PIPE_SZ || f->pipe->readers <= 0;
    if (f->type == F_SOCKET) return sock_writable(f->sock);
    if (f->type == F_SPAIR) return (f->shut & 2) || f->pipe2->count < PIPE_SZ || f->pipe2->readers <= 0;
    if (f->type == F_PTM || f->type == F_PTS) return pty_writable(f->pty, f->type == F_PTM);
    if (f->type == F_USOCK || f->type == F_ULISTEN) return false;
    return f->type != F_PIPE_R;
}

static int pipe_read(file_t* f, pipe_t* p, char* buf, uint32_t n) {
    while (p->count == 0) {
        if (p->writers <= 0) return 0;
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (proc_interrupted()) return -EINTR;
        task_yield();
    }
    uint32_t got = 0;
    while (got < n && p->count > 0) {                /* at most two runs around the ring */
        uint32_t k = PIPE_SZ - (uint32_t)p->tail;
        if (k > (uint32_t)p->count) k = (uint32_t)p->count;
        if (k > n - got) k = n - got;
        memcpy(buf + got, p->buf + p->tail, k);
        got += k;
        p->tail = (p->tail + (int)k) % PIPE_SZ;
        p->count -= (int)k;
    }
    return (int)got;
}

static int pipe_write(file_t* f, pipe_t* p, const char* buf, uint32_t n) {
    uint32_t put = 0;
    while (put < n) {
        if (p->readers <= 0) {
            proc_t* me = proc_current();
            if (me) proc_send_signal(me, 13);   /* SIGPIPE */
            return put ? (int)put : -EPIPE;
        }
        if (p->count == PIPE_SZ) {
            if (f->flags & O_NONBLOCK) return put ? (int)put : -EAGAIN;
            if (proc_interrupted()) return put ? (int)put : -EINTR;
            task_yield();
            continue;
        }
        uint32_t k = PIPE_SZ - (uint32_t)p->head, room = PIPE_SZ - (uint32_t)p->count;
        if (k > room) k = room;
        if (k > n - put) k = n - put;
        memcpy(p->buf + p->head, buf + put, k);
        put += k;
        p->head = (p->head + (int)k) % PIPE_SZ;
        p->count += (int)k;
    }
    return (int)put;
}

int file_read(file_t* f, char* buf, uint32_t n) {
    switch (f->type) {
        case F_URING: return -22;
        case F_NULL: case F_NETLINK: case F_USOCK: case F_ULISTEN: case F_EPOLL: return 0;   // netlink goes through recv
        case F_EVENTFD: case F_TIMERFD: {               /* both hand out a u64 */
            if (n < 8) return -22;
            uint64_t v;
            for (;;) {
                if (f->type == F_EVENTFD && f->cnt) {
                    v = (f->flags & 0x10000000) ? 1 : f->cnt;  /* EFD_SEMAPHORE, kept in a spare flag bit */
                    f->cnt -= v;
                    break;
                }
                if (f->type == F_TIMERFD && f->t_next && (int32_t)(pit_uptime_ms() - f->t_next) >= 0) {
                    uint32_t now = pit_uptime_ms();
                    if (f->t_int) { v = 1 + (now - f->t_next) / f->t_int; f->t_next += (uint32_t)v * f->t_int; }
                    else { v = 1; f->t_next = 0; }
                    break;
                }
                if (f->flags & O_NONBLOCK) return -11;
                if (proc_interrupted()) return -4;
                task_sleep_ms(1);
            }
            memcpy(buf, &v, 8);
            return 8;
        }
        case F_ZERO: memset(buf, 0, n); return (int)n;
        case F_RANDOM: for (uint32_t i = 0; i < n; i++) buf[i] = (char)rnd8(); return (int)n;
        case F_TTY:  return tty_read(buf, (int)n, (f->flags & O_NONBLOCK) != 0);
        case F_PTM: case F_PTS:
            return pty_read(f->pty, f->type == F_PTM, buf, (int)n, (f->flags & O_NONBLOCK) != 0);
        case F_DISK: return disk_rw(f, buf, n, false);
        case F_FB:   return -EBADF;
        case F_INPUT: return input_read(f->disk, buf, n, (f->flags & O_NONBLOCK) != 0);
        case F_SOCKET: return sock_recv(f->sock, (uint8_t*)buf, n, (f->flags & O_NONBLOCK) != 0, false, 0, 0);
        case F_PIPE_W: return -EBADF;
        case F_SPAIR:
            if (f->shut & 1) return 0;
            return pipe_read(f, f->pipe, buf, n);
        case F_PIPE_R: return pipe_read(f, f->pipe, buf, n);
        case F_NODE: {
            fs_node_t* nd = f->node;
            if (nd->type == FS_DIR) return -EISDIR;
            if (f->off >= nd->size) return 0;
            uint32_t k = nd->size - f->off;
            if (k > n) k = n;
            memcpy(buf, nd->data + f->off, k);
            f->off += k;
            return (int)k;
        }
    }
    return -EBADF;
}

int file_write(file_t* f, const char* buf, uint32_t n) {
    switch (f->type) {
        case F_NULL: case F_ZERO: case F_RANDOM: case F_NETLINK: return (int)n;
        case F_EVENTFD: {
            if (n < 8) return -22;
            uint64_t v;
            memcpy(&v, buf, 8);
            if (v == ~0ull) return -22;
            f->cnt += v;
            return 8;
        }
        case F_TIMERFD: return -22;
        case F_USOCK: case F_ULISTEN: return -107;   /* ENOTCONN */
        case F_EPOLL: case F_URING: return -22;
        case F_TTY:  return tty_write(buf, (int)n);
        case F_PTM: case F_PTS:
            return pty_write(f->pty, f->type == F_PTM, buf, (int)n, (f->flags & O_NONBLOCK) != 0);
        case F_DISK: return disk_rw(f, (char*)buf, n, true);
        case F_INPUT: return (int)n;                  /* LED events from xorg: nothing to light */
        case F_FB: {
            int r = fbdev_write(f->off, buf, n);
            if (r > 0) f->off += (uint32_t)r;
            return r;
        }
        case F_SOCKET: return sock_send(f->sock, (const uint8_t*)buf, n, (f->flags & O_NONBLOCK) != 0, 0, 0);
        case F_PIPE_R: return -EBADF;
        case F_SPAIR:
            if (f->shut & 2) {
                proc_t* me = proc_current();
                if (me) proc_send_signal(me, 13);
                return -EPIPE;
            }
            return pipe_write(f, f->pipe2, buf, n);
        case F_PIPE_W: return pipe_write(f, f->pipe, buf, n);
        case F_NODE: {
            if (f->flags & O_APPEND) f->off = f->node->size;
            int r = node_write_at(f->node, f->off, buf, n);
            if (r > 0) f->off += (uint32_t)r;
            return r;
        }
    }
    return -EBADF;
}
