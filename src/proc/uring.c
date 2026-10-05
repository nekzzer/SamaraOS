#include "proc/uring.h"
#include "proc/io_uring.h"
#include "proc/proc.h"
#include "proc/file.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"
#include "boot/pit.h"
#include "fs/ext2.h"
#include "fs/fatfs.h"

/* io_uring, linux uapi compatible (liburing runs as is). Submission is
   inline when the file is ready, the rest waits in one global list and
   a few worker tasks poll it every ms and run what became ready, in the
   address space of whoever submitted it. Same polling style as poll/epoll. */

#define EPERM 1
#define ENXIO 6
#define ENOENT 2
#define EINTR 4
#define EBADF 9
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EINVAL 22
#define ESPIPE 29
#define ECANCELED 125
#define EALREADY 114
#define ETIME 62
#define EOPNOTSUPP 95

#define MAX_ENTRIES 32768
#define NWORK 3
#define AT_FDCWD (-100)

_Static_assert(sizeof(struct io_uring_sqe) == 64, "sqe");
_Static_assert(sizeof(struct io_uring_cqe) == 16, "cqe");
_Static_assert(sizeof(struct io_uring_params) == 120, "params");

enum { R_HELD, R_WAIT, R_RUN };

typedef struct req {
    struct req *next, *snext;
    struct uring* r;
    struct proc* p;
    struct io_uring_sqe sqe;
    struct file* f;
    struct req *link, *lt, *target;     /* chain, its linked timeout / the one a timeout guards */
    int st, bad;
    uint32_t seq;
    uint32_t deadline, start_ncq;       /* timeouts: uptime ms, cqes seen when armed */
    uint32_t count, ms;
    int16_t last;
    uint32_t gen;
    bool armed, pre;                    /* pre: res is known, no need to run anything */
    int res;
} req_t;

typedef struct ovf {
    struct ovf* next;
    struct io_uring_cqe c;
} ovf_t;

typedef struct uring {
    uint32_t sq_n, cq_n, sq_mask, cq_mask, flags;
    uint8_t *rings, *sqes;
    uint64_t rings_pa, sqes_pa;
    uint32_t rings_np, sqes_np;
    volatile uint32_t *sq_head, *sq_tail, *cq_head, *cq_tail, *sq_flags, *sq_drop, *cq_ovf;
    uint32_t* sq_array;
    struct io_uring_cqe* cqes;
    struct io_uring_sqe* sq;
    ovf_t *ovf, *ovf_last;
    uint32_t seq, ncq;
    int running, ndrain;
    uint32_t ntmo;                      /* timeouts that expired, they cut a wait short */
    bool dead;
    struct file** files;
    uint32_t nfiles;
    struct { uint64_t base, len; } *bufs;
    uint32_t nbufs;
    struct file* efd;
    int waiter;                         /* task blocked in enter, -1 none */
} uring_t;

static req_t* reqs;                     /* everything that is not finished */
static int workers[NWORK];

#define bar() __asm__ volatile ("" : : : "memory")

static void kick(void) {
    uint32_t fl = irq_save();
    for (int i = 0; i < NWORK; i++) {
        task_t* t = task_at(workers[i]);
        if (t && t->state == T_BLOCKED) { t->wake_ms = 0; t->state = T_READY; }
    }
    irq_restore(fl);
}

static void cq_flush(uring_t* r) {
    while (r->ovf && *r->cq_tail - *r->cq_head < r->cq_n) {
        ovf_t* o = r->ovf;
        r->ovf = o->next;
        if (!r->ovf) r->ovf_last = NULL;
        r->cqes[*r->cq_tail & r->cq_mask] = o->c;
        bar();
        (*r->cq_tail)++;
        kfree(o);
    }
    if (!r->ovf) *r->sq_flags &= ~IORING_SQ_CQ_OVERFLOW;
}

// irq must be off here
static void cq_post(uring_t* r, __u64 ud, int res, uint32_t fl) {
    cq_flush(r);
    if (!r->ovf && *r->cq_tail - *r->cq_head < r->cq_n) {
        struct io_uring_cqe* c = &r->cqes[*r->cq_tail & r->cq_mask];
        c->user_data = ud; c->res = res; c->flags = fl;
        bar();
        (*r->cq_tail)++;
    } else {
        ovf_t* o = kmalloc(sizeof(ovf_t));
        if (!o) { (*r->cq_ovf)++; return; }     // dropped, nothing to do
        o->next = NULL;
        o->c.user_data = ud; o->c.res = res; o->c.flags = fl;
        if (r->ovf_last) r->ovf_last->next = o; else r->ovf = o;
        r->ovf_last = o;
        *r->sq_flags |= IORING_SQ_CQ_OVERFLOW;
    }
    r->ncq++;
    if (r->efd && !(*(volatile uint32_t*)(r->rings + 280) & 1)) r->efd->cnt++;      // cq_flags: EVENTFD_DISABLED
    if (r->waiter >= 0) {
        task_t* t = task_at(r->waiter);
        if (t && t->state == T_BLOCKED) { t->wake_ms = 0; t->state = T_READY; }
    }
}

bool uring_readable(uring_t* r) { return r && *r->cq_tail != *r->cq_head; }

