#include "fs/mount.h"
#include "fs/fatfs.h"
#include "fs/ext2.h"
#include "drivers/ata.h"
#include "core/string.h"
#include "core/heap.h"
#include "core/vmm.h"
#include "core/clock.h"
#include "boot/pit.h"

#define EBUSY 16
#define ENOTBLK 15
#define ENODEV 19
#define ENOTDIR 20
#define EINVAL 22
#define ENOSPC 28
#define EROFS 30

enum { K_PSEUDO, K_FAT, K_EXT, K_TMP };

typedef struct {
    bool used, ro;
    int kind, disk;
    char type[12], dev[24];
    fs_node_t* at;
    uint64_t limit;                    /* tmpfs bytes, 0 = no limit */
    uint32_t nino;                     /* tmpfs nr_inodes, 0 = no limit */
    uint64_t used_b, added;            /* bytes at the last walk, added since */
    uint32_t cnt;
} mnt_t;

#define NM 24
static mnt_t tab[NM];

static mnt_t* find_at(fs_node_t* at) {
    for (int i = 0; i < NM; i++) if (tab[i].used && tab[i].at == at) return &tab[i];
    return NULL;
}

// deepest mount that n sits on
static mnt_t* mnt_find(fs_node_t* n) {
    for (; n; n = n->parent) {
        mnt_t* e = find_at(n);
        if (e) return e;
    }
    return NULL;
}

static mnt_t* slot(void) {
    for (int i = 0; i < NM; i++) if (!tab[i].used) { memset(&tab[i], 0, sizeof(mnt_t)); tab[i].used = true; return &tab[i]; }
    return NULL;
}

void mnt_add(const char* type, const char* dev, fs_node_t* at, int disk) {
    mnt_t* e = find_at(at);
    if (!e) e = slot();
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->at = at;
    e->disk = disk;
    strncpy(e->type, type, 11);
    strncpy(e->dev, dev, 23);
    e->kind = !strcmp(type, "vfat") ? K_FAT : !strncmp(type, "ext", 3) ? K_EXT : K_PSEUDO;
}

void mnt_set_ro(fs_node_t* at) { mnt_t* e = find_at(at); if (e) e->ro = true; }

void mnt_init(void) {
    static const struct { const char* t; const char* p; } l[] = {
        { "proc", "/proc" }, { "devtmpfs", "/dev" }, { "devpts", "/dev/pts" }, { "sysfs", "/sys" },
    };
    for (unsigned i = 0; i < sizeof(l) / sizeof(l[0]); i++) {
        fs_node_t* n = fs_peek(fs_root(), l[i].p, true);
        if (n) mnt_add(l[i].t, l[i].t, n, -1);
    }
}

bool mnt_ro(fs_node_t* n) { mnt_t* e = mnt_find(n); return e && e->ro; }

bool mnt_ro_id(int id) {
    for (int i = 0; i < NM; i++)
        if (tab[i].used && tab[i].at && tab[i].at->mount_id == id && tab[i].kind != K_PSEUDO) return tab[i].ro;
    return false;
}

static void put_u(char* out, uint64_t v) {
    char t[24];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *out++ = t[--k];
    *out = 0;
}

static void line(char* o, mnt_t* e) {
    char path[256], t[24];
    fs_path(e->at, path, sizeof(path));
    strcpy(o, e->dev[0] ? e->dev : e->type);
    strcat(o, " "); strcat(o, path); strcat(o, " "); strcat(o, e->type);
    strcat(o, e->ro ? " ro" : " rw");
    if (e->kind == K_TMP) {
        strcat(o, ",nosuid,nodev,size=");
        put_u(t, (e->limit ? e->limit : pmm_total_frames() * 2048) / 1024);
        strcat(o, t); strcat(o, "k");
        if (e->nino) { strcat(o, ",nr_inodes="); put_u(t, e->nino); strcat(o, t); }
    }
    strcat(o, " 0 0\n");
}

int mnt_text(char* out, int cap) {
    int n = 0;
    char l[400];
    out[0] = 0;
    if (!find_at(fs_root())) { strcpy(l, "rootfs / rootfs rw 0 0\n"); n = strlen(l); memcpy(out, l, n); }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < NM; i++) {
            mnt_t* e = &tab[i];
            if (!e->used || (e->at == fs_root()) != (pass == 0)) continue;
            line(l, e);
            int k = strlen(l);
            if (n + k >= cap) break;
            memcpy(out + n, l, k);
            n += k;
        }
    out[n] = 0;
    return n;
}

void fs_free_tree(fs_node_t* n) {
    fs_node_t* c = n->child;
    while (c) {
        fs_node_t* nx = c->next;
        fs_free_tree(c);
        fs_data_free(c);
        kfree(c);
        c = nx;
    }
    n->child = NULL;
}

bool fs_tree_busy(fs_node_t* n) {
    for (fs_node_t* c = n->child; c; c = c->next)
        if (c->refs > 0 || c->mount_id || fs_tree_busy(c)) return true;
    return false;
}

