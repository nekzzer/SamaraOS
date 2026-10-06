/* namespaces. every process holds an index per kind (0 = the initial one).
   mount ns = a set of aliases in fs.c, pid ns = translated pids (proc->vpid[level]),
   uts = hostname, user = id maps (everybody is root underneath, this is mostly
   bookkeeping so bwrap/chromium are happy), net = loopback only, ipc = shm keys */

#include "proc/proc.h"
#include "fs/mount.h"
#include "core/heap.h"
#include "core/string.h"

#define EPERM  1
#define ENOENT 2
#define ESRCH  3
#define EINVAL 22
#define ENOSPC 28
#define EBADF  9
#define ENOMEM 12
#define ENOTDIR 20

#define NEWNS   0x00020000ull
#define NEWUTS  0x04000000ull
#define NEWIPC  0x08000000ull
#define NEWUSER 0x10000000ull
#define NEWPID  0x20000000ull
#define NEWNET  0x40000000ull

enum { N_MNT, N_UTS, N_IPC, N_USER, N_NET, N_PID, N_MAX };
#define NN 32

typedef struct { bool used; int refs; uint32_t ino; int parent, level, next; } ns_t;
static ns_t nst[N_MAX][NN];
static uint32_t next_ino = 4026532000u;

static char uhost[NN][65], udom[NN][65];
static bool uset[NN];

typedef struct { uint32_t in, out, cnt; } imap_t;
static struct { imap_t m[2][5]; int n[2]; bool sg_deny; } um[NN];

int proc_mntns(void) {
    proc_t* p = proc_current();
    return p ? p->mntns : 0;
}

bool ns_loopback_only(void) {
    proc_t* p = proc_current();
    return p && p->netns;
}

static void ns_boot(void) {
    static bool done;
    if (done) return;
    done = true;
    static const uint32_t base[N_MAX] = { 4026531841u, 4026531838u, 4026531839u, 4026531837u, 4026531840u, 4026531836u };
    for (int t = 0; t < N_MAX; t++) { nst[t][0].used = true; nst[t][0].refs = 1 << 20; nst[t][0].ino = base[t]; nst[t][0].next = 2; }
    um[0].n[0] = um[0].n[1] = 1;
    um[0].m[0][0] = um[0].m[1][0] = (imap_t){ 0, 0, 4294967295u };
}

static int ns_new(int t, int parent) {
    ns_boot();
    for (int i = 1; i < NN; i++) {
        if (nst[t][i].used) continue;
        memset(&nst[t][i], 0, sizeof(ns_t));
        nst[t][i].used = true;
        nst[t][i].refs = 0;
        nst[t][i].ino = next_ino++;
        nst[t][i].parent = parent;
        nst[t][i].level = t == N_PID ? nst[t][parent].level + 1 : 0;
        nst[t][i].next = 1;
        if (t == N_USER) memset(&um[i], 0, sizeof(um[i]));
        if (t == N_USER || t == N_PID) nst[t][parent].refs++;   // child keeps its parent alive, or the index gets reused and the chain loops
        return i;
    }
    return -1;
}

int ns_idx(proc_t* p, int t);
static void ns_get(int t, int i) { if (i) nst[t][i].refs++; }

static void ns_put(int t, int i) {
    if (!i || --nst[t][i].refs > 0) return;
    if (t == N_MNT) {
        int from = 0;
        fs_node_t* n;
        while ((n = fs_bind_drop(i, &from))) {
            // private tmpfs goes with its namespace
            if (n->mode_shadow == 2 && n->mount_id) mnt_umount(n);
        }
    }
    nst[t][i].used = false;
    if (t == N_USER || t == N_PID) ns_put(t, nst[t][i].parent);
}

static void host_of(int i, char* h, char* d) {
    if (uset[i]) { strcpy(h, uhost[i]); strcpy(d, udom[i]); return; }
    strcpy(h, "samara");
    fs_node_t* hn = fs_resolve(fs_root(), "/etc/hostname");
    if (hn && hn->data && hn->size) {
        int k = 0;
        while (k < 64 && k < (int)hn->size && hn->data[k] != '\n') { h[k] = hn->data[k]; k++; }
        if (k) h[k] = 0;
    }
    strcpy(d, "(none)");
}