static uint32_t pow2(uint32_t n) {
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

int uring_setup(uint32_t entries, void* params) {
    struct io_uring_params* pr = params;
    if (!entries || entries > MAX_ENTRIES * 8) return -EINVAL;
    if (!sys_uok(pr, sizeof(*pr))) return -EFAULT;
    if (pr->flags & ~0x33F9u) return -EINVAL;           // no sqpoll, sqe128, cqe32 and the newer ones
    if (entries > MAX_ENTRIES) {
        if (!(pr->flags & IORING_SETUP_CLAMP)) return -EINVAL;
        entries = MAX_ENTRIES;
    }
    uint32_t sn = pow2(entries), cn = sn * 2;
    if (pr->flags & IORING_SETUP_CQSIZE) {
        if (pr->cq_entries < sn) return -EINVAL;
        cn = pow2(pr->cq_entries);
        if (cn > MAX_ENTRIES * 2) {
            if (!(pr->flags & IORING_SETUP_CLAMP)) return -EINVAL;
            cn = MAX_ENTRIES * 2;
        }
    }
    uring_t* r = kmalloc(sizeof(uring_t));
    if (!r) return -ENOMEM;
    memset(r, 0, sizeof(*r));
    r->sq_n = sn; r->cq_n = cn; r->sq_mask = sn - 1; r->cq_mask = cn - 1;
    r->flags = pr->flags;
    r->waiter = -1;
    uint32_t arr = 320 + cn * 16;
    uint32_t rsz = arr + sn * 4;
    r->rings_np = (rsz + PAGE_SIZE - 1) / PAGE_SIZE;
    r->sqes_np = (sn * 64 + PAGE_SIZE - 1) / PAGE_SIZE;
    r->rings_pa = pmm_alloc_run(r->rings_np);
    r->sqes_pa = pmm_alloc_run(r->sqes_np);
    if (!r->rings_pa || !r->sqes_pa) goto nomem;
    r->rings = P2V(r->rings_pa);
    r->sqes = P2V(r->sqes_pa);
    r->sq = (struct io_uring_sqe*)r->sqes;
    r->sq_head = (uint32_t*)r->rings;
    r->sq_tail = (uint32_t*)(r->rings + 64);
    r->cq_head = (uint32_t*)(r->rings + 128);
    r->cq_tail = (uint32_t*)(r->rings + 192);
    *(uint32_t*)(r->rings + 256) = sn - 1;
    *(uint32_t*)(r->rings + 260) = cn - 1;
    *(uint32_t*)(r->rings + 264) = sn;
    *(uint32_t*)(r->rings + 268) = cn;
    r->sq_drop = (uint32_t*)(r->rings + 272);
    r->sq_flags = (uint32_t*)(r->rings + 276);
    r->cq_ovf = (uint32_t*)(r->rings + 284);
    r->cqes = (struct io_uring_cqe*)(r->rings + 320);
    r->sq_array = (uint32_t*)(r->rings + arr);
    for (uint32_t i = 0; i < sn; i++) r->sq_array[i] = i;    // identity, user rewrites it anyway

    file_t* f = file_new(F_URING, 2);
    if (!f) goto nomem;
    f->ur = r;
    int fd = sys_tmpfd(f);          // installs another ref
    if (fd < 0) { file_close(f); return fd; }
    file_close(f);                  // drop the tmp ref, the table has its own

    memset(pr, 0, sizeof(*pr));
    pr->sq_entries = sn; pr->cq_entries = cn;
    pr->flags = r->flags;
    pr->features = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP | IORING_FEAT_SUBMIT_STABLE |
                   IORING_FEAT_RW_CUR_POS | IORING_FEAT_CUR_PERSONALITY | IORING_FEAT_FAST_POLL |
                   IORING_FEAT_POLL_32BITS | IORING_FEAT_EXT_ARG | IORING_FEAT_NATIVE_WORKERS;
    pr->sq_off.head = 0; pr->sq_off.tail = 64; pr->sq_off.ring_mask = 256;
    pr->sq_off.ring_entries = 264; pr->sq_off.dropped = 272; pr->sq_off.flags = 276;
    pr->sq_off.array = arr;
    pr->cq_off.head = 128; pr->cq_off.tail = 192; pr->cq_off.ring_mask = 260;
    pr->cq_off.ring_entries = 268; pr->cq_off.overflow = 284; pr->cq_off.cqes = 320;
    pr->cq_off.flags = 280;
    return fd;
nomem:
    if (r->rings_pa) for (uint32_t i = 0; i < r->rings_np; i++) pmm_unref(r->rings_pa + i * PAGE_SIZE);
    if (r->sqes_pa) for (uint32_t i = 0; i < r->sqes_np; i++) pmm_unref(r->sqes_pa + i * PAGE_SIZE);
    kfree(r);
    return -ENOMEM;
}

int uring_mmap(uring_t* r, uint64_t pd, uint64_t addr, uint64_t len, uint64_t off) {
    uint64_t pa;
    uint32_t np;
    if (off == IORING_OFF_SQ_RING || off == IORING_OFF_CQ_RING) { pa = r->rings_pa; np = r->rings_np; }
    else if (off == IORING_OFF_SQES) { pa = r->sqes_pa; np = r->sqes_np; }
    else return -EINVAL;
    if (len > np * PAGE_SIZE) return -EINVAL;
    for (uint32_t i = 0; i < len / PAGE_SIZE; i++)
        if (vmm_map_frame(pd, addr + i * PAGE_SIZE, pa + i * PAGE_SIZE, true) < 0) return -ENOMEM;
    return 0;
}

static void req_start(req_t* q);

static bool opt_fail(req_t* q, int res) {
    int op = q->sqe.opcode;
    if (op == IORING_OP_TIMEOUT && res == -ETIME && (q->sqe.timeout_flags & IORING_TIMEOUT_ETIME_SUCCESS)) return false;
    if (res < 0) return true;
    if (op == IORING_OP_READ || op == IORING_OP_WRITE || op == IORING_OP_READ_FIXED ||
        op == IORING_OP_WRITE_FIXED || op == IORING_OP_SEND || op == IORING_OP_RECV)
        return (uint32_t)res != q->sqe.len;
    return false;
}

// finish a request: cqe, then whatever hangs on it. q is gone after this
static void req_done(req_t* q, int res) {
    uring_t* r = q->r;
    uint32_t fl = irq_save();
    for (req_t** pp = &reqs; *pp; pp = &(*pp)->next)
        if (*pp == q) { *pp = q->next; break; }
    cq_post(r, q->sqe.user_data, res, 0);
    if (q->sqe.opcode == IORING_OP_TIMEOUT && (res == -ETIME || res == 0)) { r->ncq--; r->ntmo++; }      // expiries do not count for other timeouts
    req_t* lt = q->lt;
    req_t* nx = q->link;
    if (q->target) q->target->lt = NULL;
    if (q->sqe.flags & IOSQE_IO_DRAIN) r->ndrain--;
    irq_restore(fl);
    file_close(q->f);
    bool fail = opt_fail(q, res);
    bool hard = (q->sqe.flags & IOSQE_IO_HARDLINK) != 0;
    kfree(q);
    if (lt) { lt->target = NULL; req_done(lt, lt->bad ? lt->bad : -ECANCELED); }
    if (nx) {
        if (fail && !hard) req_done(nx, -ECANCELED);
        else req_start(nx);
    }
}

// take it away from the queue if it did not start running yet
static bool req_cancel(req_t* q, int res) {
    uint32_t fl = irq_save();
    bool ok = q->st == R_WAIT;
    if (ok) q->st = R_RUN;
    irq_restore(fl);
    if (ok) {
        req_done(q, res);
    }
    return ok;
}

static void arm(req_t* q, uint32_t ms) {
    q->armed = true;
    q->deadline = pit_uptime_ms() + ms;
    q->start_ncq = q->r->ncq;
}

static void req_start(req_t* q) {
    if (q->bad) { q->st = R_RUN; req_done(q, q->bad); return; }
    uint32_t fl = irq_save();
    q->st = R_WAIT;
    if (q->sqe.opcode == IORING_OP_TIMEOUT) arm(q, q->ms);
    if (q->lt) { q->lt->st = R_WAIT; arm(q->lt, q->lt->ms); }    // lto counts from now
    irq_restore(fl);
    kick();
}

static int ts_ms(uint64_t uaddr, uint32_t* ms) {
    struct kts* t = (struct kts*)(uintptr_t)uaddr;
    if (!sys_uok(t, sizeof(*t))) return -EFAULT;
    if (t->sec < 0 || t->nsec < 0 || t->nsec >= 1000000000) return -EINVAL;
    uint64_t m = (uint64_t)t->sec * 1000 + (t->nsec + 999999) / 1000000;
    *ms = m > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)m;
    return 0;
}

