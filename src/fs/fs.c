#include "fs/fs.h"
#include "core/task.h"
#include "core/io.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/clock.h"
#include "drivers/ata.h"
#include "fs/mount.h"

static fs_node_t* root_;

static fs_node_t* node_new(const char* name, fs_type_t type, fs_node_t* parent) {
    fs_node_t* n = (fs_node_t*)kmalloc(sizeof(fs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_NAME_MAX - 1);
    n->type = type;
    n->parent = parent;
    n->mode = type == FS_DIR ? 0755 : 0644;
    n->mtime = fs_now();
    return n;
}

static void link_child(fs_node_t* dir, fs_node_t* c) {
    c->next = dir->child;
    dir->child = c;
}

void fs_init(void) {
    root_ = node_new("/", FS_DIR, NULL);

    fs_node_t* home = node_new("home", FS_DIR, root_);
    fs_node_t* etc  = node_new("etc",  FS_DIR, root_);
    fs_node_t* tmp  = node_new("tmp",  FS_DIR, root_);
    link_child(root_, tmp);
    link_child(root_, etc);
    link_child(root_, home);

    fs_node_t* dev = node_new("dev", FS_DIR, root_);
    link_child(root_, dev);
    static const struct { const char* name; uint8_t dev; } devs[] = {
        { "null", FS_DEV_NULL }, { "zero", FS_DEV_ZERO }, { "full", FS_DEV_ZERO }, { "tty", FS_DEV_TTY },
        { "console", FS_DEV_TTY }, { "random", FS_DEV_RANDOM }, { "urandom", FS_DEV_RANDOM },
        { "fb0", FS_DEV_FB }, { "input-all", FS_DEV_INPUT }, { "ptmx", FS_DEV_PTMX },
        { "input-kbd", FS_DEV_EVKBD }, { "input-mouse", FS_DEV_EVMOUSE },
        { "rtc0", FS_DEV_RTC }, { "rtc", FS_DEV_RTC },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        fs_node_t* d = node_new(devs[i].name, FS_FILE, dev);
        d->dev = devs[i].dev;
        d->mode = 0666;
        link_child(dev, d);
    }

    link_child(dev, node_new("pts", FS_DIR, dev));      /* pty slaves appear here */
    link_child(dev, node_new("shm", FS_DIR, dev));      /* shm_open, MAP_SHARED for real (yutani) */

    fs_node_t* user = node_new("user", FS_DIR, home);
    link_child(home, user);

    fs_node_t* readme = node_new("README.txt", FS_FILE, user);
    const char* msg =
        "Welcome to SamaraOS!\n"
        "Try: help, neofetch, ls, cat README.txt, nano notes.txt\n";
    readme->size = strlen(msg);
    readme->cap  = readme->size + 1;
    readme->data = (char*)kmalloc(readme->cap);
    memcpy(readme->data, msg, readme->size + 1);
    link_child(user, readme);

    fs_node_t* hostname = node_new("hostname", FS_FILE, etc);
    const char* hn = "samara\n";
    hostname->size = strlen(hn);
    hostname->cap  = hostname->size + 1;
    hostname->data = (char*)kmalloc(hostname->cap);
    memcpy(hostname->data, hn, hostname->size + 1);
    link_child(etc, hostname);
}

fs_node_t* fs_root(void) { return root_; }

uint32_t fs_now(void) { return clock_epoch(); }

void (*fs_dirty_hook)(int mount_id);

void fs_touch(fs_node_t* n) {
    if (n && n->pc) pc_changed(n);
    fs_dirty(n);
}

/* mmap writeback already has the current bytes in the cache. */
void fs_dirty(fs_node_t* n) {
    for (; n; n = n->parent)
        if (n->mount_id) {
            if (mnt_ro_id(n->mount_id) || n->mount_id >= 16) return;
            if (n->mount_id >= 8) { extern void ext2_dirty(int); ext2_dirty(n->mount_id); }
            else if (fs_dirty_hook) fs_dirty_hook(n->mount_id);
            return;
        }
}

static fs_node_t* find_child(fs_node_t* dir, const char* name) {
    if (!dir || dir->type != FS_DIR) return NULL;
    if (!strcmp(name, "."))  return dir;
    if (!strcmp(name, "..")) return dir->parent ? dir->parent : dir;
    for (fs_node_t* c = dir->child; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* walks path; a symlink in the middle (or at the end with follow) is
   replaced by its target + the rest, 8 levels deep at most */
/* bind/mount aliases, one set per mount namespace (ns.c numbers them, 0 = initial) */
#define NBT 768
static struct { fs_node_t *src, *dst; uint8_t ns; } bt[NBT];
extern int proc_mntns(void);
extern fs_node_t* proc_root(void);

static fs_node_t* alias_of(fs_node_t* n, int ns) {
    for (int i = 0; i < NBT; i++)
        if (bt[i].dst == n && bt[i].ns == ns) return bt[i].src;
    return n;
}

int fs_bind_ns(fs_node_t* src, fs_node_t* dst, int ns) {
    int fr = -1;
    for (int i = 0; i < NBT; i++) {
        if (bt[i].dst == dst && bt[i].ns == ns) { bt[i].src = src; return 0; }
        if (!bt[i].dst && fr < 0) fr = i;
    }
    if (fr < 0) return -1;
    bt[fr].src = src; bt[fr].dst = dst; bt[fr].ns = (uint8_t)ns;
    dst->bnd = 1;
    return 0;
}

fs_node_t* fs_alias(fs_node_t* n) { return n->bnd ? alias_of(n, proc_mntns()) : n; }

int fs_bind(fs_node_t* src, fs_node_t* dst) { return fs_bind_ns(src, dst, proc_mntns()); }

void fs_unbind(fs_node_t* dst, int ns) {
    for (int i = 0; i < NBT; i++)
        if (bt[i].dst == dst && bt[i].ns == ns) bt[i].dst = NULL;
}

void fs_bind_copy(int from, int to) {
    for (int i = 0; i < NBT; i++)
        if (bt[i].dst && bt[i].ns == from) fs_bind_ns(bt[i].src, bt[i].dst, to);
}

/* ns is gone: give back its aliases, targets that were private tmpfs are returned to the caller one by one */
fs_node_t* fs_bind_drop(int ns, int* from) {
    for (int i = *from; i < NBT; i++) {
        if (!bt[i].dst || bt[i].ns != ns) continue;
        fs_node_t* t = bt[i].src;
        bt[i].dst = NULL;
        *from = i + 1;
        return t;
    }
    return NULL;
}

fs_node_t* fs_new_file(const char* name) { return node_new(name, FS_FILE, NULL); }
fs_node_t* fs_new_dir(const char* name, fs_node_t* parent) { return node_new(name, FS_DIR, parent); }

/* mountpoint lookup for a new mount in a private namespace: the last name is not
   followed through its alias, and a dir that is only an alias of foreign tree gets
   a private copy of its first level so the mount doesn't land in the real tree */
static fs_node_t* mp_walk(fs_node_t* cwd, const char* path, bool cp, fs_node_t* d0) {
    int ns = proc_mntns();
    fs_node_t* rt = proc_root();
    if (!rt) rt = root_;
    fs_node_t* cur = path[0] == '/' ? rt : cwd;
    fs_node_t* dent = NULL;                 /* last alias dentry we went through */
    if (d0 && path[0] != '/') { dent = d0; cur = alias_of(d0, ns); }
    char part[FS_NAME_MAX];
    const char* p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) return dent && cur == alias_of(dent, ns) ? dent : cur;
        int k = 0;
        while (*p && *p != '/' && k < FS_NAME_MAX - 1) part[k++] = *p++;
        part[k] = 0;
        bool last = true;
        for (const char* q = p; *q; q++) if (*q != '/') last = false;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (cur != rt && cur->parent) cur = cur->parent; continue; }
        fs_node_t* nx = find_child(cur, part);
        if (!nx) return NULL;
        if (nx->hl) nx = nx->hl;
        if (last) {
            if (nx->type == FS_LINK) return fs_resolve(cwd, path);
            /* cur is a bare alias target: copy it up first */
            if (cp && dent && cur == alias_of(dent, ns) && dent->type == FS_DIR && !cur->mode_shadow) {
                fs_node_t* sh = node_new(dent->name, FS_DIR, dent->parent);
                sh->mode_shadow = 1;
                sh->mode = cur->mode;
                for (fs_node_t* c = cur->child; c; c = c->next) {
                    if (c->hl) continue;
                    fs_node_t* s = node_new(c->name, c->type, sh);
                    s->mode = c->mode;
                    s->mode_shadow = 1;
                    link_child(sh, s);
                    fs_bind_ns(alias_of(c, ns), s, ns);
                }
                fs_bind_ns(sh, dent, ns);
                nx = find_child(sh, part);
                if (!nx) nx = node_new(part, FS_DIR, sh);
            }
            return nx;
        }
        if (nx->type == FS_LINK) {
            fs_node_t* t = fs_resolve(cur, part);
            if (!t) return NULL;
            cur = t; dent = NULL;
            continue;
        }
        fs_node_t* a = nx->bnd ? alias_of(nx, ns) : nx;
        if (a != nx) dent = nx;
        cur = a;
    }
}


