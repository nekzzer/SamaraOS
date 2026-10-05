#include "proc/file.h"
#include "proc/proc.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"

#define EAGAIN  11
#define EBADF   9
#define EINVAL  22
#define EINTR   4
#define ENOMEM  12
#define EDEADLK 35
#define O_ACCMODE 3

/* flock (kind 0, owner = open file description), fcntl record locks
   (kind 1, owner = the process' fd table) and OFD locks (kind 2, owner = file).
   flock and fcntl locks don't see each other. waiting is a sleep loop, nobody
   queues. e is the last byte, ~0 = to EOF */
typedef struct flk {
    struct flk* next;
    fs_node_t* node;
    void* own;
    int kind, type, pid;
    uint64_t s, e;
} flk_t;

static flk_t* locks;
static struct { void* own; void* on; } wt[32];

static bool clash(flk_t* a, int kind, void* own, int type, uint64_t s, uint64_t e) {
    if (a->own == own) return false;
    if ((a->kind == 0) != (kind == 0)) return false;
    if (a->type == 0 && type == 0) return false;
    return a->s <= e && s <= a->e;
}

static flk_t* find_clash(fs_node_t* n, int kind, void* own, int type, uint64_t s, uint64_t e) {
    for (flk_t* l = locks; l; l = l->next)
        if (l->node == n && clash(l, kind, own, type, s, e)) return l;
    return NULL;
}

/* [s,e] of this owner's locks goes away, cutting the ones that stick out */
static int cut(fs_node_t* n, int kind, void* own, uint64_t s, uint64_t e) {
    flk_t** pp = &locks;
    while (*pp) {
        flk_t* l = *pp;
        if (l->node != n || l->own != own || l->kind != kind || l->s > e || s > l->e) { pp = &l->next; continue; }
        if (l->s < s && l->e > e) {
            flk_t* r = kmalloc(sizeof(flk_t));
            if (!r) return -ENOMEM;
            *r = *l;
            r->s = e + 1;
            r->next = l->next;
            l->next = r;
            l->e = s - 1;
            return 0;
        }
        if (l->s < s) l->e = s - 1;
        else if (l->e > e) l->s = e + 1;
        else { *pp = l->next; kfree(l); continue; }
        pp = &l->next;
    }
    return 0;
}

static int add(fs_node_t* n, int kind, void* own, int type, uint64_t s, uint64_t e) {
    flk_t* l = kmalloc(sizeof(flk_t));
    if (!l) return -ENOMEM;
    l->node = n; l->own = own; l->kind = kind; l->type = type;
    l->pid = proc_current() ? proc_current()->tgid : 0;
    l->s = s; l->e = e;
    l->next = locks;
    locks = l;
    return 0;
}

static void wait_set(void* own, void* on) {
    int fr = -1;
    for (int i = 0; i < 32; i++) {
        if (wt[i].own == own) { wt[i].on = on; return; }
        if (!wt[i].own && fr < 0) fr = i;
    }
    if (fr >= 0) { wt[fr].own = own; wt[fr].on = on; }
}

static void wait_clr(void* own) {
    for (int i = 0; i < 32; i++) if (wt[i].own == own) wt[i].own = NULL;
}

static bool deadlock(void* me, void* on) {
    for (int d = 0; d < 16 && on; d++) {
        if (on == me) return true;
        void* nx = NULL;
        for (int i = 0; i < 32; i++) if (wt[i].own == on) nx = wt[i].on;
        on = nx;
    }
    return false;
}

static int flk_wait(fs_node_t* n, int kind, void* own, int type, uint64_t s, uint64_t e, bool wait) {
    flk_t* c;
    while ((c = find_clash(n, kind, own, type, s, e))) {
        if (!wait) return -EAGAIN;
        if (kind == 1 && deadlock(own, c->own)) { wait_clr(own); return -EDEADLK; }
        wait_set(own, c->own);
        if (proc_interrupted()) { wait_clr(own); return -EINTR; }
        task_sleep_ms(2);
    }
    wait_clr(own);
    return 0;
}

int flk_flock(file_t* f, int op) {
    if (f->type != F_NODE) return 0;
    fs_node_t* n = f->node;
    int t = op & ~4;
    if (t == 8) return cut(n, 0, f, 0, ~0ull);
    if (t != 1 && t != 2) return -EINVAL;
    int r = flk_wait(n, 0, f, t == 2, 0, ~0ull, !(op & 4));
    if (r < 0) return r;
    cut(n, 0, f, 0, ~0ull);                    /* the old one, sh -> ex */
    return add(n, 0, f, t == 2, 0, ~0ull);
}