static file_t* fixed_file(uring_t* r, uint32_t i) {
    return i < r->nfiles ? r->files[i] : NULL;
}

// build a request from an sqe, in the context of the submitter
static req_t* prep(uring_t* r, struct io_uring_sqe* s) {
    req_t* q = kmalloc(sizeof(req_t));
    if (!q) return NULL;
    memset(q, 0, sizeof(*q));
    q->r = r; q->p = proc_current();
    q->sqe = *s;
    q->seq = r->seq++;
    if (s->flags & IOSQE_IO_DRAIN) r->ndrain++;
    int op = s->opcode;
    bool needf = true;
    switch (op) {
        case IORING_OP_NOP: case IORING_OP_TIMEOUT: case IORING_OP_TIMEOUT_REMOVE:
        case IORING_OP_LINK_TIMEOUT: case IORING_OP_ASYNC_CANCEL: case IORING_OP_OPENAT:
        case IORING_OP_STATX: case IORING_OP_POLL_REMOVE:
            needf = false; break;
        case IORING_OP_REMOVE_BUFFERS: needf = false; q->bad = -ENOENT; break;      // nothing is ever provided
        case IORING_OP_CLOSE: needf = false; break;
        case IORING_OP_READV: case IORING_OP_WRITEV: case IORING_OP_READ: case IORING_OP_WRITE:
        case IORING_OP_SYNC_FILE_RANGE:
        case IORING_OP_READ_FIXED: case IORING_OP_WRITE_FIXED: case IORING_OP_FSYNC:
        case IORING_OP_POLL_ADD: case IORING_OP_ACCEPT: case IORING_OP_CONNECT:
        case IORING_OP_SEND: case IORING_OP_RECV: case IORING_OP_SENDMSG: case IORING_OP_RECVMSG:
            break;
        default: q->bad = -EINVAL; needf = false;
    }
    if (op == IORING_OP_ACCEPT && (s->ioprio & 1) && s->file_index && s->file_index != ~0u) q->bad = -EINVAL;
    if ((op == IORING_OP_SENDMSG || op == IORING_OP_RECVMSG) && !q->bad && !sys_uok((void*)(uintptr_t)s->addr, 56)) q->bad = -EFAULT;
    if (needf && !q->bad) {
        if (s->flags & IOSQE_FIXED_FILE) q->f = fixed_file(r, (uint32_t)s->fd);
        else q->f = sys_getf(s->fd);
        if (!q->f) q->bad = -EBADF;
        else file_ref(q->f);
    }
    if (op == IORING_OP_TIMEOUT || op == IORING_OP_LINK_TIMEOUT) {
        uint32_t ms = 0;
        int e = 0;
        if (s->ioprio) e = -EINVAL;
        else if (s->timeout_flags & ~0xAFu) e = -EINVAL;            // abs, update, boottime, realtime, immediate
        else if (s->timeout_flags & 0x80) ms = (uint32_t)((s->addr + 999999) / 1000000);    // immediate arg: ns in addr
        else e = ts_ms(s->addr, &ms);
        if (e < 0) q->bad = e;
        else {
            if (s->timeout_flags & IORING_TIMEOUT_ABS) {
                uint32_t now = pit_uptime_ms();
                ms = ms > now ? ms - now : 0;
            }
            q->ms = ms;
            q->count = (uint32_t)s->off;
        }
    }
    return q;
}