static void walk(fs_node_t* d, uint64_t* sz, uint32_t* cnt) {
    for (fs_node_t* c = d->child; c; c = c->next) {
        if (c->hl) continue;
        (*cnt)++;
        if (c->type == FS_FILE && !c->dev) *sz += c->size;
        if (c->type == FS_DIR && !c->mount_id) walk(c, sz, cnt);
    }
}

static void recount(mnt_t* e) {
    uint64_t sz = 0;
    uint32_t cnt = 1;
    walk(e->at, &sz, &cnt);
    e->used_b = sz; e->cnt = cnt; e->added = 0;
}

int mnt_grow(fs_node_t* n, uint64_t newsize) {
    mnt_t* e = mnt_find(n);
    if (!e) return 0;
    if (e->ro) return -EROFS;
    if (e->kind != K_TMP || !e->limit || newsize <= n->size) return 0;
    uint64_t d = newsize - n->size;
    if (e->used_b + e->added + d > e->limit) {
        recount(e);
        if (e->used_b + d > e->limit) return -ENOSPC;
    }
    e->added += d;
    return 0;
}

int mnt_newnode(fs_node_t* dir) {
    mnt_t* e = mnt_find(dir);
    if (!e) return 0;
    if (e->ro) return -EROFS;
    if (e->kind != K_TMP || !e->nino) return 0;
    recount(e);
    return e->cnt >= e->nino ? -ENOSPC : 0;
}

// "64m", "50%", "1048576k"
static uint64_t num(const char* s, uint64_t whole) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    switch (*s) {
        case 'k': case 'K': v <<= 10; break;
        case 'm': case 'M': v <<= 20; break;
        case 'g': case 'G': v <<= 30; break;
        case '%': v = whole / 100 * v; break;
    }
    return v;
}

static void opts(mnt_t* e, const char* data, int* mode) {
    char o[160], *p;
    if (!data) return;
    strncpy(o, data, 159);
    o[159] = 0;
    for (p = o; *p; ) {
        char* t = p;
        while (*p && *p != ',') p++;
        if (*p) *p++ = 0;
        uint64_t ram = pmm_total_frames() * 4096;
        if (!strncmp(t, "size=", 5)) e->limit = num(t + 5, ram);
        else if (!strncmp(t, "nr_inodes=", 10)) e->nino = (uint32_t)num(t + 10, 0);
        else if (!strncmp(t, "nr_blocks=", 10)) e->limit = num(t + 10, 0) * 4096;
        else if (!strncmp(t, "mode=", 5)) { int m = 0; for (t += 5; *t >= '0' && *t <= '7'; t++) m = m * 8 + *t - '0'; *mode = m; }
        else if (!strcmp(t, "ro")) e->ro = true;
        else if (!strcmp(t, "rw")) e->ro = false;
    }
}

static bool pseudo_type(const char* t) {
    static const char* const l[] = { "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "mqueue",
                                     "debugfs", "tracefs", "securityfs", "configfs", "fusectl", "pstore", "bpf" };
    for (unsigned i = 0; i < sizeof(l) / sizeof(l[0]); i++) if (!strcmp(t, l[i])) return true;
    return false;
}

int mnt_mount(fs_node_t* src, fs_node_t* dst, const char* type, uint64_t flags, const char* data) {
    if (dst->type != FS_DIR) return -ENOTDIR;
    if (flags & 32) {                                        /* remount */
        mnt_t* e = find_at(dst);
        if (!e) e = mnt_find(dst);
        if (!e) return -EINVAL;
        int m = -1;
        mnt_t t = *e;
        t.ro = false;
        opts(&t, data, &m);
        e->limit = t.limit; e->nino = t.nino;
        e->ro = (flags & 1) || t.ro;
        return 0;
    }
    if (flags & (4096 | 8192)) return -EINVAL;     /* bind, move, shared..: no */
    if (!type) type = "auto";
    if (!strcmp(type, "tmpfs") || !strcmp(type, "ramfs")) {
        if (dst->mount_id || find_at(dst)) return -EBUSY;
        int id = MNT_TMP_ID;
        while (id < MNT_TMP_ID + 8) {
            bool used = false;
            for (int i = 0; i < NM; i++) if (tab[i].used && tab[i].kind == K_TMP && tab[i].at->mount_id == id) used = true;
            if (!used) break;
            id++;
        }
        if (id == MNT_TMP_ID + 8) return -EBUSY;
        mnt_t* e = slot();
        if (!e) return -EBUSY;
        e->kind = K_TMP;
        e->at = dst;
        e->disk = -1;
        e->ro = (flags & 1) != 0;
        strcpy(e->type, type);
        strncpy(e->dev, src ? src->name : "tmpfs", 23);
        if (!src || src->dev || src->type != FS_DIR) strcpy(e->dev, "tmpfs");
        int mode = -1;
        opts(e, data, &mode);
        if (mode >= 0) dst->mode = (uint16_t)mode;
        dst->mount_id = (uint8_t)id;
        return 0;
    }
    if (pseudo_type(type)) {
        if (find_at(dst)) return -EBUSY;
        mnt_add(type, type, dst, -1);
        mnt_t* e = find_at(dst);
        if (e) e->ro = (flags & 1) != 0;
        return 0;
    }
    if (!src) return -2;
    if (!FS_DEV_IS_DISK(src->dev)) return -ENOTBLK;
    int disk = src->dev - FS_DEV_DISK;
    for (int i = 0; i < NM; i++) if (tab[i].used && tab[i].kind != K_PSEUDO && tab[i].disk == disk) return -EBUSY;
    if (!strcmp(type, "auto")) type = ext2_probe(disk) ? "ext4" : fatfs_probe(disk) ? "vfat" : NULL;
    if (!type) return -EINVAL;
    int r;
    if (!strncmp(type, "ext", 3)) {
        if (!ext2_probe(disk)) return -EINVAL;
        r = ext2_mount(disk, dst);
        if (r > 0) r = 0;
    } else if (!strcmp(type, "vfat") || !strcmp(type, "msdos") || !strcmp(type, "fat")) {
        if (!fatfs_probe(disk)) return -EINVAL;
        r = fatfs_mount(disk, dst);
    } else return -ENODEV;
    if (r == 0 && (flags & 1)) { mnt_t* e = find_at(dst); if (e) e->ro = true; }
    return r;
}