/* apply CLONE_NEW* to p. pid is separate: it only counts for the children */
static int apply(proc_t* p, uint64_t fl, bool fork) {
    ns_boot();
    int n_mnt = -1, n_uts = -1, n_ipc = -1, n_user = -1, n_net = -1, n_pid = -1;
    if ((fl & NEWNS) && (n_mnt = ns_new(N_MNT, 0)) < 0) return -ENOSPC;
    if ((fl & NEWUTS) && (n_uts = ns_new(N_UTS, 0)) < 0) return -ENOSPC;
    if ((fl & NEWIPC) && (n_ipc = ns_new(N_IPC, 0)) < 0) return -ENOSPC;
    if ((fl & NEWUSER) && (n_user = ns_new(N_USER, p->userns)) < 0) return -ENOSPC;
    if ((fl & NEWNET) && (n_net = ns_new(N_NET, 0)) < 0) return -ENOSPC;
    if (fl & NEWPID) {
        int cur = p->kidns ? p->kidns : p->pidns;
        if (nst[N_PID][cur].level >= 3) return -ENOSPC;
        if ((n_pid = ns_new(N_PID, cur)) < 0) return -ENOSPC;
    }
    if (n_mnt >= 0) {
        fs_bind_copy(p->mntns, n_mnt);
        ns_put(N_MNT, p->mntns);
        p->mntns = (uint8_t)n_mnt;
        ns_get(N_MNT, n_mnt);
    }
    if (n_uts >= 0) {
        host_of(p->utsns, uhost[n_uts], udom[n_uts]);
        uset[n_uts] = true;
        ns_put(N_UTS, p->utsns);
        p->utsns = (uint8_t)n_uts;
        ns_get(N_UTS, n_uts);
    }
    if (n_ipc >= 0) {
        ns_put(N_IPC, p->ipcns);
        p->ipcns = (uint8_t)n_ipc;
        ns_get(N_IPC, n_ipc);
    }
    if (n_user >= 0) {
        ns_put(N_USER, p->userns);
        p->capx = false;                          // fresh userns: full caps inside it
        p->userns = (uint8_t)n_user;
        ns_get(N_USER, n_user);
    }
    if (n_net >= 0) {
        ns_put(N_NET, p->netns);
        p->netns = (uint8_t)n_net;
        ns_get(N_NET, n_net);
    }
    if (n_pid >= 0) {
        if (fork) {
            p->pidns = (uint8_t)n_pid;       /* the child itself is the init of it */
            ns_get(N_PID, n_pid);
        } else {
            ns_put(N_PID, p->kidns);
            p->kidns = (uint8_t)n_pid;
            ns_get(N_PID, n_pid);
        }
    }
    return 0;
}

int ns_unshare(uint64_t fl) {
    proc_t* p = proc_current();
    if (fl & ~(NEWNS | NEWUTS | NEWIPC | NEWUSER | NEWPID | NEWNET | 0x2000000ull | 0x80ull | 0x400ull | 0x200ull | 0x40000ull | 0x10000ull | 0x800ull))
        return -EINVAL;
    return apply(p, fl & (NEWNS | NEWUTS | NEWIPC | NEWUSER | NEWPID | NEWNET), false);
}

void ns_fork(proc_t* p, proc_t* c) {
    ns_boot();
    uint64_t fl = p->nsfl;
    p->nsfl = 0;
    c->mntns = p->mntns; c->utsns = p->utsns; c->ipcns = p->ipcns;
    c->userns = p->userns; c->netns = p->netns;
    c->pidns = p->kidns ? p->kidns : p->pidns;
    c->kidns = 0;
    if (!c->is_thread) {
        ns_get(N_MNT, c->mntns); ns_get(N_UTS, c->utsns); ns_get(N_IPC, c->ipcns);
        ns_get(N_USER, c->userns); ns_get(N_NET, c->netns);
        ns_get(N_PID, c->pidns);
        if (fl & (NEWNS | NEWUTS | NEWIPC | NEWUSER | NEWNET)) apply(c, fl & ~NEWPID, true);
        if (fl & NEWPID) {
            int cur = c->pidns;
            if (nst[N_PID][cur].level < 3) {
                int n = ns_new(N_PID, cur);
                if (n > 0) { c->pidns = (uint8_t)n; ns_get(N_PID, n); ns_put(N_PID, cur); }
            }
        }
    }
    /* a pid for every level, the child sees the last one */
    int a = c->pidns, lv[4], k = 0;
    while (a && k < 4) { lv[k++] = a; a = nst[N_PID][a].parent; }
    c->vpid[0] = c->pid;
    for (int i = 0; i < k; i++) c->vpid[nst[N_PID][lv[i]].level] = nst[N_PID][lv[i]].next++;
}