int flk_fcntl(file_t* f, int cmd, uint8_t* u) {
    if (f->type != F_NODE) return -EBADF;
    bool ofd = cmd >= 36;
    int kind = ofd ? 2 : 1;
    void* own = ofd ? (void*)f : (void*)proc_current()->sh;
    int ty = *(int16_t*)u, wh = *(int16_t*)(u + 2);
    int64_t st = *(int64_t*)(u + 8), len = *(int64_t*)(u + 16);
    fs_node_t* n = f->node;
    if (ty < 0 || ty > 2) return -EINVAL;
    if (ofd && *(int32_t*)(u + 24)) return -EINVAL;
    if (wh == 1) st += (int64_t)f->off;
    else if (wh == 2) st += (int64_t)n->size;
    else if (wh != 0) return -EINVAL;
    uint64_t s, e;
    if (len < 0) { s = (uint64_t)(st + len); e = (uint64_t)st - 1; if (st + len < 0) return -EINVAL; }
    else { s = (uint64_t)st; e = len ? s + (uint64_t)len - 1 : ~0ull; }
    if (st < 0) return -EINVAL;
    bool get = cmd == 5 || cmd == 12 || cmd == 36;
    if (get) {
        flk_t* c = ty == 2 ? NULL : find_clash(n, kind, own, ty, s, e);
        if (!c) { *(int16_t*)u = 2; return 0; }
        *(int16_t*)u = (int16_t)c->type;
        *(int16_t*)(u + 2) = 0;
        *(int64_t*)(u + 8) = (int64_t)c->s;
        *(int64_t*)(u + 16) = c->e == ~0ull ? 0 : (int64_t)(c->e - c->s + 1);
        *(int32_t*)(u + 24) = c->kind == 2 ? -1 : c->pid;
        return 0;
    }
    if (ty == 2) return cut(n, kind, own, s, e);
    if (ty == 0 && (f->flags & O_ACCMODE) == 1) return -EBADF;
    if (ty == 1 && (f->flags & O_ACCMODE) == 0) return -EBADF;
    int r = flk_wait(n, kind, own, ty, s, e, cmd == 7 || cmd == 14 || cmd == 38);
    if (r < 0) return r;
    if ((r = cut(n, kind, own, s, e)) < 0) return r;
    return add(n, kind, own, ty, s, e);
}

/* posix locks of this process on the file: gone on any close of it */
void flk_close(void* sh, file_t* f) {
    if (!locks || !f || f->type != F_NODE) return;
    cut(f->node, 1, sh, 0, ~0ull);
}

void flk_exit(void* sh) {
    wait_clr(sh);
}

/* last close of the open file description */
void flk_release(file_t* f) {
    flk_t** pp = &locks;
    while (*pp) {
        flk_t* l = *pp;
        if (l->own == f) { *pp = l->next; kfree(l); }
        else pp = &l->next;
    }
}

int flk_text(char* b, int cap) {
    int k = 0, id = 1;
    for (flk_t* l = locks; l; l = l->next) {
        char tmp[160];
        char* p = tmp;
        const char* cl = l->kind == 0 ? "FLOCK  ADVISORY  " : l->kind == 2 ? "OFDLCK ADVISORY  " : "POSIX  ADVISORY  ";
        char num[24];
        utoa((uint32_t)id++, num, 10); for (char* q = num; *q; ) *p++ = *q++;
        *p++ = ':'; *p++ = ' ';
        for (const char* q = cl; *q; ) *p++ = *q++;
        const char* tn = l->type ? "WRITE " : "READ  ";
        for (const char* q = tn; *q; ) *p++ = *q++;
        utoa((uint32_t)l->pid, num, 10); for (char* q = num; *q; ) *p++ = *q++;
        for (const char* q = " 00:01:"; *q; ) *p++ = *q++;
        utoa((uint32_t)(((uint64_t)l->node >> 4) & 0x0FFFFFFF), num, 10); for (char* q = num; *q; ) *p++ = *q++;
        *p++ = ' ';
        utoa((uint32_t)l->s, num, 10); for (char* q = num; *q; ) *p++ = *q++;
        *p++ = ' ';
        if (l->e == ~0ull) { for (const char* q = "EOF"; *q; ) *p++ = *q++; }
        else { utoa((uint32_t)l->e, num, 10); for (char* q = num; *q; ) *p++ = *q++; }
        *p++ = '\n';
        if (k + (p - tmp) > cap) break;
        memcpy(b + k, tmp, (size_t)(p - tmp));
        k += (int)(p - tmp);
    }
    return k;
}