// rw on a plain file: offset in the sqe, -1 is the current position
static int do_rw(req_t* q, char* buf, uint32_t n, bool wr) {
    file_t* f = q->f;
    int acc = f->flags & 3;
    if (wr ? (acc == 0 && f->type == F_NODE) : acc == 1) return -EBADF;
    if (!n) return 0;
    if (!sys_uok(buf, n)) return -EFAULT;
    if (wr && f->type == F_PIPE_W && f->pipe->readers <= 0) return -32;     // EPIPE, no SIGPIPE from here
    uint64_t off = q->sqe.off;
    if (off == ~0ull || f->type != F_NODE) return wr ? file_write(f, buf, n) : file_read(f, buf, n);
    uint64_t save = f->off;
    f->off = off;
    int ret = wr ? file_write(f, buf, n) : file_read(f, buf, n);
    f->off = save;
    return ret;
}

typedef struct { uint64_t base, len; } uiov_t;

static int do_rwv(req_t* q, bool wr) {
    uiov_t* iov = (uiov_t*)(uintptr_t)q->sqe.addr;
    uint32_t cnt = q->sqe.len;
    if (cnt > 1024) return -EINVAL;
    if (!sys_uok(iov, cnt * sizeof(*iov))) return -EFAULT;
    uint64_t off = q->sqe.off;
    int total = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (!iov[i].len) continue;
        int r = do_rw(q, (char*)iov[i].base, iov[i].len, wr);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].len) break;
        if (off != ~0ull) q->sqe.off += iov[i].len;      // next vector continues after this one
    }
    return total;
}

static int do_sock(req_t* q, int call) {
    struct io_uring_sqe* s = &q->sqe;
    file_t* f = q->f;
    int fd = s->fd, tmp = 0;
    if ((s->flags & IOSQE_FIXED_FILE) || sys_getf(fd) != f) {
        fd = sys_tmpfd(f);
        if (fd < 0) return fd;
        tmp = 1;
    }
    int ret;
    uint64_t a = s->addr;
    switch (call) {
        case 3: ret = sys_sock(3, fd, a, s->off, 0, 0, 0); break;                         // connect
        case 18: ret = sys_sock(18, fd, a, s->off, s->accept_flags, 0, 0); break;         // accept4, addrlen ptr in off
        case 11: ret = sys_sock(11, fd, a, s->len, s->msg_flags, 0, 0); break;
        case 12: ret = sys_sock(12, fd, a, s->len, s->msg_flags, 0, 0); break;
        default: ret = sys_sock(call, fd, a, s->msg_flags, 0, 0, 0);                      // msg
    }
    if (tmp) sys_untmpfd(fd);
    return ret;
}

// move a fresh fd into the registered table (direct descriptors). ~0 = any free slot
static int to_fixed(uring_t* r, int fd, uint32_t idx) {
    if (fd < 0) return fd;
    if (!r->files) { sys_close(fd); return -EBADF; }
    int ret = 0;
    if (idx == ~0u) {
        idx = 0;
        while (idx < r->nfiles && r->files[idx]) idx++;
        ret = idx;
    } else idx--;
    if (idx >= r->nfiles) { sys_close(fd); return -EBADF; }
    file_t* f = sys_getf(fd);
    file_ref(f);
    sys_close(fd);
    file_close(r->files[idx]);
    r->files[idx] = f;
    return ret;
}

static req_t* find_ud(uring_t* r, uint64_t ud, int op_only) {
    for (req_t* q = reqs; q; q = q->next)
        if (q->r == r && q->sqe.user_data == ud && (op_only < 0 || q->sqe.opcode == op_only)) return q;
    return NULL;
}

static int do_cancel(req_t* me, uint64_t ud, uint32_t fl, int fd) {
    uring_t* r = me->r;
    int n = 0, busy = 0;
    for (;;) {
        req_t* hit = NULL;
        uint32_t f = irq_save();
        for (req_t* q = reqs; q && !hit; q = q->next) {
            if (q->r != r || q == me || q->st == R_HELD) continue;
            if (!(fl & IORING_ASYNC_CANCEL_ANY)) {
                if (fl & IORING_ASYNC_CANCEL_FD) { if (q->sqe.fd != fd) continue; }
                else if (q->sqe.user_data != ud) continue;
            }
            if (q->st == R_RUN) { busy = 1; continue; }
            hit = q;
        }
        irq_restore(f);
        if (!hit) break;
        if (req_cancel(hit, -ECANCELED)) n++;
        if (!(fl & IORING_ASYNC_CANCEL_ALL) && !(fl & IORING_ASYNC_CANCEL_ANY)) break;
    }
    if (n) return (fl & IORING_ASYNC_CANCEL_ALL) ? n : 0;
    return busy ? -EALREADY : -ENOENT;
}