fs_node_t* fs_mp(fs_node_t* cwd, const char* path) { return mp_walk(cwd, path, true, NULL); }
fs_node_t* fs_dent(fs_node_t* cwd, const char* path, fs_node_t* d0, bool cp) { return mp_walk(cwd, path, cp, d0); }

static fs_node_t* resolve(fs_node_t* cwd, const char* path, bool follow, int depth, bool dent) {
    if (!path || !*path) return cwd;
    fs_node_t* rt = proc_root();
    if (!rt) rt = root_;
    fs_node_t* cur = (path[0] == '/') ? rt : cwd;
    char part[FS_NAME_MAX];
    int pi = 0;
    for (const char* p = (path[0] == '/') ? path + 1 : path; ; p++) {
        if (*p == '/' || *p == 0) {
            if (pi > 0) {
                part[pi] = 0;
                fs_node_t* dir = cur;
                if (cur == rt && !strcmp(part, "..")) cur = dir;
                else cur = find_child(cur, part);
                if (!cur) return NULL;
                if (cur->bnd) cur = alias_of(cur, proc_mntns());
                pi = 0;
                const char* rest = p;
                while (*rest == '/') rest++;
                if (cur->hl && (*rest || follow || !dent)) cur = cur->hl;
                if (cur->type == FS_LINK && (*rest || follow)) {
                    if (depth > 8 || !cur->data) return NULL;
                    static char buf[1024];                       /* target + "/" + rest */
                    char tmp[1024];
                    int k = 0;
                    for (size_t i = 0; i < cur->size && k < 1000; i++) tmp[k++] = cur->data[i];
                    if (*rest && k < 1000) tmp[k++] = '/';
                    for (; *rest && k < 1022; rest++) tmp[k++] = *rest;
                    tmp[k] = 0;
                    memcpy(buf, tmp, (size_t)k + 1);
                    return resolve(dir, tmp, follow, depth + 1, false);
                }
            }
            if (*p == 0) break;
        } else if (pi < FS_NAME_MAX - 1) {
            part[pi++] = *p;
        }
    }
    return cur;
}