int mnt_umount(fs_node_t* n) {
    mnt_t* e = NULL;
    if (n->dev && FS_DEV_IS_DISK(n->dev)) {
        for (int i = 0; i < NM; i++)
            if (tab[i].used && tab[i].kind != K_PSEUDO && tab[i].disk == n->dev - FS_DEV_DISK) e = &tab[i];
    } else e = find_at(n);
    if (!e) return -EINVAL;
    n = e->at;
    if (n == fs_root()) return -EBUSY;
    if (e->kind == K_PSEUDO) { e->used = false; return 0; }
    if (fs_tree_busy(n)) return -EBUSY;
    int r = 0;
    if (e->kind == K_TMP) {
        n->mount_id = 0;
        fs_free_tree(n);
    } else if (e->kind == K_FAT) r = fatfs_umount(n);
    else r = ext2_umount(n);
    if (r == 0) e->used = false;
    return r;
}

void mnt_tmpfs_boot(const char* path) {
    fs_node_t* n = fs_peek(fs_root(), path, true);
    if (n) mnt_mount(NULL, n, "tmpfs", 0, NULL);
}

int mnt_statfs(fs_node_t* n, uint64_t* b) {
    mnt_t* e = n ? mnt_find(n) : NULL;
    uint32_t cs, tc, fc;
    uint64_t tb, fb;
    uint32_t ino[2];
    if (e && e->kind == K_FAT && fatfs_statfs(e->at->mount_id, &cs, &tc, &fc)) {
        b[0] = 0x4d44; b[1] = cs; b[2] = tc; b[3] = b[4] = fc;
        b[8] = 255; b[9] = cs;
    } else if (e && e->kind == K_EXT && ext2_statfs(e->at, &cs, &tb, &fb, ino)) {
        b[0] = 0xEF53; b[1] = cs; b[2] = tb; b[3] = fb; b[4] = fb;
        b[5] = ino[0]; b[6] = ino[1];
        b[8] = 255; b[9] = cs;
    } else if (e && e->kind == K_TMP) {
        uint64_t lim = e->limit ? e->limit : pmm_total_frames() * 2048;
        uint64_t sz = 0;
        uint32_t cnt = 1;
        walk(e->at, &sz, &cnt);
        uint64_t used = (sz + 4095) / 4096;
        b[0] = 0x01021994; b[1] = 4096; b[2] = lim / 4096;
        b[3] = b[4] = b[2] > used ? b[2] - used : 0;
        if (!e->limit && b[3] > pmm_free_frames()) b[3] = b[4] = pmm_free_frames();
        b[5] = e->nino ? e->nino : b[2];
        b[6] = e->nino ? (e->nino > cnt ? e->nino - cnt : 0) : b[3];
        b[8] = 255; b[9] = 4096;
    } else if (e && e->kind == K_PSEUDO && strcmp(e->type, "devtmpfs")) {
        b[0] = !strcmp(e->type, "proc") ? 0x9fa0 : !strcmp(e->type, "sysfs") ? 0x62656572 : 0x1cd1;
        b[1] = 4096; b[8] = 255; b[9] = 4096;
    } else {
        b[0] = 0x858458f6; b[1] = 4096;
        b[2] = pmm_total_frames();
        b[3] = b[4] = pmm_free_frames();
        b[8] = 255; b[9] = 4096;
    }
    b[7] = (e ? (uint64_t)(e - tab) + 1 : 0);
    b[10] = e && e->ro ? 1 : 0;
    return 0;
}