static int do_probe(void* arg, uint32_t nr) {
    struct io_uring_probe* p = arg;
    static const uint8_t ok[] = { IORING_OP_NOP, IORING_OP_READV, IORING_OP_WRITEV, IORING_OP_FSYNC, IORING_OP_SYNC_FILE_RANGE,
        IORING_OP_READ_FIXED, IORING_OP_WRITE_FIXED, IORING_OP_POLL_ADD, IORING_OP_POLL_REMOVE,
        IORING_OP_SENDMSG, IORING_OP_RECVMSG, IORING_OP_TIMEOUT, IORING_OP_TIMEOUT_REMOVE,
        IORING_OP_ACCEPT, IORING_OP_ASYNC_CANCEL, IORING_OP_LINK_TIMEOUT, IORING_OP_CONNECT,
        IORING_OP_OPENAT, IORING_OP_CLOSE, IORING_OP_STATX, IORING_OP_READ, IORING_OP_WRITE,
        IORING_OP_SEND, IORING_OP_RECV };
    if (!sys_uok(p, sizeof(*p) + nr * sizeof(p->ops[0]))) return -EFAULT;
    if (p->resv || p->resv2[0] || p->resv2[1] || p->resv2[2]) return -EINVAL;
    memset(p, 0, sizeof(*p) + nr * sizeof(p->ops[0]));
    p->last_op = IORING_OP_RECV;
    p->ops_len = nr > IORING_OP_RECV + 1 ? IORING_OP_RECV + 1 : nr;
    for (uint32_t i = 0; i < sizeof(ok); i++)
        if (ok[i] < p->ops_len) p->ops[ok[i]].flags = IO_URING_OP_SUPPORTED;
    return 0;
}

static int req_exec(req_t* q) {
    struct io_uring_sqe* s = &q->sqe;
    uring_t* r = q->r;
    switch (s->opcode) {
        case IORING_OP_NOP: return (s->rw_flags & 1) ? (int)s->len : 0;       // INJECT_RESULT
        case IORING_OP_READ: case IORING_OP_WRITE:
            return do_rw(q, (char*)(uintptr_t)s->addr, s->len, s->opcode == IORING_OP_WRITE);
        case IORING_OP_READV: case IORING_OP_WRITEV: return do_rwv(q, s->opcode == IORING_OP_WRITEV);
        case IORING_OP_READ_FIXED: case IORING_OP_WRITE_FIXED: {
            uint64_t a = s->addr;
            uint32_t i = s->buf_index;
            if (i >= r->nbufs || a < r->bufs[i].base || s->len > r->bufs[i].len ||
                a - r->bufs[i].base > r->bufs[i].len - s->len) return -EFAULT;
            return do_rw(q, (char*)(uintptr_t)a, s->len, s->opcode == IORING_OP_WRITE_FIXED);
        }
        case IORING_OP_FSYNC: case IORING_OP_SYNC_FILE_RANGE: ext2_sync_all(); fatfs_sync_all(); return 0;
        case IORING_OP_SEND: return do_sock(q, 11);
        case IORING_OP_RECV: return do_sock(q, 12);
        case IORING_OP_SENDMSG: return do_sock(q, 16);
        case IORING_OP_RECVMSG: return do_sock(q, 17);
        case IORING_OP_ACCEPT:
            if (s->file_index) return to_fixed(r, do_sock(q, 18), s->file_index);
            return do_sock(q, 18);
        case IORING_OP_CONNECT: return do_sock(q, 3);
        case IORING_OP_OPENAT: {
            if (s->file_index && (s->open_flags & 02000000)) return -EINVAL;     // cloexec makes no sense for a direct descriptor
            int fd = sys_openat(s->fd, (const char*)(uintptr_t)s->addr, s->open_flags, s->len);
            return s->file_index ? to_fixed(r, fd, s->file_index) : fd;
        }
        case IORING_OP_STATX:
            return sys_statx(s->fd, (const char*)(uintptr_t)s->addr, s->statx_flags, s->len, (void*)(uintptr_t)s->off);
        case IORING_OP_CLOSE:
            if (s->file_index) {
                uint32_t i = s->file_index - 1;
                if (s->fd) return -EINVAL;
                if (!r->files) return -ENXIO;
                if (i >= r->nfiles) return -EINVAL;
                if (!r->files[i]) return -EBADF;
                file_close(r->files[i]);
                r->files[i] = NULL;
                return 0;
            }
            {
                file_t* cf = sys_getf(s->fd);
                if (cf && cf->type == F_URING && cf->ur == r) return -EBADF;      // closing our own ring would wait for us forever
            }
            return sys_close(s->fd);
        case IORING_OP_ASYNC_CANCEL: return do_cancel(q, s->addr, s->cancel_flags, s->fd);
        case IORING_OP_POLL_REMOVE: {
            req_t* t = find_ud(r, s->addr, IORING_OP_POLL_ADD);
            if (!t) return -ENOENT;
            return req_cancel(t, -ECANCELED) ? 0 : -EALREADY;
        }
        case IORING_OP_TIMEOUT_REMOVE: {
            uint32_t tf = s->timeout_flags;
            if ((tf & ~0x1Fu) || (tf && !(tf & 0x12))) return -EINVAL;
            req_t* t = find_ud(r, s->addr, (tf & IORING_LINK_TIMEOUT_UPDATE) ? IORING_OP_LINK_TIMEOUT : IORING_OP_TIMEOUT);
            if (!t) return -ENOENT;
            if (tf & (IORING_TIMEOUT_UPDATE | IORING_LINK_TIMEOUT_UPDATE)) {
                uint32_t ms;
                int e = ts_ms(s->addr2, &ms);
                if (e < 0) return e;
                if (s->timeout_flags & IORING_TIMEOUT_ABS) {
                    uint32_t now = pit_uptime_ms();
                    ms = ms > now ? ms - now : 0;
                }
                t->deadline = pit_uptime_ms() + ms;
                return 0;
            }
            return req_cancel(t, -ECANCELED) ? 0 : -EALREADY;
        }
    }
    return -EINVAL;
}