int (*fs_lazy_hook)(fs_node_t* n);

/* fs_write_begin/end, fs_sync_begin/end: in proc/syscall.c, they need the process table */

/* last few lookups. eviction keeps its hands off them: the caller is about to
   use ->data. was a 1 s age check on the uptime, but with lost ticks thrown
   away the uptime crawls during disk io and nothing was ever old enough */
static fs_node_t* recent[8];
static int recent_i;

void fs_need(fs_node_t* n) {
    if (!n) return;
    recent[recent_i++ & 7] = n;
    if (n->lazy && fs_lazy_hook) fs_lazy_hook(n);
}

bool fs_recent(fs_node_t* n) {
    for (int i = 0; i < 8; i++) if (recent[i] == n) return true;
    return false;
}

void fs_need_tree(fs_node_t* n) {
    fs_need(n);
    if (n->type == FS_DIR) for (fs_node_t* c = n->child; c; c = c->next) fs_need_tree(c);
}

fs_node_t* fs_owner(fs_node_t* n) {
    for (; n; n = n->parent) if (n->mount_id || !n->parent) return n;
    return root_;
}

fs_node_t* fs_resolve(fs_node_t* cwd, const char* path)    { fs_node_t* n = resolve(cwd, path, true, 0, false); fs_need(n); return n; }
fs_node_t* fs_resolve_nf(fs_node_t* cwd, const char* path) { fs_node_t* n = resolve(cwd, path, false, 0, false); fs_need(n); return n; }
fs_node_t* fs_peek(fs_node_t* cwd, const char* path, bool follow) { return resolve(cwd, path, follow, 0, false); }
fs_node_t* fs_peek_d(fs_node_t* cwd, const char* path) { return resolve(cwd, path, false, 0, true); }

