#include "proc/pty.h"
#include "proc/proc.h"
#include "proc/tty.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"
#include "fs/fs.h"
#include "boot/pit.h"

#define EIO      5
#define ENXIO    6
#define EAGAIN  11
#define EFAULT  14
#define EBUSY   16
#define EINVAL  22
#define ENOTTY  25
#define EINTR    4
#define ENOSPC  28

#define PTY_BUF 4096

typedef struct { char b[PTY_BUF]; int h, t, n; } ring_t;

typedef struct {
    bool       used, locked;
    int        mrefs, srefs;       /* open file descriptions of each side */
    bool       slave_seen;         /* the slave was opened at least once */
    ring_t     in;                 /* cooked input, read by the slave */
    ring_t     out;                /* slave output, read by the master */
    char       line[256];          /* canonical line being edited */
    int        llen;
    int        eof;                /* ^D on an empty line: reads that return 0 */
    ktermios_t tio;
    uint16_t   rows, cols;
    int        pgrp, sid;          /* foreground job / session of the slave */
    fs_node_t* node;               /* /dev/pts/N */
} pty_t;

static pty_t ptys[NPTY];

/* termios bits beyond the ones tty.h names */
#define I_INLCR  0x0040
#define I_IGNCR  0x0080
#define L_ECHOK  0x0020
#define L_ECHOCTL 0x0200
#define L_IEXTEN 0x8000

static bool uptr(uint32_t a, uint32_t len) {
    return a >= USER_BASE && a < USER_TOP && len <= USER_TOP - a;
}

/* ---------------- rings ---------------- */

static int  ring_space(const ring_t* r) { return PTY_BUF - r->n; }
static void ring_put(ring_t* r, char c) {
    if (r->n == PTY_BUF) return;
    r->b[r->h] = c; r->h = (r->h + 1) % PTY_BUF; r->n++;
}
static char ring_get(ring_t* r) {
    char c = r->b[r->t]; r->t = (r->t + 1) % PTY_BUF; r->n--; return c;
}

/* ---------------- line discipline ---------------- */

static void out_char(pty_t* p, char c) {                 /* OPOST */
    if ((p->tio.c_oflag & TTY_OPOST) && (p->tio.c_oflag & TTY_ONLCR) && c == '\n')
        ring_put(&p->out, '\r');
    ring_put(&p->out, c);
}

static void echo_char(pty_t* p, char c) {
    uint8_t u = (uint8_t)c;
    if ((p->tio.c_lflag & L_ECHOCTL) && u < 0x20 && c != '\n' && c != '\t' && c != '\r') {
        out_char(p, '^');
        out_char(p, (char)(u + '@'));
        return;
    }
    out_char(p, c);
}

static void signal_fg(pty_t* p, int sig) {
    if (p->pgrp) proc_signal_group(p->pgrp, sig);
}

static void flush_line(pty_t* p) {                       /* canonical line -> reader */
    for (int k = 0; k < p->llen; k++) ring_put(&p->in, p->line[k]);
    p->llen = 0;
}