// a drain request waits for everything before it, everything after waits for it
static bool drain_blocked(req_t* q) {
    bool d = (q->sqe.flags & IOSQE_IO_DRAIN) != 0;
    if (!d && !q->r->ndrain) return false;
    for (req_t* x = reqs; x; x = x->next) {
        if (x->r != q->r || x->seq >= q->seq) continue;
        if (d || (x->sqe.flags & IOSQE_IO_DRAIN)) return true;
    }
    return false;
}

// next request that can go now, marked R_RUN. only: inline mode from enter
static req_t* pick(proc_t* only) {
    uint32_t fl = irq_save();
    uint32_t now = pit_uptime_ms();
    req_t* q;
    for (q = reqs; q; q = q->next) {
        if (q->st != R_WAIT || q->r->dead || drain_blocked(q)) continue;
        int op = q->sqe.opcode, need = 0;
        if (op == IORING_OP_TIMEOUT || op == IORING_OP_LINK_TIMEOUT) {
            bool cnt = op == IORING_OP_TIMEOUT && q->count && q->r->ncq - q->start_ncq >= q->count;
            if (cnt) q->res = 0;
            else if ((int32_t)(now - q->deadline) >= 0) q->res = -ETIME;
            else continue;
            q->pre = true;
            break;
        }
        if (op == IORING_OP_POLL_ADD) {
            int16_t want = (int16_t)q->sqe.poll_events;
            int16_t rv = sys_revents(q->f, want);
            if (q->sqe.len & IORING_POLL_ADD_MULTI) {
                int16_t m = rv & (want | 0x18);
                uint32_t g = file_gen(q->f);
                if (m && ((m & ~q->last) || g != q->gen)) cq_post(q->r, q->sqe.user_data, m, IORING_CQE_F_MORE);
                q->last = m;
                q->gen = g;
                continue;
            }
            if (!rv) continue;
            q->res = rv & (want | 0x18);
            q->pre = true;
            break;
        }
        if (op == IORING_OP_READ || op == IORING_OP_READV || op == IORING_OP_READ_FIXED ||
            op == IORING_OP_RECV || op == IORING_OP_RECVMSG || op == IORING_OP_ACCEPT) need = 1;
        if (op == IORING_OP_WRITE || op == IORING_OP_WRITEV || op == IORING_OP_WRITE_FIXED ||
            op == IORING_OP_SEND || op == IORING_OP_SENDMSG) need = 4;
        if (need && q->f && !sys_revents(q->f, need)) continue;
        if (only && (q->p != only || (q->sqe.flags & IOSQE_ASYNC) || op == IORING_OP_CONNECT)) continue;
        break;
    }
    if (q) { q->st = R_RUN; q->r->running++; }
    irq_restore(fl);
    return q;
}

static void run(req_t* q) {
    uring_t* r = q->r;
    int res;
    if (q->pre) {
        if (q->sqe.opcode == IORING_OP_LINK_TIMEOUT && q->target) {
            req_t* tg = q->target;
            q->target = NULL;
            tg->lt = NULL;
            req_cancel(tg, -ECANCELED);
        }
        res = q->res;
    } else {
        proc_t* p = q->p;
        task_t* t = task_current();
        proc_t* old = t->proc;
        if (old != p) { t->proc = p; task_set_cr3(p->pd); }
        res = req_exec(q);
        if (old != p) { t->proc = old; task_set_cr3(old ? old->pd : 0); }
    }
    if (!q->pre && q->sqe.opcode == IORING_OP_ACCEPT && (q->sqe.ioprio & 1) && res >= 0) {
        uint32_t fl = irq_save();               // multishot accept: report it and wait for the next one
        cq_post(r, q->sqe.user_data, res, IORING_CQE_F_MORE);
        q->st = R_WAIT;
        irq_restore(fl);
        r->running--;
        return;
    }
    req_done(q, res);
    r->running--;
}

static int run_all(proc_t* only) {
    int n = 0;
    req_t* q;
    while ((q = pick(only))) { run(q); n++; }
    return n;
}

static void uw(void) {
    for (;;) {
        if (run_all(NULL)) continue;
        uint32_t fl = irq_save();
        bool wait = false;
        for (req_t* q = reqs; q; q = q->next) if (q->st == R_WAIT) wait = true;
        task_t* t = task_current();
        t->wake_ms = wait ? pit_uptime_ms() + 1 : 0;
        t->state = T_BLOCKED;
        while (t->state == T_BLOCKED) task_yield();
        irq_restore(fl);
    }
}

void uring_init(void) {
    for (int i = 0; i < NWORK; i++) workers[i] = task_spawn_sz("uring", uw, 32768);
}