fs_node_t* fs_symlink(fs_node_t* dir, const char* name, const char* target) {
    if (!dir || dir->type != FS_DIR || find_child(dir, name)) return NULL;
    fs_node_t* n = node_new(name, FS_LINK, dir);
    if (!n) return NULL;
    size_t len = strlen(target);
    n->data = (char*)kmalloc((uint32_t)len + 1);
    if (!n->data) { kfree(n); return NULL; }
    memcpy(n->data, target, len + 1);
    n->size = len;
    n->cap = len + 1;
    n->mode = 0777;
    link_child(dir, n);
    fs_touch(dir);
    return n;
}

static void split_dir_base(const char* path, char* dir, char* base) {
    int slash = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') slash = i;
    if (slash < 0) { dir[0] = 0; strncpy(base, path, FS_NAME_MAX - 1); base[FS_NAME_MAX-1]=0; return; }
    if (slash == 0) { dir[0] = '/'; dir[1] = 0; }
    else { for (int i = 0; i < slash; i++) dir[i] = path[i]; dir[slash] = 0; }
    strncpy(base, path + slash + 1, FS_NAME_MAX - 1);
    base[FS_NAME_MAX - 1] = 0;
}

fs_node_t* fs_create(fs_node_t* cwd, const char* path, fs_type_t type) {
    char dir[256], base[FS_NAME_MAX];
    split_dir_base(path, dir, base);
    fs_node_t* parent = dir[0] ? fs_resolve(cwd, dir) : cwd;
    if (!parent || parent->type != FS_DIR) return NULL;
    if (find_child(parent, base)) return NULL;
    fs_node_t* n = node_new(base, type, parent);
    if (!n) return NULL;
    link_child(parent, n);
    fs_touch(parent);
    return n;
}

int fs_unlink(fs_node_t* cwd, const char* path) {
    fs_node_t* n = resolve(cwd, path, false, 0, true);   /* no point reading it in to delete it */
    if (!n || n == root_) return -1;
    if (n->type == FS_DIR && n->child) return -1;
    fs_drop_name(n);
    return 0;
}

int fs_hlink(fs_node_t* t, fs_node_t* dir, const char* name) {
    fs_node_t* a = node_new(name, t->type, dir);
    if (!a) return -1;
    a->hl = t;
    a->hn = t->hn;
    t->hn = a;
    t->xl++;
    link_child(dir, a);
    dir->mtime = t->mtime = fs_now();
    fs_touch(dir);
    return 0;
}