static void input_char(pty_t* p, char c) {
    const ktermios_t* t = &p->tio;
    if (c == '\r' && (t->c_iflag & I_IGNCR)) return;
    if (c == '\r' && (t->c_iflag & TTY_ICRNL)) c = '\n';
    else if (c == '\n' && (t->c_iflag & I_INLCR)) c = '\r';

    if (t->c_lflag & TTY_ISIG) {
        int sig = c == (char)t->c_cc[VINTR] ? 2 : c == (char)t->c_cc[VQUIT] ? 3 :
                  c == 0x1A ? 20 : 0;                    /* ^C, ^\, ^Z */
        if (sig && c) {
            if (t->c_lflag & TTY_ECHO) { echo_char(p, c); out_char(p, '\n'); }
            p->llen = 0;
            if (sig != 20) signal_fg(p, sig);
            return;
        }
    }

    if (!(t->c_lflag & TTY_ICANON)) {
        ring_put(&p->in, c);
        if (t->c_lflag & TTY_ECHO) echo_char(p, c);
        return;
    }

    if (c == (char)t->c_cc[VERASE] || c == 0x08) {
        if (p->llen > 0) {
            p->llen--;
            if (t->c_lflag & TTY_ECHO) { out_char(p, '\b'); out_char(p, ' '); out_char(p, '\b'); }
        }
        return;
    }
    if (c == (char)t->c_cc[VKILL]) {
        while (p->llen > 0) {
            p->llen--;
            if (t->c_lflag & TTY_ECHO) { out_char(p, '\b'); out_char(p, ' '); out_char(p, '\b'); }
        }
        return;
    }
    if (c == (char)t->c_cc[VEOF]) {
        if (p->llen) flush_line(p);                      /* hand over without a newline */
        else p->eof++;
        return;
    }
    if (c == '\n') {
        if (p->llen < (int)sizeof(p->line)) p->line[p->llen++] = c;
        if (t->c_lflag & TTY_ECHO) out_char(p, '\n');
        flush_line(p);
        return;
    }
    if (p->llen < (int)sizeof(p->line) - 1) {
        p->line[p->llen++] = c;
        if (t->c_lflag & TTY_ECHO) echo_char(p, c);
    }
}

/* ---------------- lifetime ---------------- */

static pty_t* get(int i) { return (i >= 0 && i < NPTY && ptys[i].used) ? &ptys[i] : NULL; }

static void set_default_termios(ktermios_t* t) {
    memset(t, 0, sizeof(*t));
    t->c_iflag = TTY_ICRNL;
    t->c_oflag = TTY_OPOST | TTY_ONLCR;
    t->c_cflag = 0x04BF;                                 /* B38400 CS8 CREAD HUPCL */
    t->c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | L_ECHOK | L_ECHOCTL | L_IEXTEN;
    t->c_cc[VINTR] = 3; t->c_cc[VQUIT] = 0x1C; t->c_cc[VERASE] = 0x7F;
    t->c_cc[VKILL] = 0x15; t->c_cc[VEOF] = 4; t->c_cc[VTIME] = 0; t->c_cc[VMIN] = 1;
}

static void node_name(int i, char* out) {
    strcpy(out, "/dev/pts/");
    char d[4]; int n = 0;
    do { d[n++] = (char)('0' + i % 10); i /= 10; } while (i);
    int k = (int)strlen(out);
    while (n) out[k++] = d[--n];
    out[k] = 0;
}

static void release(pty_t* p, int i) {
    if (p->mrefs > 0 || p->srefs > 0) return;
    char path[24];
    node_name(i, path);
    fs_unlink(fs_root(), path);
    memset(p, 0, sizeof(*p));
}

int pty_alloc(void) {
    uint32_t f = irq_save();
    int i = 0;
    while (i < NPTY && ptys[i].used) i++;
    if (i == NPTY) { irq_restore(f); return -ENOSPC; }
    pty_t* p = &ptys[i];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->locked = true;                                    /* until unlockpt() */
    p->mrefs = 1;
    p->rows = 25; p->cols = 80;
    set_default_termios(&p->tio);
    irq_restore(f);

    fs_node_t* dir = fs_resolve(fs_root(), "/dev/pts");
    if (!dir) dir = fs_create(fs_root(), "/dev/pts", FS_DIR);
    char path[24];
    node_name(i, path);
    fs_node_t* n = fs_create(fs_root(), path, FS_FILE);
    if (n) { n->dev = (uint8_t)(FS_DEV_PTS + i); n->mode = 0620; }
    p->node = n;
    return i;
}

void pty_master_close(int i) {
    pty_t* p = get(i);
    if (!p) return;
    if (--p->mrefs <= 0) {
        p->mrefs = 0;
        signal_fg(p, 1);                                 /* SIGHUP: the line is gone */
        if (p->sid) proc_signal_group(p->sid, 1);
    }
    release(p, i);
}