static int sq_submit(uring_t* r, uint32_t n) {
    uint32_t head = *r->sq_head, tail = *r->sq_tail;
    if (n > tail - head) n = tail - head;
    req_t *heads = NULL, *hl = NULL, *tl = NULL;
    bool open = false;
    int done = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = r->sq_array[head++ & r->sq_mask];
        if (idx >= r->sq_n) { (*r->sq_drop)++; continue; }
        struct io_uring_sqe s = r->sq[idx];
        req_t* q = prep(r, &s);
        if (!q) {
            uint32_t fl = irq_save();
            cq_post(r, s.user_data, -ENOMEM, 0);
            irq_restore(fl);
            continue;
        }
        done++;
        q->st = R_HELD;
        uint32_t fl = irq_save();
        req_t** pp = &reqs;
        while (*pp) pp = &(*pp)->next;
        *pp = q;
        irq_restore(fl);
        bool lnk = (s.flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK)) != 0;
        if (s.opcode == IORING_OP_LINK_TIMEOUT && open && tl && !tl->lt) {
            if (q->bad && !tl->bad) tl->bad = -ECANCELED;     // broken lto kills what it hangs on
            tl->lt = q;
            q->target = tl;
            open = lnk;
            continue;
        }
        if (s.opcode == IORING_OP_LINK_TIMEOUT && !q->bad) q->bad = -EINVAL;
        if (open && tl) tl->link = q;
        else { if (hl) hl->snext = q; else heads = q; hl = q; }
        tl = q;
        open = lnk;
        if (q->bad && q->bad != -ENOENT && !lnk && s.opcode != IORING_OP_LINK_TIMEOUT && !(r->flags & IORING_SETUP_SUBMIT_ALL)) break;      // stop at the first broken sqe
    }
    *r->sq_head = head;
    bar();
    while (heads) {
        req_t* nx = heads->snext;
        req_start(heads);
        heads = nx;
    }
    return done;
}

// drop what is queued for a ring or a process, wait for what is already running
static void purge(uring_t* r, proc_t* p) {
    req_t* dead = NULL;
    uint32_t fl = irq_save();
    for (req_t** pp = &reqs; *pp;) {
        req_t* q = *pp;
        if (!((r && q->r == r) || (p && q->p == p))) { pp = &q->next; continue; }
        if (q->st == R_RUN) { q->link = NULL; q->lt = NULL; q->target = NULL; pp = &q->next; continue; }
        *pp = q->next;
        if (q->sqe.flags & IOSQE_IO_DRAIN) q->r->ndrain--;
        q->next = dead;
        dead = q;
    }
    irq_restore(fl);
    while (dead) {
        req_t* nx = dead->next;
        file_close(dead->f);
        kfree(dead);
        dead = nx;
    }
    for (;;) {
        bool busy = false;
        fl = irq_save();
        if (r) busy = r->running > 0;
        for (req_t* q = reqs; q && !busy; q = q->next) if (p && q->p == p && q->st == R_RUN) busy = true;
        irq_restore(fl);
        if (!busy) break;
        task_sleep_ms(1);
    }
}

void uring_exit(proc_t* p) {
    // timeouts live on without the submitter, the rest gets canceled
    for (;;) {
        req_t* hit = NULL;
        uint32_t fl = irq_save();
        for (req_t* q = reqs; q; q = q->next) {
            if (q->p != p || q->r->dead || q->st != R_WAIT) continue;
            if (q->sqe.opcode == IORING_OP_TIMEOUT && !q->link) { q->p = NULL; continue; }
            hit = q;
            break;
        }
        irq_restore(fl);
        if (!hit) break;
        req_cancel(hit, -ECANCELED);
    }
    purge(NULL, p);
}

static void drop_files(uring_t* r) {
    for (uint32_t i = 0; i < r->nfiles; i++) file_close(r->files[i]);
    kfree(r->files);
    r->files = NULL; r->nfiles = 0;
}

void uring_release(uring_t* r) {
    r->dead = true;
    purge(r, NULL);
    drop_files(r);
    kfree(r->bufs);
    file_close(r->efd);
    while (r->ovf) { ovf_t* o = r->ovf; r->ovf = o->next; kfree(o); }
    for (uint32_t i = 0; i < r->rings_np; i++) pmm_unref(r->rings_pa + i * PAGE_SIZE);
    for (uint32_t i = 0; i < r->sqes_np; i++) pmm_unref(r->sqes_pa + i * PAGE_SIZE);
    kfree(r);
}

int uring_enter(int fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags, const void* arg, size_t argsz) {
    file_t* f = sys_getf(fd);
    if (!f) return -EBADF;
    if (f->type != F_URING) return -EOPNOTSUPP;
    if (flags & ~0x1Fu) return -EINVAL;
    uring_t* r = f->ur;
    uint32_t tmo = 0;
    bool has_tmo = false;
    if ((flags & IORING_ENTER_GETEVENTS) && (flags & IORING_ENTER_EXT_ARG)) {
        const struct io_uring_getevents_arg* a = arg;
        if (argsz != sizeof(*a)) return -EINVAL;
        if (!sys_uok(a, sizeof(*a))) return -EFAULT;
        if (a->ts) {
            int e = ts_ms(a->ts, &tmo);
            if (e < 0) return e;
            has_tmo = true;
        }
    }
    int sub = 0;
    if (to_submit) sub = sq_submit(r, to_submit);
    run_all(proc_current());
    if (!(flags & IORING_ENTER_GETEVENTS)) return sub;

    uint32_t start = pit_uptime_ms();
    proc_t* me = proc_current();
    int err = 0;
    uint32_t tm0 = r->ntmo;
    for (;;) {
        uint32_t fl = irq_save();
        cq_flush(r);
        irq_restore(fl);
        if (*r->cq_tail - *r->cq_head >= min_complete) break;
        run_all(me);
        if (*r->cq_tail - *r->cq_head >= min_complete || r->ntmo != tm0) break;
        uint32_t el = pit_uptime_ms() - start;
        if (has_tmo && el >= tmo) { err = -ETIME; break; }
        if (proc_interrupted() || proc_signal_deliverable(me)) { err = -EINTR; break; }
        fl = irq_save();
        if (*r->cq_tail - *r->cq_head < min_complete && r->ntmo == tm0) {
            task_t* t = task_current();
            uint32_t w = has_tmo && tmo - el < 20 ? tmo - el : 20;
            r->waiter = t->id;
            t->wake_ms = pit_uptime_ms() + (w ? w : 1);
            t->state = T_BLOCKED;
            while (t->state == T_BLOCKED) task_yield();
            r->waiter = -1;
        }
        irq_restore(fl);
    }
    if (err && *r->cq_tail != *r->cq_head) err = 0;       // linux does that: a timeout with something to read is fine
    return sub ? sub : err;
}