void fs_drop_name(fs_node_t* n) {
    fs_node_t* t = n->hl;
    if (t) {                                   /* only a name */
        fs_node_t** pp = &t->hn;
        while (*pp && *pp != n) pp = &(*pp)->hn;
        if (*pp) *pp = n->hn;
        t->xl--;
        fs_detach(n);
        kfree(n);
        return;
    }
    fs_node_t* a = n->hn;
    if (a) {                                   /* the node moves into the place of another name */
        char nm[FS_NAME_MAX];
        fs_node_t* dir = a->parent;
        strcpy(nm, a->name);
        n->hn = a->hn;
        n->xl--;
        fs_detach(a);
        kfree(a);
        fs_detach(n);
        strcpy(n->name, nm);
        fs_attach(dir, n);
        return;
    }
    fs_detach(n);
    if (n->refs > 0) { n->unlinked = true; return; }   /* still open */
    pc_free(n);
    fs_data_free(n);
    kfree(n);
}

void fs_detach(fs_node_t* n) {
    fs_node_t* p = n->parent;
    if (!p) return;
    fs_touch(p);
    fs_node_t** link = &p->child;
    while (*link && *link != n) link = &(*link)->next;
    if (*link == n) *link = n->next;
    n->next = NULL;
    n->parent = NULL;
    p->mtime = fs_now();
}

void fs_attach(fs_node_t* dir, fs_node_t* n) {
    n->parent = dir;
    link_child(dir, n);
    dir->mtime = fs_now();
    fs_touch(dir);
}

void (*fs_free_hook)(fs_node_t* n);

void fs_release(fs_node_t* n) {
    if (!n || --n->refs > 0) return;
    if (n->unlinked) {
        if (fs_free_hook) fs_free_hook(n);       /* memfd: its shm frames go too */
        pc_free(n);
        fs_data_free(n);
        kfree(n);
    }
}

fs_node_t* fs_child(fs_node_t* dir, const char* name) { return find_child(dir, name); }