void ns_exit(proc_t* p) {
    if (p->pidns) {
        ns_t* n = &nst[N_PID][p->pidns];
        if (p->vpid[n->level] == 1) {             /* init of a pid ns is gone: everybody inside dies */
            for (int i = 0; i < proc_count(); i++) {
                proc_t* q = proc_at(i);
                if (!q || q->tgid == p->tgid || q->state != P_ALIVE) continue;   // own threads would call teardown(p) again
                int a = q->pidns;
                while (a && a != p->pidns) a = nst[N_PID][a].parent;
                if (a == p->pidns) proc_send_signal(q, 9);
            }
        }
    }
    if (p->is_thread) return;
    ns_put(N_MNT, p->mntns); ns_put(N_UTS, p->utsns); ns_put(N_IPC, p->ipcns);
    ns_put(N_USER, p->userns); ns_put(N_NET, p->netns); ns_put(N_PID, p->pidns);
    if (p->kidns) ns_put(N_PID, p->kidns);
    p->mntns = p->utsns = p->ipcns = p->userns = p->netns = p->kidns = 0;
}

int ns_pid_view(proc_t* v, proc_t* q) {
    int a = q->pidns, L = nst[N_PID][v->pidns].level;
    while (a != v->pidns) {
        if (nst[N_PID][a].level <= L) return 0;
        a = nst[N_PID][a].parent;
    }
    return q->vpid[L];
}

/* global pid -> what v calls it (0: v can't see it) and back */
int ns_gpid(proc_t* v, int g) {
    if (!v->pidns) return g;
    proc_t* q = proc_by_pid(g);
    return q ? ns_pid_view(v, q) : 0;
}

int ns_lpid(proc_t* v, int vp) {
    if (!v->pidns) return vp;
    for (int i = 0; i < proc_count(); i++) {
        proc_t* q = proc_at(i);
        if (q && ns_pid_view(v, q) == vp) return q->pid;
    }
    return 0;
}

proc_t* ns_pid_find(proc_t* v, int vp) {
    int g = ns_lpid(v, vp);
    return g ? proc_by_pid(g) : NULL;
}

static uint32_t id_in(int ns, uint32_t k, int g) {
    if (!ns) return k;
    uint32_t o = id_in(nst[N_USER][ns].parent, k, g);
    for (int i = 0; i < um[ns].n[g]; i++)
        if (o >= um[ns].m[g][i].out && o - um[ns].m[g][i].out < um[ns].m[g][i].cnt) return um[ns].m[g][i].in + (o - um[ns].m[g][i].out);
    return 65534;
}

int ns_uid(proc_t* p, int g) { return (int)id_in(p->userns, 0, g); }

int ns_uname(proc_t* p, char* host, char* dom) {
    if (!uset[p->utsns]) return 0;
    strcpy(host, uhost[p->utsns]);
    strcpy(dom, udom[p->utsns]);
    return 1;
}

int ns_sethost(proc_t* p, const char* s, int n, bool dom) {
    if (n < 0 || n > 64) return -EINVAL;
    int i = p->utsns;
    if (!uset[i]) { host_of(i, uhost[i], udom[i]); uset[i] = true; }
    char* d = dom ? udom[i] : uhost[i];
    memcpy(d, s, n);
    d[n] = 0;
    return 0;
}