static int reg_files(uring_t* r, const int32_t* fds, uint32_t nr, bool sparse) {
    if (r->files) return -EBUSY;
    if (!nr || nr > 65536) return -EINVAL;
    if (!sparse && !sys_uok(fds, nr * 4)) return -EFAULT;
    r->files = kmalloc(nr * sizeof(file_t*));
    if (!r->files) return -ENOMEM;
    memset(r->files, 0, nr * sizeof(file_t*));
    r->nfiles = nr;
    for (uint32_t i = 0; !sparse && i < nr; i++) {
        if (fds[i] == -1) continue;
        file_t* f = sys_getf(fds[i]);
        if (!f) { drop_files(r); return -EBADF; }
        file_ref(f);
        r->files[i] = f;
    }
    return 0;
}

static int upd_files(uring_t* r, uint32_t off, const int32_t* fds, uint32_t nr) {
    if (!r->files) return -ENXIO;
    if (off > r->nfiles || nr > r->nfiles - off) return -EINVAL;
    if (!sys_uok(fds, nr * 4)) return -EFAULT;
    for (uint32_t i = 0; i < nr; i++) {
        if (fds[i] == -2) continue;
        file_t* f = NULL;
        if (fds[i] != -1) {
            f = sys_getf(fds[i]);
            if (!f) return i ? (int)i : -EBADF;
            file_ref(f);
        }
        file_close(r->files[off + i]);
        r->files[off + i] = f;
    }
    return nr;
}

static int reg_bufs(uring_t* r, const uiov_t* iov, uint32_t nr) {
    if (r->bufs) return -EBUSY;
    if (!nr || nr > 16384) return -EINVAL;
    if (!sys_uok(iov, nr * sizeof(*iov))) return -EFAULT;
    for (uint32_t i = 0; i < nr; i++) {
        if (!iov[i].len || !sys_uok((void*)iov[i].base, iov[i].len)) return -EFAULT;
    }
    r->bufs = kmalloc(nr * sizeof(*r->bufs));
    if (!r->bufs) return -ENOMEM;
    for (uint32_t i = 0; i < nr; i++) { r->bufs[i].base = iov[i].base; r->bufs[i].len = iov[i].len; }
    r->nbufs = nr;
    return 0;
}

int uring_register(int fd, uint32_t op, void* arg, uint32_t nr) {
    file_t* f = sys_getf(fd);
    if (!f) return -EBADF;
    if (f->type != F_URING) return -EOPNOTSUPP;
    uring_t* r = f->ur;
    op &= 0x7FFFFFFF;
    switch (op) {
        case IORING_REGISTER_BUFFERS: return reg_bufs(r, arg, nr);
        case IORING_REGISTER_BUFFERS2: {
            struct io_uring_rsrc_register* g = arg;
            if (!sys_uok(g, sizeof(*g)) || nr != sizeof(*g)) return -EINVAL;
            if (!g->data) return -EINVAL;           // sparse buffers: no
            return reg_bufs(r, (void*)(uintptr_t)g->data, g->nr);
        }
        case IORING_UNREGISTER_BUFFERS:
            if (!r->bufs) return -ENXIO;
            kfree(r->bufs); r->bufs = NULL; r->nbufs = 0;
            return 0;
        case IORING_REGISTER_FILES: return reg_files(r, arg, nr, false);
        case IORING_REGISTER_FILES2: {
            struct io_uring_rsrc_register* g = arg;
            if (!sys_uok(g, sizeof(*g)) || nr != sizeof(*g)) return -EINVAL;
            return reg_files(r, (void*)(uintptr_t)g->data, g->nr, !g->data);
        }
        case IORING_UNREGISTER_FILES:
            if (!r->files) return -ENXIO;
            drop_files(r);
            return 0;
        case IORING_REGISTER_FILES_UPDATE: {
            struct io_uring_files_update* u = arg;
            if (!sys_uok(u, sizeof(*u))) return -EFAULT;
            return upd_files(r, u->offset, (void*)(uintptr_t)u->fds, nr);
        }
        case IORING_REGISTER_FILES_UPDATE2: {
            struct io_uring_rsrc_update2* u = arg;
            if (!sys_uok(u, sizeof(*u))) return -EFAULT;
            return upd_files(r, u->offset, (void*)(uintptr_t)u->data, u->nr);
        }
        case IORING_REGISTER_EVENTFD: case IORING_REGISTER_EVENTFD_ASYNC: {
            if (r->efd) return -EBUSY;
            if (!sys_uok(arg, 4)) return -EFAULT;
            file_t* e = sys_getf(*(int*)arg);
            if (!e) return -EBADF;
            if (e->type != F_EVENTFD) return -EINVAL;
            file_ref(e);
            r->efd = e;
            return 0;
        }
        case IORING_UNREGISTER_EVENTFD:
            if (!r->efd) return -ENXIO;
            file_close(r->efd); r->efd = NULL;
            return 0;
        case IORING_REGISTER_PROBE: return do_probe(arg, nr);
    }
    return -EINVAL;
}