int pty_slave_open(int i, int flags) {
    pty_t* p = get(i);
    if (!p || p->mrefs <= 0) return -ENXIO;
    if (p->locked) return -EIO;
    p->srefs++;
    p->slave_seen = true;
    /* A session leader without a terminal gets this one (unless O_NOCTTY). */
    proc_t* me = proc_current();
    if (me && me->pid == me->sid && me->ctty < 0 && !(flags & 0x100) && !p->sid) {
        me->ctty = i + 1;
        p->sid = me->sid;
        p->pgrp = me->pgid;
    }
    return 0;
}

void pty_slave_close(int i) {
    pty_t* p = get(i);
    if (!p) return;
    if (--p->srefs < 0) p->srefs = 0;
    if (p->srefs == 0) { p->sid = 0; p->pgrp = 0; }
    release(p, i);
}

/* ---------------- data ---------------- */

static bool slave_input_ready(pty_t* p) {
    if (p->mrefs <= 0) return true;                      /* hangup: EOF */
    if (p->in.n > 0) return true;
    return (p->tio.c_lflag & TTY_ICANON) && p->eof > 0;
}

bool pty_readable(int i, bool master) {
    pty_t* p = get(i);
    if (!p) return true;
    if (master) return p->out.n > 0 || (p->slave_seen && p->srefs <= 0);
    return slave_input_ready(p);
}

bool pty_writable(int i, bool master) {
    pty_t* p = get(i);
    if (!p) return true;
    return master ? ring_space(&p->in) > 64 : ring_space(&p->out) > 0;
}

int pty_pending(int i, bool master) {
    pty_t* p = get(i);
    return p ? (master ? p->out.n : p->in.n) : 0;
}

int pty_read(int i, bool master, char* buf, int n, bool nonblock) {
    pty_t* p = get(i);
    if (!p) return -EIO;
    if (n <= 0) return 0;
    if (master) {
        for (;;) {
            uint32_t f = irq_save();
            if (p->out.n > 0) {
                int k = 0;
                while (k < n && p->out.n > 0) buf[k++] = ring_get(&p->out);
                irq_restore(f);
                return k;
            }
            irq_restore(f);
            if (p->slave_seen && p->srefs <= 0) return -EIO;   /* shell gone */
            if (nonblock) return -EAGAIN;
            if (proc_interrupted()) return -EINTR;
            task_yield();
        }
    }
    /* slave */
    uint32_t start = pit_uptime_ms();
    for (;;) {
        uint32_t f = irq_save();
        bool canon = p->tio.c_lflag & TTY_ICANON;
        if (p->in.n > 0) {
            int k = 0;
            while (k < n && p->in.n > 0) {
                char c = ring_get(&p->in);
                buf[k++] = c;
                if (canon && c == '\n') break;           /* one line per read */
            }
            irq_restore(f);
            return k;
        }
        if (canon && p->eof > 0) { p->eof--; irq_restore(f); return 0; }
        irq_restore(f);
        if (p->mrefs <= 0) return 0;                     /* hung up */
        if (nonblock) return -EAGAIN;
        if (!canon && p->tio.c_cc[VMIN] == 0) {          /* VMIN=0: poll / VTIME timeout */
            uint32_t tmo = (uint32_t)p->tio.c_cc[VTIME] * 100;
            if (pit_uptime_ms() - start >= tmo) return 0;
        }
        if (proc_interrupted()) return -EINTR;
        task_yield();
    }
}

int pty_write(int i, bool master, const char* buf, int n, bool nonblock) {
    pty_t* p = get(i);
    if (!p) return -EIO;
    int done = 0;
    while (done < n) {
        uint32_t f = irq_save();
        if (master) {
            if (p->slave_seen && p->srefs <= 0) { irq_restore(f); return done ? done : -EIO; }
            /* each input byte may echo up to 3 and queue 1 */
            while (done < n && ring_space(&p->in) > 2 && ring_space(&p->out) > 8)
                input_char(p, buf[done++]);
        } else {
            if (p->mrefs <= 0) { irq_restore(f); return done ? done : -EIO; }
            while (done < n && ring_space(&p->out) > 2) out_char(p, buf[done++]);
        }
        irq_restore(f);
        if (done == n) break;
        if (nonblock) return done ? done : -EAGAIN;
        if (proc_interrupted()) return done ? done : -EINTR;
        task_yield();
    }
    return done;
}