int ns_idmap(proc_t* t, int what, const char* s, int n) {
    int i = t->userns;
    if (what == 2) {
        um[i].sg_deny = !strncmp(s, "deny", 4);
        return 0;
    }
    if (!i) return -EPERM;
    if (um[i].n[what]) return -EPERM;                    /* once only, like linux */
    int k = 0;
    const char* e = s + n;
    while (s < e && k < 5) {
        uint64_t v[3] = { 0, 0, 0 };
        int f = 0;
        while (s < e && *s != '\n' && f < 3) {
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            while (s < e && *s >= '0' && *s <= '9') v[f] = v[f] * 10 + (uint64_t)(*s++ - '0');
            f++;
        }
        while (s < e && *s != '\n') s++;
        if (s < e) s++;
        if (f < 3 || !v[2]) return -EINVAL;
        um[i].m[what][k++] = (imap_t){ (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2] };
    }
    if (!k) return -EINVAL;
    um[i].n[what] = k;
    return 0;
}

int ns_idmap_text(proc_t* t, int what, char* out, int cap) {
    int i = t->userns, len = 0;
    char tmp[16];
    if (what == 2) { strcpy(out, !i || !um[i].sg_deny ? "allow\n" : "deny\n"); return strlen(out); }
    ns_boot();
    for (int k = 0; k < um[i].n[what]; k++) {
        uint32_t v[3] = { um[i].m[what][k].in, um[i].m[what][k].out, um[i].m[what][k].cnt };
        for (int f = 0; f < 3; f++) {
            utoa(v[f], tmp, 10);
            int l = strlen(tmp);
            if (len + l + 12 >= cap) return len;
            memcpy(out + len, tmp, l);
            len += l;
            out[len++] = f == 2 ? '\n' : ' ';
        }
    }
    out[len] = 0;
    return len;
}

uint32_t ns_ino(proc_t* p, int type) {
    ns_boot();
    return nst[type][ns_idx(p, type)].ino;
}

int ns_pivot(const char* nw, const char* old) {
    proc_t* p = proc_current();
    fs_node_t* n = fs_resolve(p->sh->cwd, nw);
    if (!n) return -ENOENT;
    if (n->type != FS_DIR) return -ENOTDIR;
    fs_node_t* o = fs_mp(p->sh->cwd, old);
    if (!o) return -ENOENT;
    fs_node_t* oldroot = p->sh->root ? p->sh->root : fs_root();
    if (o != n && p->mntns) fs_bind(oldroot, o);
    for (int i = 0; i < proc_count(); i++) {
        proc_t* q = proc_at(i);
        if (!q || q->mntns != p->mntns) continue;
        if ((q->sh->root ? q->sh->root : fs_root()) == oldroot) q->sh->root = n;
    }
    return 0;
}

/* name for the ns fd nodes in /proc/pid/ns */
const char* ns_type_name(int t) {
    static const char* const nm[] = { "mnt", "uts", "ipc", "user", "net", "pid" };
    return t >= 0 && t < N_MAX ? nm[t] : "";
}

int ns_level(proc_t* p) { return nst[N_PID][p->pidns].level; }

int ns_idx(proc_t* p, int t) {
    return t == N_MNT ? p->mntns : t == N_UTS ? p->utsns : t == N_IPC ? p->ipcns :
           t == N_USER ? p->userns : t == N_NET ? p->netns : p->pidns;
}

int ns_join(int t, int idx) {
    proc_t* p = proc_current();
    ns_boot();
    if (idx < 0 || idx >= NN || !nst[t][idx].used) return -ENOENT;
    ns_get(t, idx);
    switch (t) {
        case N_MNT: ns_put(t, p->mntns); p->mntns = (uint8_t)idx; break;
        case N_UTS: ns_put(t, p->utsns); p->utsns = (uint8_t)idx; break;
        case N_IPC: ns_put(t, p->ipcns); p->ipcns = (uint8_t)idx; break;
        case N_USER: ns_put(t, p->userns); p->userns = (uint8_t)idx; break;
        case N_NET: ns_put(t, p->netns); p->netns = (uint8_t)idx; break;
        default: ns_put(t, p->kidns); p->kidns = (uint8_t)idx;
    }
    return 0;
}

