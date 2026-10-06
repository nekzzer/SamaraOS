/* inotify: one pipe_t per instance as the event queue, a global watch table */

#include "proc/file.h"
#include "proc/proc.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/wq.h"

#define IN_ISDIR 0x40000000
#define IN_IGNORED 0x8000
#define IN_ONESHOT 0x80000000
#define MAXW 8192
#define O_NONBLOCK 04000
#define EAGAIN 11
#define EINTR 4

typedef struct { file_t* f; int wd; fs_node_t* n; uint32_t mask; } iw_t;
static iw_t ws[MAXW];
static int nw;
static spin_t ilk;

static fs_node_t* key(fs_node_t* n) { return n->hl ? n->hl : n; }

static void rd(pipe_t* p, int off, void* dst, int n) {
    for (int i = 0; i < n; i++) ((char*)dst)[i] = p->buf[(off + i) % PIPE_SZ];
}

/* ilk held */
static void put(file_t* f, int wd, uint32_t mask, uint32_t cookie, const char* name) {
    pipe_t* p = f->pipe;
    uint32_t nl = name ? (strlen(name) + 16) & ~15u : 0;   // len incl. nul, padded to 16
    char ev[16 + FS_NAME_MAX + 16];
    memset(ev, 0, 16 + nl);
    uint32_t* h = (uint32_t*)ev;
    h[0] = wd; h[1] = mask; h[2] = cookie; h[3] = nl;
    if (name) memcpy(ev + 16, name, strlen(name));
    int tot = 16 + nl;
    uint64_t fl = spin_lock(&p->lk);
    if (f->cnt && (int)((p->head - (int)(f->cnt - 1) + PIPE_SZ) % PIPE_SZ) == tot && p->count >= tot) {
        char old[16 + FS_NAME_MAX + 16];     // same as the last one? then drop it
        rd(p, (int)(f->cnt - 1), old, tot);
        if (!memcmp(old, ev, tot) && ((int)(f->cnt - 1) - p->tail + PIPE_SZ) % PIPE_SZ < p->count) { spin_unlock(&p->lk, fl); return; }
    }
    if (p->count + tot > PIPE_SZ - (mask == 0x4000 ? 0 : 16)) {
        spin_unlock(&p->lk, fl);
        if (mask != 0x4000) put(f, -1, 0x4000, 0, NULL);       // IN_Q_OVERFLOW
        return;
    }
    f->cnt = p->head + 1;
    for (int i = 0; i < tot; i++) p->buf[(p->head + i) % PIPE_SZ] = ev[i];
    p->head = (p->head + tot) % PIPE_SZ;
    p->count += tot;
    wq_wake(&p->wq);
    spin_unlock(&p->lk, fl);
}

static void drop(int i) {
    ws[i] = ws[--nw];
    ws[nw].f = NULL;
}

void ino_ev(fs_node_t* n, uint32_t mask, const char* name, uint32_t cookie) {
    if (!nw || !n) return;
    n = key(n);
    uint64_t fl = spin_lock(&ilk);
    for (int i = 0; i < nw; i++) {
        if (ws[i].n != n || !(ws[i].mask & mask & 0xfff)) continue;
        put(ws[i].f, ws[i].wd, mask, cookie, name);
        if (ws[i].mask & IN_ONESHOT) { put(ws[i].f, ws[i].wd, IN_IGNORED, 0, NULL); drop(i--); }
    }
    spin_unlock(&ilk, fl);
}

/* something happened to n itself: tell n's watchers and the parent dir's */
int ino_any(void) { return nw; }

void ino_node(fs_node_t* n, uint32_t mask) {
    if (!nw || !n) return;
    if (n->type == FS_DIR) mask |= IN_ISDIR;
    ino_ev(n, mask, NULL, 0);
    if (n->parent) ino_ev(n->parent, mask, n->name, 0);
}

/* node goes away: self events, then the watches die with it */
void ino_gone(fs_node_t* n, bool self) {
    if (!nw || !n) return;
    n = key(n);
    if (self) ino_ev(n, 0x400, NULL, 0);
    uint64_t fl = spin_lock(&ilk);
    for (int i = 0; i < nw; i++)
        if (ws[i].n == n) { put(ws[i].f, ws[i].wd, IN_IGNORED, 0, NULL); drop(i--); }
    spin_unlock(&ilk, fl);
}

int ino_add(file_t* f, fs_node_t* n, uint32_t mask) {
    n = key(n);
    uint64_t fl = spin_lock(&ilk);
    for (int i = 0; i < nw; i++)
        if (ws[i].f == f && ws[i].n == n) {
            if (mask & 0x10000000) { spin_unlock(&ilk, fl); return -17; }
            ws[i].mask = (mask & 0x20000000) ? ws[i].mask | mask : mask;
            int wd = ws[i].wd;
            spin_unlock(&ilk, fl);
            return wd;
        }
    if (nw == MAXW) { spin_unlock(&ilk, fl); return -28; }
    ws[nw].f = f; ws[nw].n = n; ws[nw].mask = mask;
    ws[nw].wd = ++f->disk;
    nw++;
    spin_unlock(&ilk, fl);
    return f->disk;
}

int ino_rm(file_t* f, int wd) {
    uint64_t fl = spin_lock(&ilk);
    for (int i = 0; i < nw; i++)
        if (ws[i].f == f && ws[i].wd == wd) {
            put(f, wd, IN_IGNORED, 0, NULL);
            drop(i);
            spin_unlock(&ilk, fl);
            return 0;
        }
    spin_unlock(&ilk, fl);
    return -22;
}

void ino_close(file_t* f) {
    uint64_t fl = spin_lock(&ilk);
    for (int i = 0; i < nw; i++)
        if (ws[i].f == f) drop(i--);
    spin_unlock(&ilk, fl);
    wq_drain(&f->pipe->wq);
    kfree(f->pipe);
    f->pipe = NULL;
}

file_t* ino_new(int fl) {
    file_t* f = file_new(F_INOTIFY, 2 | (fl & O_NONBLOCK));
    if (!f) return NULL;
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) { kfree(f); return NULL; }
    memset(p, 0, sizeof(*p));
    f->pipe = p;
    return f;
}

int ino_read(file_t* f, char* buf, uint32_t n) {
    pipe_t* p = f->pipe;
    char tmp[16 + FS_NAME_MAX + 16];
    WQ_W(w);
    uint32_t got = 0;
    for (;;) {
        bool intr = got ? false : proc_interrupted();
        uint64_t fl = spin_lock(&p->lk);
        if (!p->count) {
            spin_unlock(&p->lk, fl);
            if (got) return got;
            if (f->flags & O_NONBLOCK) return -EAGAIN;
            if (intr) return -EINTR;
            wq_wait(&p->wq, &w, 10);
            continue;
        }
        uint32_t len;
        rd(p, p->tail + 12, &len, 4);
        uint32_t tot = 16 + len;
        if (tot > n - got) {
            spin_unlock(&p->lk, fl);
            return got ? (int)got : -22;
        }
        rd(p, p->tail, tmp, tot);
        p->tail = (p->tail + tot) % PIPE_SZ;
        p->count -= tot;
        spin_unlock(&p->lk, fl);
        memcpy(buf + got, tmp, tot);
        got += tot;
    }
}