fs_node_t* pty_node(int i) { pty_t* p = get(i); return p ? p->node : NULL; }

/* ---------------- ioctl ---------------- */

int pty_ioctl(int i, bool master, uint32_t req, uint32_t arg) {
    pty_t* p = get(i);
    if (!p) return -EIO;
    proc_t* me = proc_current();
    switch (req) {
        case 0x5401:                                         /* TCGETS */
            if (!uptr(arg, sizeof(ktermios_t))) return -EFAULT;
            memcpy((void*)arg, &p->tio, sizeof(ktermios_t));
            return 0;
        case 0x5402: case 0x5403: case 0x5404: {            /* TCSETS/W/F */
            if (!uptr(arg, sizeof(ktermios_t))) return -EFAULT;
            uint32_t f = irq_save();
            bool was_canon = p->tio.c_lflag & TTY_ICANON;
            memcpy(&p->tio, (void*)arg, sizeof(ktermios_t));
            if (req == 0x5404) { p->in.n = p->in.h = p->in.t = 0; p->llen = 0; }
            if (was_canon && !(p->tio.c_lflag & TTY_ICANON)) flush_line(p);
            irq_restore(f);
            return 0;
        }
        case 0x5413:                                         /* TIOCGWINSZ */
            if (!uptr(arg, 8)) return -EFAULT;
            ((uint16_t*)arg)[0] = p->rows; ((uint16_t*)arg)[1] = p->cols;
            ((uint16_t*)arg)[2] = ((uint16_t*)arg)[3] = 0;
            return 0;
        case 0x5414: {                                       /* TIOCSWINSZ */
            if (!uptr(arg, 8)) return -EFAULT;
            uint16_t r = ((uint16_t*)arg)[0], c = ((uint16_t*)arg)[1];
            if (r != p->rows || c != p->cols) {
                p->rows = r; p->cols = c;
                signal_fg(p, 28);                            /* SIGWINCH */
            }
            return 0;
        }
        case 0x540F:                                         /* TIOCGPGRP */
            if (!uptr(arg, 4)) return -EFAULT;
            *(int*)arg = p->pgrp ? p->pgrp : (me ? me->pgid : 0);
            return 0;
        case 0x5410:                                         /* TIOCSPGRP */
            if (!uptr(arg, 4)) return -EFAULT;
            p->pgrp = *(int*)arg;
            return 0;
        case 0x5429:                                         /* TIOCGSID */
            if (!uptr(arg, 4)) return -EFAULT;
            if (!p->sid) return -ENOTTY;
            *(int*)arg = p->sid;
            return 0;
        case 0x540E:                                         /* TIOCSCTTY */
            if (master || !me) return -EINVAL;
            if (p->sid && p->sid != me->sid && !arg) return -EBUSY;
            me->ctty = i + 1;
            p->sid = me->sid;
            p->pgrp = me->pgid;
            return 0;
        case 0x5422:                                         /* TIOCNOTTY */
            if (me && me->ctty == i + 1) me->ctty = -1;
            return 0;
        case 0x80045430:                                     /* TIOCGPTN */
            if (!master) return -ENOTTY;
            if (!uptr(arg, 4)) return -EFAULT;
            *(uint32_t*)arg = (uint32_t)i;
            return 0;
        case 0x40045431:                                     /* TIOCSPTLCK */
            if (!master) return -ENOTTY;
            if (!uptr(arg, 4)) return -EFAULT;
            p->locked = *(int*)arg != 0;
            return 0;
        case 0x540B: {                                       /* TCFLSH */
            uint32_t f = irq_save();
            if (arg == 0 || arg == 2) { p->in.n = p->in.h = p->in.t = 0; p->llen = 0; }
            if (arg == 1 || arg == 2) { p->out.n = p->out.h = p->out.t = 0; }
            irq_restore(f);
            return 0;
        }
        case 0x5409: case 0x540A:                            /* TCSBRK, TCXONC */
        case 0x5425: case 0x5427: case 0x5428:               /* TCSBRKP, TIOC[SC]BRK */
            return 0;
    }
    return -EINVAL;
}