void node_lock(fs_node_t* n) { while (__atomic_test_and_set(&n->nlk, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
void node_unlock(fs_node_t* n) { __atomic_clear(&n->nlk, __ATOMIC_RELEASE); }

static volatile int nb_wr, nb_rd;
int nb_tree_enter(void) {
    if (nb_wr) return 0;
    __atomic_add_fetch(&nb_rd, 1, __ATOMIC_SEQ_CST);
    if (nb_wr) { __atomic_sub_fetch(&nb_rd, 1, __ATOMIC_SEQ_CST); return 0; }
    return 1;
}
void nb_tree_leave(void) { __atomic_sub_fetch(&nb_rd, 1, __ATOMIC_SEQ_CST); }
void nb_tree_close(void) {
    __atomic_add_fetch(&nb_wr, 1, __ATOMIC_SEQ_CST);
    while (nb_rd) __builtin_ia32_pause();
}
void nb_tree_open(void) { __atomic_sub_fetch(&nb_wr, 1, __ATOMIC_SEQ_CST); }

void fs_data_free(fs_node_t* n) {
    uint64_t fl = irq_save();
    node_lock(n);
    char* d = n->data;
    uint32_t c = n->cap;
    n->data = NULL;
    n->cap = 0;
    n->lazy = 0;                       /* whoever frees it puts new bytes in */
    node_unlock(n);
    irq_restore(fl);
    if (d && c) kfree(d);
}

void fs_set_static(fs_node_t* n, const char* data, size_t len) {
    fs_data_free(n);
    n->data = (char*)data;
    n->size = len;
    n->cap = 0;
    n->mtime = fs_now();
    fs_touch(n);
}

int fs_write(fs_node_t* f, const char* data, size_t len) {
    if (!f || f->type != FS_FILE) return -1;
    if (f->cap < len + 1) {
        fs_data_free(f);
        f->cap = len + 64;
        f->data = (char*)kmalloc_big(f->cap);
        if (!f->data) { f->cap = 0; f->size = 0; return -1; }
    }
    memcpy(f->data, data, len);
    f->data[len] = 0;
    f->size = len;
    f->mtime = fs_now();
    fs_touch(f);
    return (int)len;
}

int fs_append(fs_node_t* f, const char* data, size_t len) {
    if (!f || f->type != FS_FILE) return -1;
    size_t need = f->size + len + 1;
    if (f->cap < need) {
        char* nb = (char*)kmalloc_big(need + 64);
        if (!nb) return -1;
        if (f->data) memcpy(nb, f->data, f->size);
        fs_data_free(f);
        f->data = nb; f->cap = need + 64;
    }
    memcpy(f->data + f->size, data, len);
    f->size += len;
    f->data[f->size] = 0;
    f->mtime = fs_now();
    fs_touch(f);
    return (int)len;
}

/* mountinfo lines for a private ns, one per alias */
int fs_bt_info(int ns, char* out, int cap) {
    char l[320], p[256], t[12];
    strcpy(out, "1 0 0:1 / / rw - rootfs rootfs rw\n");
    int n = strlen(out);
    for (int i = 0; i < NBT; i++) {
        if (!bt[i].dst || bt[i].ns != ns) continue;
        fs_path(bt[i].dst, p, sizeof(p));
        utoa(100 + i, t, 10);
        strcpy(l, t); strcat(l, " 1 0:"); strcat(l, t); strcat(l, " / ");
        strcat(l, p); strcat(l, " rw,relatime - tmpfs none rw\n");
        int k = strlen(l);
        if (n + k >= cap) break;
        memcpy(out + n, l, k);
        n += k;
    }
    out[n] = 0;
    return n;
}

void fs_path(fs_node_t* n, char* out, size_t cap) {
    if (!n) { if (cap) out[0] = 0; return; }
    fs_node_t* rt = proc_root();           /* chrooted: paths start at its root */
    fs_node_t* a = n;
    while (a && a != rt) a = a->parent;
    char tmp[256] = {0};
    fs_node_t* parts[64];
    int np = 0, pos = 0;
    fs_node_t* top = NULL;
    if (!a && proc_mntns()) {
        /* outside of our root, but maybe bound in under it (pivot_root's oldroot) */
        for (fs_node_t* c = n; c && !top; c = c->parent)
            for (int i = NBT - 1; i >= 0 && !top; i--) {
                if (bt[i].ns != proc_mntns() || !bt[i].dst || bt[i].src != c || bt[i].dst == c) continue;
                fs_node_t* d = bt[i].dst;
                while (d && d != rt) d = d->parent;
                if (!d) continue;
                top = c;
                fs_path(bt[i].dst, tmp, sizeof(tmp));
                pos = strlen(tmp);
                if (pos == 1) pos = 0;
                for (fs_node_t* q = n; q && q != c && np < 64; q = q->parent) parts[np++] = q;
            }
    }
    if (!top) {
        if (!a) rt = root_;
        if (n == rt) { strncpy(out, "/", cap); out[cap-1]=0; return; }
        for (fs_node_t* c = n; c && c != rt && np < 64; c = c->parent) parts[np++] = c;
    } else if (!np && !pos) { strcpy(out, "/"); return; }
    for (int i = np - 1; i >= 0; i--) {
        tmp[pos++] = '/';
        for (const char* s = parts[i]->name; *s && pos < (int)sizeof(tmp) - 1; s++) tmp[pos++] = *s;
    }
    tmp[pos] = 0;
    strncpy(out, tmp, cap); out[cap-1] = 0;
}

void fs_add_disk_nodes(void) {
    fs_node_t* dev = fs_resolve(root_, "/dev");
    if (!dev) return;
    for (int i = 0; i < DISK_ALL; i++) {
        if (!ata_drive_present(i) || find_child(dev, ata_drive_name(i))) continue;
        fs_node_t* d = node_new(ata_drive_name(i), FS_FILE, dev);
        if (!d) return;
        d->dev = (uint8_t)(FS_DEV_DISK + i);
        d->mode = 0660;
        link_child(dev, d);
    }
}
