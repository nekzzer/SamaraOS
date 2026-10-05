#include "fs/ext2.h"
#include "drivers/ata.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/smp.h"
#include "core/io.h"
#include "boot/pit.h"

/* ext2 volumes, the same way FAT works here: the whole tree is read into
   ramfs nodes at mount, changes mark the volume dirty and a task writes
   them back. Write-back is incremental now: every node keeps its inode and
   its blocks between syncs (ent_t), the bitmaps come from the disk and only
   what changed gets written. The old way laid the whole volume out again
   and one new file early in the tree moved every block after it.
   A volume labelled "/" is merged into the root: what's on it wins over the
   boot files, and /usr, /lib, /etc ... changes (apk!) go back to it.
   ext3/ext4 (journal, extents, 64bit, ...) mount read-only: we can read
   extents, we just don't write them. */

#define E2_MAX 4
#define E2_ID0 8                          /* mount ids 8.. (FAT has 1..4) */

typedef struct {
    fs_node_t* n;
    uint32_t ino, nb, nm;
    uint32_t* bl;                         /* nb data blocks, then nm indirect ones */
    uint32_t ib[15];                      /* i_block as on disk (or inline link) */
    uint32_t sz, mt, h, isig;             /* what we wrote last time */
    const char* dp;
    uint8_t type, seen;
} ent_t;

typedef struct {
    bool used, ro;
    int id, disk;
    fs_node_t* root;
    uint32_t bs, spb, nblocks, ninodes, ipg, bpg, ngroups, first_data, first_ino, isize;
    uint32_t gdt_blocks, incompat, rocompat, compat;
    uint8_t sb[1024];
    uint8_t* gdt;                         /* ngroups * 32 */
    uint8_t* mgdt;                        /* the same at mount: where metadata lives, never changes */
    uint32_t itb;                         /* inode table blocks per group */
    uint8_t* bbm, *ibm;                   /* block / inode bitmaps, one block per group */
    uint8_t* gdirty;
    uint32_t* bhash;                      /* per block: hash of what we last wrote */
    ent_t* E;
    uint32_t ne, ecap;
    int32_t* tab;                         /* node ptr -> E index, open addressing */
    uint32_t tcap;
    uint32_t acur, icur;
    volatile bool dirty;
    uint32_t dirty_ms, dirty_since;       /* last change, first change since the last sync */
} ev_t;

static ev_t vols[E2_MAX];

static void klog(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i]; h *= 16777619u;
        if (n > 0x100000 && !(i & 0x3FFFF)) bkl_yield(this_cpu());   // hashing a huge file used to freeze the other cpus
    }
    return h | 1;
}

static int rblk(ev_t* v, uint32_t b, void* buf) { return ata_read(v->disk, b * v->spb, (int)v->spb, buf); }
static int wblk(ev_t* v, uint32_t b, const void* buf) {
    uint32_t h = fnv(buf, v->bs);
    if (v->bhash[b] == h) return 0;
    v->bhash[b] = h;
    return ata_write(v->disk, b * v->spb, (int)v->spb, buf);
}

/* statfs of the volume n sits on, false if it isn't ext2. free count is from the last sync */
bool ext2_statfs(fs_node_t* n, uint32_t* bs, uint64_t* tot, uint64_t* fr) {
    fs_node_t* o = n ? fs_owner(n) : NULL;
    if (!o || o->mount_id < E2_ID0 || o->mount_id >= E2_ID0 + E2_MAX) return false;
    ev_t* v = &vols[o->mount_id - E2_ID0];
    if (!v->used) return false;
    *bs = v->bs; *tot = v->nblocks; *fr = rd32(v->sb + 12);
    return true;
}

/* volume label; we use "/some/path" labels as the mount point */
void ext2_label(int disk, char* out) {
    uint8_t s[1024];
    out[0] = 0;
    if (ata_read(disk, 2, 2, s) < 0 || rd16(s + 56) != 0xEF53) return;
    memcpy(out, s + 120, 16);
    out[16] = 0;
}

bool ext2_probe(int disk) {
    if (!ata_drive_present(disk)) return false;
    uint8_t s[1024];
    if (ata_read(disk, 2, 2, s) < 0) return false;
    return rd16(s + 56) == 0xEF53;
}

static uint8_t* inode_ptr(ev_t* v, uint32_t ino, uint8_t* blkbuf) {
    uint32_t g = (ino - 1) / v->ipg, idx = (ino - 1) % v->ipg;
    if (g >= v->ngroups) return NULL;
    uint32_t tab = rd32(v->gdt + g * 32 + 8);
    uint32_t off = idx * v->isize;
    if (rblk(v, tab + off / v->bs, blkbuf) < 0) return NULL;
    return blkbuf + off % v->bs;
}

/* logical -> physical block, 0 = hole. cache the last indirect blocks */
static uint32_t ind_blk[3], *ind_buf[3];
static uint32_t bmap(ev_t* v, const uint8_t* ino, uint32_t lb) {
    uint32_t flags = rd32(ino + 32);
    const uint8_t* ib = ino + 40;
    if (flags & 0x80000) {                          /* extents */
        static uint8_t eb[4096];
        const uint8_t* h = ib;
        for (int depth = 0; depth < 6; depth++) {
            if (rd16(h) != 0xF30A) return 0;
            int n = rd16(h + 2), dp = rd16(h + 6);
            const uint8_t* e = h + 12;
            if (!dp) {
                for (int i = 0; i < n; i++, e += 12) {
                    uint32_t st = rd32(e), len = rd16(e + 4);
                    if (len > 32768) len -= 32768;        /* uninitialized extent */
                    if (lb >= st && lb < st + len) return rd32(e + 8) + (lb - st);
                }
                return 0;
            }
            int pick = -1;
            for (int i = 0; i < n; i++) if (rd32(e + i * 12) <= lb) pick = i;
            if (pick < 0) return 0;
            if (rblk(v, rd32(e + pick * 12 + 4), eb) < 0) return 0;
            h = eb;
        }
        return 0;
    }
    uint32_t per = v->bs / 4;
    if (lb < 12) return rd32(ib + lb * 4);
    lb -= 12;
    uint32_t idx[3], lev, top;
    if (lb < per) { lev = 1; top = rd32(ib + 48); idx[0] = lb; }
    else if ((lb -= per) < per * per) { lev = 2; top = rd32(ib + 52); idx[0] = lb / per; idx[1] = lb % per; }
    else { lb -= per * per; lev = 3; top = rd32(ib + 56); idx[0] = lb / (per * per); idx[1] = lb / per % per; idx[2] = lb % per; }
    uint32_t b = top;
    for (uint32_t k = 0; k < lev && b; k++) {
        if (!ind_buf[k]) ind_buf[k] = kmalloc(4096);
        if (ind_blk[k] != b) { if (rblk(v, b, ind_buf[k]) < 0) return 0; ind_blk[k] = b; }
        b = ind_buf[k][idx[k]];
    }
    return b;
}

static uint64_t isize64(const uint8_t* ino) { return rd32(ino + 4) | ((uint64_t)rd32(ino + 108) << 32); }

static int fill_data(ev_t* v, const uint8_t* ino, uint32_t size, char* d);
static char* read_data(ev_t* v, const uint8_t* ino, uint32_t size) {
    char* d = kmalloc_big(size + 1);
    if (!d) return NULL;
    if (fill_data(v, ino, size, d) < 0) { kfree(d); return NULL; }
    return d;
}

/* runs of blocks that sit next to each other on disk go in one request,
   straight into d. one 4k request per block made xorg start in a minute
   (libLLVM alone is 161 MB) */
static int fill_data(ev_t* v, const uint8_t* ino, uint32_t size, char* d) {
    static uint8_t blk[4096];
    uint32_t n = (size + v->bs - 1) / v->bs, full = size / v->bs;
    for (uint32_t i = 0; i < n; ) {
        uint32_t pb = bmap(v, ino, i), take = size - i * v->bs < v->bs ? size - i * v->bs : v->bs;
        if (!pb) { memset(d + i * v->bs, 0, take); i++; continue; }
        if (i < full) {
            uint32_t run = 1;
            while (i + run < full && run < 128 && bmap(v, ino, i + run) == pb + run) run++;
            if (ata_read(v->disk, pb * v->spb, (int)(run * v->spb), d + i * v->bs) < 0) return -1;
            i += run;
            continue;
        }
        if (rblk(v, pb, blk) < 0) return -1;                     /* the tail */
        memcpy(d + i * v->bs, blk, take);
        i++;
    }
    d[size] = 0;
    return 0;
}

/* ---------- node -> ent table ---------- */

static uint32_t ph(fs_node_t* n, uint32_t cap) { return (((uintptr_t)n >> 4) * 2654435761u) & (cap - 1); }

/* 0 = no memory. it was unchecked and from the file arena, which apk fills
   up: a NULL table gave t_find garbage, files got each other's inodes and
   blocks, and the group descriptors ended up as zeros on the disk */
static int t_rebuild(ev_t* v) {
    uint32_t cap = 1024;
    while (cap < v->ne * 2 + 64) cap <<= 1;
    if (cap != v->tcap || !v->tab) {
        int32_t* t = kmalloc(cap * 4);
        if (!t) return -1;
        if (v->tab) kfree(v->tab);
        v->tab = t;
        v->tcap = cap;
    }
    for (uint32_t i = 0; i < cap; i++) v->tab[i] = -1;
    for (uint32_t i = 0; i < v->ne; i++) {
        if (!v->E[i].n) continue;
        uint32_t k = ph(v->E[i].n, cap);
        while (v->tab[k] != -1) k = (k + 1) & (cap - 1);
        v->tab[k] = (int32_t)i;
    }
    return 0;
}

static int t_find(ev_t* v, fs_node_t* n) {
    uint32_t k = ph(n, v->tcap);
    for (uint32_t t = 0; t < v->tcap && v->tab[k] != -1; t++, k = (k + 1) & (v->tcap - 1))
        if (v->tab[k] >= 0 && v->E[v->tab[k]].n == n) return v->tab[k];
    return -1;
}

static void t_del(ev_t* v, fs_node_t* n) {
    uint32_t k = ph(n, v->tcap);
    for (uint32_t t = 0; t < v->tcap && v->tab[k] != -1; t++, k = (k + 1) & (v->tcap - 1))
        if (v->tab[k] >= 0 && v->E[v->tab[k]].n == n) { v->tab[k] = -2; return; }   /* tombstone */
}

static int ent_new(ev_t* v, fs_node_t* n, uint32_t ino, uint8_t type) {
    if (v->ne == v->ecap) {
        uint32_t nc = v->ecap ? v->ecap * 2 : 1024;
        ent_t* ne = kmalloc(nc * sizeof(ent_t));
        if (!ne) return -1;
        if (v->E) { memcpy(ne, v->E, v->ne * sizeof(ent_t)); kfree(v->E); }
        v->E = ne; v->ecap = nc;
    }
    int i = (int)v->ne++;
    ent_t* e = &v->E[i];
    memset(e, 0, sizeof *e);
    e->n = n; e->ino = ino; e->type = type; e->seen = 1;
    if (v->ne * 2 >= v->tcap) { if (t_rebuild(v) < 0) { v->ne--; return -1; } }
    else {
        uint32_t k = ph(n, v->tcap);
        while (v->tab[k] >= 0) k = (k + 1) & (v->tcap - 1);
        v->tab[k] = i;
    }
    return i;
}

/* ---------- bitmaps ---------- */

static bool has_super(ev_t* v, uint32_t g) {
    if (g <= 1 || !(v->rocompat & 1)) return true;        /* sparse_super */
    for (uint32_t p = 3; p <= g; p *= 3) if (p == g) return true;
    for (uint32_t p = 5; p <= g; p *= 5) if (p == g) return true;
    for (uint32_t p = 7; p <= g; p *= 7) if (p == g) return true;
    return false;
}

/* superblocks, descriptors, bitmaps, inode tables. file data never goes
   there whatever else is wrong: the last line of defence for the disk */
static bool is_meta(ev_t* v, uint32_t b) {
    if (b <= v->first_data + v->gdt_blocks) return true;
    uint32_t g = (b - v->first_data) / v->bpg;
    if (g >= v->ngroups) return true;
    uint32_t base = v->first_data + g * v->bpg;
    if (has_super(v, g) && b <= base + v->gdt_blocks) return true;
    const uint8_t* gd = v->mgdt + g * 32;
    uint32_t it = rd32(gd + 8);
    return b == rd32(gd) || b == rd32(gd + 4) || (b >= it && b < it + v->itb);
}

static bool bb_get(ev_t* v, uint32_t b) {
    uint32_t i = b - v->first_data, k = i % v->bpg;
    return v->bbm[(i / v->bpg) * v->bs + k / 8] & (1 << (k & 7));
}
static void bb_set(ev_t* v, uint32_t b, bool on) {
    if (b < v->first_data || b >= v->nblocks) return;
    if (!on && is_meta(v, b)) { klog("ext2: tried to free a metadata block\r\n"); return; }
    uint32_t i = b - v->first_data, g = i / v->bpg, k = i % v->bpg;
    uint8_t* p = v->bbm + g * v->bs + k / 8;
    if (on) *p |= (uint8_t)(1 << (k & 7)); else *p &= (uint8_t)~(1 << (k & 7));
    v->gdirty[g] = 1;
}
static uint32_t balloc(ev_t* v) {
    uint32_t span = v->nblocks - v->first_data;
    for (uint32_t t = 0; t < span; t++) {
        uint32_t i = (v->acur + t) % span;
        if ((i & 7) == 0 && i + 8 <= span && v->bbm[(i / v->bpg) * v->bs + (i % v->bpg) / 8] == 0xFF) { t += 7; continue; }
        uint32_t b = v->first_data + i;
        if (!bb_get(v, b)) {
            bb_set(v, b, true);
            if (is_meta(v, b)) { klog("ext2: metadata block was free in the bitmap\r\n"); continue; }
            v->acur = i + 1; v->bhash[b] = 0; return b;
        }
    }
    return 0;
}

static bool ib_get(ev_t* v, uint32_t ino) {
    uint32_t i = ino - 1, k = i % v->ipg;
    return v->ibm[(i / v->ipg) * v->bs + k / 8] & (1 << (k & 7));
}
static void ib_set(ev_t* v, uint32_t ino, bool on) {
    uint32_t i = ino - 1, g = i / v->ipg, k = i % v->ipg;
    uint8_t* p = v->ibm + g * v->bs + k / 8;
    if (on) *p |= (uint8_t)(1 << (k & 7)); else *p &= (uint8_t)~(1 << (k & 7));
    v->gdirty[g] = 1;
}
static uint32_t ialloc(ev_t* v) {
    for (uint32_t t = 0; t < v->ninodes; t++) {
        uint32_t ino = v->first_ino + (v->icur + t) % (v->ninodes - v->first_ino + 1);
        if (!ib_get(v, ino)) { ib_set(v, ino, true); v->icur = ino - v->first_ino + 1; return ino; }
    }
    return 0;
}

static void free_blocks(ev_t* v, ent_t* e) {
    for (uint32_t i = 0; i < e->nb + e->nm; i++) if (e->bl[i]) bb_set(v, e->bl[i], false);
    if (e->bl) kfree(e->bl);
    e->bl = NULL; e->nb = e->nm = 0;
}

static uint32_t isig(uint32_t mode, uint32_t size, uint32_t mt, uint32_t links, uint32_t iblk, const uint32_t* ib) {
    uint32_t s[20] = { mode, size, mt, links, iblk };
    memcpy(s + 5, ib, 60);
    return fnv((const uint8_t*)s, sizeof(s));
}

/* ---------- mount: tree into the ramfs ---------- */

/* the blocks an inode has: data in order (0 = hole), then the indirect ones */
static int ent_load(ev_t* v, int ei, const uint8_t* raw) {
    ent_t* e = &v->E[ei];
    memcpy(e->ib, raw + 40, 60);
    uint16_t mode = rd16(raw);
    uint32_t size = rd32(raw + 4), per = v->bs / 4;
    e->isig = isig(mode, size, rd32(raw + 16), rd16(raw + 26), rd32(raw + 28), e->ib);
    if ((mode & 0xF000) == 0xA000 && size < 60 && !rd32(raw + 28)) return 0;   /* inline link */
    if (e->ib[14]) return -1;                                /* triple indirect, nope */
    uint32_t nb = (size + v->bs - 1) / v->bs, nm = 0;
    uint32_t* dind = NULL;
    if (e->ib[12]) nm++;
    if (e->ib[13]) {
        nm++;
        dind = kmalloc(v->bs);
        if (!dind || rblk(v, e->ib[13], dind) < 0) { if (dind) kfree(dind); return -1; }
        for (uint32_t i = 0; i < per; i++) if (dind[i]) nm++;
    }
    e->bl = kmalloc((nb + nm) * 4 + 4);
    if (!e->bl) { if (dind) kfree(dind); return -1; }
    for (uint32_t i = 0; i < nb; i++) e->bl[i] = bmap(v, raw, i);
    uint32_t k = nb;
    if (e->ib[12]) e->bl[k++] = e->ib[12];
    if (e->ib[13]) {
        e->bl[k++] = e->ib[13];
        for (uint32_t i = 0; i < per; i++) if (dind[i]) e->bl[k++] = dind[i];
        kfree(dind);
    }
    e->nb = nb; e->nm = nm;
    return 0;
}

/* a disk entry we didn't take (type clash with the boot files, a mount on
   top): an ent without a node, so the first sync frees it. was leaking
   unattached inodes before, e2fsck caught a busybox symlink */
static void orphan(ev_t* v, uint32_t ino, int depth) {
    static uint8_t ob[4096];
    uint8_t raw[256];
    uint8_t* p = inode_ptr(v, ino, ob);
    if (!p || !v->E || depth > 40) return;
    memcpy(raw, p, v->isize < 256 ? v->isize : 256);
    int ei = ent_new(v, NULL, ino, 0);
    if (ei < 0 || ent_load(v, ei, raw) < 0) { v->ro = true; return; }
    if ((rd16(raw) & 0xF000) != 0x4000) return;
    uint32_t sz = rd32(raw + 4);
    char* d = read_data(v, raw, sz);
    if (!d) return;
    for (uint32_t o = 0; o + 8 <= sz; ) {
        uint32_t ci = rd32((uint8_t*)d + o);
        uint16_t rl = rd16((uint8_t*)d + o + 4);
        uint8_t nl = (uint8_t)d[o + 6];
        if (rl < 8) break;
        bool dots = (nl == 1 && d[o + 8] == '.') || (nl == 2 && d[o + 8] == '.' && d[o + 9] == '.');
        o += rl;
        if (ci && !dots) orphan(v, ci, depth + 1);
    }
    kfree(d);
}

/* boot made /bin, void has /bin -> usr/bin: the boot dir goes away */
static void drop(fs_node_t* n) {
    while (n->child) { fs_node_t* c = n->child; drop(c); }
    fs_detach(n);
    fs_data_free(n);
    kfree(n);
}

static int load_dir(ev_t* v, fs_node_t* dir, uint32_t dino, int depth) {
    static uint8_t ib[4096];
    uint8_t raw[256];
    uint8_t* p = inode_ptr(v, dino, ib);
    if (!p || depth > 40) return -1;
    memcpy(raw, p, v->isize < 256 ? v->isize : 256);
    uint32_t sz = rd32(raw + 4);
    char* d = read_data(v, raw, sz);
    if (!d) return -1;
    if (v->E) {
        int ei = ent_new(v, dir, dino, FS_DIR);
        if (ei < 0 || ent_load(v, ei, raw) < 0) v->ro = true;
        else v->E[ei].h = fnv((uint8_t*)d, sz);
    }
    for (uint32_t o = 0; o + 8 <= sz; ) {
        uint32_t ino = rd32((uint8_t*)d + o);
        uint16_t rl = rd16((uint8_t*)d + o + 4);
        uint8_t nl = (uint8_t)d[o + 6];
        if (rl < 8) break;
        char name[FS_NAME_MAX];
        int k = nl < FS_NAME_MAX - 1 ? nl : FS_NAME_MAX - 1;
        memcpy(name, d + o + 8, (size_t)k);
        name[k] = 0;
        o += rl;
        if (!ino || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        uint8_t craw[256];
        p = inode_ptr(v, ino, ib);
        if (!p) continue;
        memcpy(craw, p, v->isize < 256 ? v->isize : 256);
        uint16_t mode = rd16(craw);
        uint32_t csz = (uint32_t)isize64(craw);
        /* merging into a tree that already has stuff (the "/" volume):
           the disk wins, except over mounts, devices and type clashes */
        fs_node_t* old = fs_child(dir, name);
        if (old && (old->mount_id || old->dev)) { orphan(v, ino, depth + 1); continue; }
        if (old && old->type == FS_DIR && (mode & 0xF000) == 0xA000 && dir == fs_root()) { drop(old); old = NULL; }
        fs_node_t* n = NULL;
        uint8_t ty = 0;
        uint32_t want = (mode & 0xF000) == 0x4000 ? FS_DIR : (mode & 0xF000) == 0x8000 ? FS_FILE : (mode & 0xF000) == 0xA000 ? FS_LINK : 0;
        if (!want || (old && old->type != want)) { orphan(v, ino, depth + 1); continue; }
        if ((mode & 0xF000) == 0x4000) {
            n = old ? old : fs_create(dir, name, FS_DIR);
            if (n) load_dir(v, n, ino, depth + 1);
            if (n) { n->mode = mode & 07777; n->mtime = rd32(craw + 16); }
            continue;                                         /* load_dir made its ent */
        } else if ((mode & 0xF000) == 0x8000) {
            n = old ? old : fs_create(dir, name, FS_FILE);
            if (n) {
                fs_data_free(n);
                n->size = csz;                                /* bytes come on first open */
                if (csz) { n->lazy = ino; n->lazy_vol = (uint8_t)(E2_ID0 + (v - vols)); }
            }
            ty = FS_FILE;
        } else if ((mode & 0xF000) == 0xA000) {
            static char tg[4096];
            if (csz < 60 && !rd32(craw + 28)) { memcpy(tg, craw + 40, csz); tg[csz] = 0; }
            else { char* t = read_data(v, craw, csz < 4000 ? csz : 4000); if (!t) continue; memcpy(tg, t, csz < 4000 ? csz : 4000); tg[csz < 4000 ? csz : 4000] = 0; kfree(t); }
            if (old) {
                fs_data_free(old);
                uint32_t l = (uint32_t)strlen(tg);
                old->data = kmalloc(l + 1);
                if (old->data) { memcpy(old->data, tg, l + 1); old->size = l; old->cap = l + 1; }
                n = old;
            } else n = fs_symlink(dir, name, tg);
            ty = FS_LINK;
        }
        if (!n) continue;
        n->mode = mode & 07777;
        n->mtime = rd32(craw + 16);
        if (v->E) {
            int ei = ent_new(v, n, ino, ty);
            if (ei < 0 || ent_load(v, ei, craw) < 0) { v->ro = true; continue; }
            ent_t* e = &v->E[ei];
            e->dp = n->data; e->sz = (uint32_t)n->size; e->mt = n->mtime;
            if (ty == FS_LINK) e->h = n->data ? fnv((uint8_t*)n->data, (uint32_t)n->size) : 1;
        }
    }
    dir->mtime = rd32(raw + 16);
    kfree(d);
    return 0;
}

/* ---------- write back ---------- */

static uint32_t meta_for(ev_t* v, uint32_t nb) {
    uint32_t per = v->bs / 4;
    if (nb <= 12) return 0;
    if (nb <= 12 + per) return 1;
    return 2 + (nb - 12 - per + per - 1) / per;
}

static int wdata(ev_t* v, uint32_t b, const void* buf) {
    if (is_meta(v, b)) { klog("ext2: refused a data write over metadata\r\n"); return -1; }
    return wblk(v, b, buf);
}

/* content into the ent's blocks, same blocks again if the count fits */
static int put_blocks(ev_t* v, int ei, const char* data, uint32_t len) {
    uint32_t per = v->bs / 4, nb = (len + v->bs - 1) / v->bs;
    if (nb > 12 + per + per * per) { klog("ext2: file over 4 GB\r\n"); return -1; }
    uint32_t nm = meta_for(v, nb);
    ent_t* e = &v->E[ei];
    if (nb != e->nb || nm != e->nm) {
        uint32_t* nl = kmalloc((nb + nm) * 4 + 4);
        if (!nl) { klog("ext2: no memory for a block list\r\n"); return -1; }
        uint32_t k = 0, old = e->nb + e->nm;
        for (uint32_t i = 0; i < old; i++) {
            if (!e->bl[i]) continue;
            if (k < nb + nm) nl[k++] = e->bl[i];
            else bb_set(v, e->bl[i], false);
        }
        while (k < nb + nm) nl[k++] = 0;
        if (e->bl) kfree(e->bl);
        e->bl = nl; e->nb = nb; e->nm = nm;
    }
    for (uint32_t i = 0; i < nb + nm; i++)
        if (!e->bl[i] && !(e->bl[i] = balloc(v))) { klog("ext2: out of blocks\r\n"); return -1; }   /* holes too */
    static uint8_t blk[4096];
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t take = len - i * v->bs < v->bs ? len - i * v->bs : v->bs;
        memcpy(blk, data + i * v->bs, take);
        if (take < v->bs) memset(blk + take, 0, v->bs - take);
        if (wdata(v, e->bl[i], blk) < 0) return -1;
        if (!(i & 255)) bkl_yield(this_cpu());
    }
    memset(e->ib, 0, 60);
    for (uint32_t i = 0; i < nb && i < 12; i++) e->ib[i] = e->bl[i];
    if (nm) {
        uint32_t* ind = (uint32_t*)blk;
        memset(blk, 0, v->bs);
        for (uint32_t i = 12; i < nb && i < 12 + per; i++) ind[i - 12] = e->bl[i];
        e->ib[12] = e->bl[nb];
        if (wdata(v, e->bl[nb], blk) < 0) return -1;
    }
    if (nm > 1) {
        uint32_t nd = nm - 2;
        e->ib[13] = e->bl[nb + 1];
        uint32_t* ind = (uint32_t*)blk;
        for (uint32_t j = 0; j < nd; j++) {
            memset(blk, 0, v->bs);
            for (uint32_t i = 0; i < per; i++) {
                uint32_t lb = 12 + per + j * per + i;
                if (lb < nb) ind[i] = e->bl[lb];
            }
            if (wdata(v, e->bl[nb + 2 + j], blk) < 0) return -1;
        }
        memset(blk, 0, v->bs);
        for (uint32_t j = 0; j < nd; j++) ind[j] = e->bl[nb + 2 + j];
        if (wdata(v, e->bl[nb + 1], blk) < 0) return -1;
    }
    return 0;
}

typedef struct { char* buf; uint32_t len, last; } db_t;

static void dir_add(ev_t* v, db_t* d, uint32_t ino, const char* name, uint8_t type) {
    uint32_t nl = (uint32_t)strlen(name), rl = (8 + nl + 3) & ~3u;
    uint32_t off = d->len;
    if (off / v->bs != (off + rl - 1) / v->bs) {           /* would cross a block: stretch the last one */
        uint32_t end = (off / v->bs + 1) * v->bs;
        wr16((uint8_t*)d->buf + d->last + 4, (uint16_t)(end - d->last));
        off = end;
    }
    uint8_t* e = (uint8_t*)d->buf + off;
    wr32(e, ino);
    wr16(e + 4, (uint16_t)rl);
    e[6] = (uint8_t)nl;
    e[7] = type;
    memcpy(e + 8, name, nl);
    d->last = off;
    d->len = off + rl;
}

/* what never goes to disk */
static bool skip(ev_t* v, fs_node_t* d, fs_node_t* c) {
    if (c->dev || c->unlinked) return true;
    if (c->mount_id && c->mount_id != v->id) return true;
    if (c->type == FS_FILE && c->data && !c->cap && c->size) return true;   /* still the boot archive's bytes */
    if (d == v->root && v->root == fs_root())
        if (!strcmp(c->name, "proc") || !strcmp(c->name, "dev") || !strcmp(c->name, "tmp") || !strcmp(c->name, "sys"))
            return true;
    return false;
}

static void walk(ev_t* v, fs_node_t* d) {
    for (fs_node_t* c = d->child; c; c = c->next) {
        if (skip(v, d, c)) continue;
        int ei = t_find(v, c);
        if (ei >= 0 && (v->E[ei].type != c->type || v->E[ei].seen)) { t_del(v, c); v->E[ei].n = NULL; ei = -1; }
        if (ei < 0) {
            uint32_t ino = ialloc(v);
            if (!ino) { klog("ext2: out of inodes\r\n"); continue; }
            ei = ent_new(v, c, ino, (uint8_t)c->type);
            if (ei < 0) continue;
        }
        v->E[ei].seen = 1;
        if (c->type == FS_DIR) walk(v, c);
    }
}

static int sync_vol(ev_t* v) {
    uint32_t now = fs_now();
    uint8_t* dib = kmalloc(v->ninodes / 8 + 1);              /* inodes to write this time */
    int32_t* i2e = kmalloc((v->ninodes + 1) * 4);
    if (!dib || !i2e) { if (dib) kfree(dib); if (i2e) kfree(i2e); return -1; }
    memset(dib, 0, v->ninodes / 8 + 1);
    for (uint32_t i = 0; i <= v->ninodes; i++) i2e[i] = -1;

    for (uint32_t i = 0; i < v->ne; i++) v->E[i].seen = 0;
    if (t_rebuild(v) < 0) { kfree(dib); kfree(i2e); return -1; }    /* nothing written yet */
    int ri = t_find(v, v->root);
    if (ri < 0) ri = ent_new(v, v->root, 2, FS_DIR);
    v->E[ri].seen = 1;
    walk(v, v->root);

    /* gone from the tree: free the blocks, zero the inode */
    uint32_t w = 0;
    for (uint32_t i = 0; i < v->ne; i++) {
        ent_t* e = &v->E[i];
        if (!e->seen || !e->n) {
            free_blocks(v, e);
            if (e->ino >= v->first_ino) { ib_set(v, e->ino, false); dib[e->ino >> 3] |= (uint8_t)(1 << (e->ino & 7)); }
            continue;
        }
        if (w != i) v->E[w] = *e;
        w++;
    }
    v->ne = w;
    if (t_rebuild(v) < 0) { v->ro = true; klog("ext2: no memory, volume read-only now\r\n"); kfree(dib); kfree(i2e); return -1; }

    bool ft = v->incompat & 2;
    int err = 0;
    for (uint32_t i = 0; i < v->ne; i++) {      /* one bad file used to stop the whole sync, forever */
        if (!(i & 63)) bkl_yield(this_cpu());
        ent_t* e = &v->E[i];
        fs_node_t* n = e->n;
        uint32_t links = 1, size = (uint32_t)n->size;
        if (n->type == FS_DIR) {
            /* children backwards: link_child prepends, this keeps the disk order */
            uint32_t cnt = 0, bytes = 0;
            for (fs_node_t* c = n->child; c; c = c->next) { cnt++; bytes += (uint32_t)strlen(c->name) + 12; }
            fs_node_t** arr = kmalloc(cnt * sizeof(fs_node_t*) + 4);
            db_t db = { kmalloc_big(bytes * 2 + v->bs * 2 + 64), 0, 0 };
            if (!arr || !db.buf) { if (arr) kfree(arr); if (db.buf) kfree(db.buf); err = -1; break; }
            memset(db.buf, 0, bytes * 2 + v->bs * 2 + 64);
            cnt = 0;
            for (fs_node_t* c = n->child; c; c = c->next) arr[cnt++] = c;
            uint32_t pino = e->ino;
            if (n != v->root && n->parent) { int pe = t_find(v, n->parent); if (pe >= 0) pino = v->E[pe].ino; }
            dir_add(v, &db, e->ino, ".", ft ? 2 : 0);
            dir_add(v, &db, pino, "..", ft ? 2 : 0);
            links = 2;
            while (cnt--) {
                fs_node_t* c = arr[cnt];
                int ce = t_find(v, c);
                if (ce < 0 || skip(v, n, c)) continue;
                uint8_t t = c->type == FS_DIR ? 2 : c->type == FS_LINK ? 7 : 1;
                dir_add(v, &db, v->E[ce].ino, c->name, ft ? t : 0);
                if (c->type == FS_DIR) links++;
            }
            kfree(arr);
            uint32_t end = ((db.len + v->bs - 1) / v->bs) * v->bs;
            wr16((uint8_t*)db.buf + db.last + 4, (uint16_t)(end - db.last));
            db.len = end;
            uint32_t h = fnv((uint8_t*)db.buf, db.len);
            e = &v->E[i];
            if (h != e->h) {
                if (put_blocks(v, (int)i, db.buf, db.len) < 0) { err = -1; kfree(db.buf); klog("ext2: dir not written: "); klog(n->name); klog("\r\n"); continue; }
                v->E[i].h = h;
            }
            kfree(db.buf);
            e = &v->E[i];
            size = e->nb * v->bs;
        } else if (n->type == FS_LINK && n->size < 60) {
            uint32_t h = n->data ? fnv((uint8_t*)n->data, (uint32_t)n->size) : 1;
            if (h != e->h || e->nb) {
                free_blocks(v, e);
                memset(e->ib, 0, 60);
                if (n->data) memcpy(e->ib, n->data, n->size);
                e->h = h;
            }
        } else {
            /* same buffer, size and an old enough mtime: don't even hash it */
            bool same = n->lazy || (e->dp == n->data && e->sz == size && e->mt == n->mtime && n->mtime + 2 < now && e->h);
            if (!same) {
                uint32_t h = size ? fnv((uint8_t*)n->data, size) : 1;
                if (h != e->h || (size + v->bs - 1) / v->bs != e->nb) {
                    if (put_blocks(v, (int)i, n->data, size) < 0) {   /* inode stays as it was */
                        char b[16]; utoa(size, b, 10);
                        klog("ext2: not written: "); klog(n->name); klog(" size "); klog(b); klog("\r\n");
                        err = -1;
                        continue;
                    }
                    e = &v->E[i];
                }
                e->h = h;
                e->dp = n->data; e->sz = size; e->mt = n->mtime;
            }
        }
        e = &v->E[i];
        uint32_t ty = n->type == FS_DIR ? 0x4000 : n->type == FS_LINK ? 0xA000 : 0x8000;
        uint32_t used = e->nm;
        for (uint32_t k = 0; k < e->nb; k++) if (e->bl[k]) used++;
        uint32_t s = isig(ty | (n->mode & 07777), size, n->mtime, links, used * v->spb, e->ib);
        if (s != e->isig) {
            e->isig = s;
            dib[e->ino >> 3] |= (uint8_t)(1 << (e->ino & 7));
            i2e[e->ino] = (int32_t)i;
        }
    }
    // if (err) klog("ext2: disk full\r\n");   said disk full for any failure, misleading

    /* the descriptors must still say what they said at mount, or we'd write
       bitmaps and inode tables to wherever garbage points */
    for (uint32_t g = 0; g < v->ngroups; g++)
        if (memcmp(v->gdt + g * 32, v->mgdt + g * 32, 12)) {
            klog("ext2: group descriptors changed in memory! volume read-only, nothing more written\r\n");
            v->ro = true;
            kfree(dib); kfree(i2e);
            return -1;
        }
    /* inodes, read-modify-write so whatever we don't know about stays */
    static uint8_t tb[4096];
    uint32_t cur = 0;
    for (uint32_t ino = 1; ino <= v->ninodes; ino++) {
        if (!(dib[ino >> 3] & (1 << (ino & 7)))) continue;
        uint32_t g = (ino - 1) / v->ipg, off = ((ino - 1) % v->ipg) * v->isize;
        uint32_t b = rd32(v->gdt + g * 32 + 8) + off / v->bs;
        if (b != cur) {
            if (cur) wblk(v, cur, tb);
            if (rblk(v, b, tb) < 0) { cur = 0; continue; }
            cur = b;
        }
        uint8_t* p = tb + off % v->bs;
        if (i2e[ino] < 0) {                                  /* deleted */
            memset(p, 0, v->isize);
            wr32(p + 20, now);
            continue;
        }
        ent_t* e = &v->E[i2e[ino]];
        fs_node_t* n = e->n;
        if (!rd16(p + 26) || !rd16(p)) {                    /* fresh one */
            memset(p, 0, v->isize);
            if (v->isize > 128) wr16(p + 128, 32);
        }
        uint16_t ty = n->type == FS_DIR ? 0x4000 : n->type == FS_LINK ? 0xA000 : 0x8000;
        wr16(p, (uint16_t)(ty | (n->mode & 07777)));
        uint32_t size = n->type == FS_DIR ? e->nb * v->bs : (uint32_t)n->size;
        wr32(p + 4, size);
        wr32(p + 8, n->mtime); wr32(p + 12, n->mtime); wr32(p + 16, n->mtime);
        wr32(p + 20, 0);
        uint32_t links = 1;
        if (n->type == FS_DIR) { links = 2; for (fs_node_t* c = n->child; c; c = c->next) if (c->type == FS_DIR && !skip(v, n, c) && t_find(v, c) >= 0) links++; }
        wr16(p + 26, (uint16_t)links);
        uint32_t used = e->nm;
        for (uint32_t k = 0; k < e->nb; k++) if (e->bl[k]) used++;
        wr32(p + 28, used * v->spb);
        wr32(p + 32, rd32(p + 32) & ~0x81000u);             /* no extents, no htree: we write plain */
        memcpy(p + 40, e->ib, 60);
        wr32(p + 108, 0);
    }
    if (cur) wblk(v, cur, tb);
    kfree(dib); kfree(i2e);

    /* bitmaps and counts of the groups we touched */
    uint32_t* dirs = kmalloc(v->ngroups * 4);
    if (dirs) {
        memset(dirs, 0, v->ngroups * 4);
        for (uint32_t i = 0; i < v->ne; i++) if (v->E[i].type == FS_DIR) dirs[(v->E[i].ino - 1) / v->ipg]++;
    }
    for (uint32_t g = 0; g < v->ngroups; g++) {
        if (!v->gdirty[g]) continue;
        v->gdirty[g] = 0;
        uint8_t* gd = v->gdt + g * 32;
        wblk(v, rd32(gd), v->bbm + g * v->bs);
        wblk(v, rd32(gd + 4), v->ibm + g * v->bs);
        uint32_t fb = 0, fi = 0;
        for (uint32_t k = 0; k < v->bpg; k++) {
            uint32_t b = v->first_data + g * v->bpg + k;
            if (b >= v->nblocks) break;
            if (!bb_get(v, b)) fb++;
        }
        for (uint32_t k = 0; k < v->ipg; k++) if (!ib_get(v, g * v->ipg + k + 1)) fi++;
        wr16(gd + 12, (uint16_t)fb);
        wr16(gd + 14, (uint16_t)fi);
        if (dirs) wr16(gd + 16, (uint16_t)dirs[g]);
    }
    if (dirs) kfree(dirs);
    uint32_t free_b = 0, free_i = 0;
    for (uint32_t g = 0; g < v->ngroups; g++) { free_b += rd16(v->gdt + g * 32 + 12); free_i += rd16(v->gdt + g * 32 + 14); }
    uint8_t* gbuf = kmalloc_big(v->gdt_blocks * v->bs);
    if (gbuf) {
        memset(gbuf, 0, v->gdt_blocks * v->bs);
        memcpy(gbuf, v->gdt, v->ngroups * 32);
        for (uint32_t b = 0; b < v->gdt_blocks; b++) wblk(v, v->first_data + 1 + b, gbuf + b * v->bs);
        kfree(gbuf);
    }
    wr32(v->sb + 12, free_b);
    wr32(v->sb + 16, free_i);
    wr32(v->sb + 48, now);                                  /* s_wtime */
    wr16(v->sb + 58, 1);                                    /* clean */
    ata_write(v->disk, 2, 2, v->sb);
    return err;
}

/* ---------- mount, sync task ---------- */

void ext2_dirty(int id) {
    for (int i = 0; i < E2_MAX; i++)
        if (vols[i].used && vols[i].id == id && !vols[i].ro) {
            if (!vols[i].dirty) vols[i].dirty_since = pit_uptime_ms();
            vols[i].dirty = true;
            vols[i].dirty_ms = pit_uptime_ms();
        }
}

static bool started;
static volatile uint32_t nsyncs;          /* finished passes, ext2_throttle waits on it */
static volatile bool kicked;

/* sync(2) and the task below both get here; the layout state is shared */
static volatile int busy;
static void lock(void) {
    for (;;) {
        uint32_t f = irq_save();
        if (!busy) { busy = 1; irq_restore(f); return; }
        irq_restore(f);
        task_sleep_ms(5);
    }
}
static void unlock(void) { busy = 0; }

/* a lazy file is opened: read it now. the hash goes into its ent, else
   the next sync would think it changed and write it all back */
static void rc_do(size_t need);
static int ext2_lazy(fs_node_t* n) {
    ev_t* v = NULL;
    for (int i = 0; i < E2_MAX; i++) if (vols[i].used && vols[i].id == n->lazy_vol) v = &vols[i];
    if (!v) return -1;
    uint32_t want = n->lazy, sz = (uint32_t)n->size;
    /* memory first, outside the lock: reclaim needs the lock to make room.
       fail = the node just stays lazy, open() says ENOMEM. it used to zero the
       size and the next sync wrote the file back empty */
    char* d = kmalloc_big(sz + 1);
    /* the reclaim in kmalloc_big only tries the lock and the sync holds it a
       lot when memory is tight: exec failed with ENOMEM. here we may wait */
    for (int t = 0; !d && t < 3; t++) {
        if (t) fs_need_room(sz + 1);            /* dirty stuff from apk: sync first, then it can go */
        else { lock(); rc_do(sz + 1); unlock(); }
        d = kmalloc_big(sz + 1);
    }
    if (!d) { klog("ext2: no memory for a lazy file\r\n"); return -1; }
    lock();
    if (n->lazy != want || n->size != sz) { unlock(); kfree(d); return 0; }   /* somebody beat us to it */
    static uint8_t lb[4096];
    uint8_t raw[256];
    uint8_t* p = inode_ptr(v, want, lb);
    if (!p) { unlock(); kfree(d); return -1; }
    memcpy(raw, p, v->isize < 256 ? v->isize : 256);
    if (fill_data(v, raw, sz, d) < 0) { klog("ext2: lazy read failed\r\n"); unlock(); kfree(d); return -1; }
    n->data = d;
    n->cap = sz + 1;
    n->lazy = 0;
    if (v->E && v->tab) {
        int ei = t_find(v, n);
        if (ei >= 0) {
            ent_t* e = &v->E[ei];
            e->dp = d; e->sz = sz; e->mt = n->mtime;
            e->h = sz ? fnv((uint8_t*)d, sz) : 1;
        }
    }
    unlock();
    return 0;
}

/* big arena is full: drop files we can read back. only clean ones (what's
   in memory is what's on disk), closed, and not one of the last lookups
   (exec and the kernel shell use n->data right after fs_resolve).
   walks the live tree, the ent array can point at freed nodes */
static uint32_t rc_freed, rc_want;
static void rc_walk(ev_t* v, fs_node_t* d, uint32_t now) {
    for (fs_node_t* c = d->child; c && rc_freed < rc_want; c = c->next) {
        if (skip(v, d, c)) continue;
        if (c->type == FS_DIR) { rc_walk(v, c, now); continue; }
        if (c->type != FS_FILE || c->lazy || !c->data || !c->cap || c->refs || fs_recent(c)) continue;
        int ei = t_find(v, c);
        if (ei < 0) continue;
        ent_t* e = &v->E[ei];
        if (e->dp != c->data || e->sz != c->size || e->mt != c->mtime || !e->h) continue;
        rc_freed += (uint32_t)c->cap;
        kfree(c->data);
        c->data = NULL; c->cap = 0;
        c->lazy = e->ino; c->lazy_vol = (uint8_t)v->id;
        e->dp = NULL;
    }
}

/* lock held by the caller */
static void rc_do(size_t need) {
    uint32_t f = irq_save();
    rc_freed = 0;
    rc_want = (uint32_t)need + 8 * 1024 * 1024;
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < E2_MAX && rc_freed < rc_want; i++)
        if (vols[i].used && !vols[i].ro && vols[i].E && vols[i].tab) rc_walk(&vols[i], vols[i].root, now);
    irq_restore(f);
}

static void ext2_reclaim(size_t need) {
    uint32_t f = irq_save();
    if (busy) { irq_restore(f); return; }                  /* sync or a lazy read is on it, don't wait */
    busy = 1;
    rc_freed = 0;
    rc_want = (uint32_t)need + 8 * 1024 * 1024;           /* some slack, not one file at a time */
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < E2_MAX && rc_freed < rc_want; i++)
        if (vols[i].used && !vols[i].ro && vols[i].E && vols[i].tab) rc_walk(&vols[i], vols[i].root, now);
    busy = 0;
    irq_restore(f);
    // { char b[16]; klog("ext2: reclaim "); utoa(rc_freed >> 10, b, 10); klog(b); klog("k\r\n"); }
}

int ext2_sync_all(void) {
    fs_sync_begin();                      /* before our lock: a writer may be in a lazy read */
    lock();
    for (int i = 0; i < E2_MAX; i++)
        if (vols[i].used && vols[i].dirty) { vols[i].dirty = false; sync_vol(&vols[i]); }
    nsyncs++;
    unlock();
    fs_sync_end();
    return 0;
}

static bool pressure(void) { return heap_big_used() > heap_big_total() / 20 * 17; }   /* 85% */

/* a quiet second, or 3 s since the first change, or the file arena filling
   up. it used to wait for a quiet second only: apk never stops writing,
   nothing got saved and dirty files filled all memory */
static void e2syncd(void) {
    for (;;) {
        task_sleep_ms(kicked ? 20 : 300);
        uint32_t now = pit_uptime_ms();
        bool any = false;
        for (int i = 0; i < E2_MAX; i++) {
            ev_t* v = &vols[i];
            if (v->used && v->dirty && (now - v->dirty_ms > 1000 || now - v->dirty_since > 3000 || pressure() || kicked)) any = true;
        }
        if (!any) { if (kicked) { kicked = false; nsyncs++; } continue; }
        kicked = false;
        fs_sync_begin();
        lock();
        for (int i = 0; i < E2_MAX; i++) {
            ev_t* v = &vols[i];
            if (!v->used || !v->dirty) continue;
            v->dirty = false;
            if (sync_vol(v) < 0) { klog("ext2: sync failed\r\n"); if (!v->ro) v->dirty = true; }   /* again next time */
        }
        nsyncs++;
        unlock();
        fs_sync_end();
    }
}

/* a writer when the file arena is nearly full: get the dirty files on disk
   (then they can be dropped) before adding more. called outside
   fs_write_begin, the sync waits for writers */
static void ext2_reclaim(size_t need);
static uint32_t last_throttle;

/* out of file memory in the middle of a write: sync, then drop clean files */
void ext2_make_room(size_t need) {
    if (!started) return;
    uint32_t n = nsyncs, t0 = pit_uptime_ms();
    kicked = true;
    while (nsyncs == n && pit_uptime_ms() - t0 < 5000) task_sleep_ms(5);
    ext2_reclaim(need + heap_big_total() / 8);
}
void ext2_throttle(void) {
    if (!started || !pressure()) return;
    uint32_t now = pit_uptime_ms();
    if (now - last_throttle < 1000) return;     /* once a second, not every write(): dd crawled */
    last_throttle = now;
    uint32_t n = nsyncs;
    kicked = true;
    while (nsyncs == n && pit_uptime_ms() - now < 5000) task_sleep_ms(5);
    ext2_reclaim(heap_big_total() / 4);          /* clean now: drop them, or the pressure never goes */
}

int ext2_mount(int disk, fs_node_t* at) {
    int s = -1;
    for (int i = 0; i < E2_MAX; i++) if (!vols[i].used) { s = i; break; }
    if (s < 0 || !at || at->type != FS_DIR) return -16;
    ev_t* v = &vols[s];
    memset(v, 0, sizeof *v);
    v->disk = disk;
    if (ata_read(disk, 2, 2, v->sb) < 0 || rd16(v->sb + 56) != 0xEF53) return -22;
    uint8_t* sb = v->sb;
    v->ninodes = rd32(sb); v->nblocks = rd32(sb + 4); v->first_data = rd32(sb + 20);
    v->bs = 1024u << rd32(sb + 24); v->bpg = rd32(sb + 32); v->ipg = rd32(sb + 40);
    uint32_t rev = rd32(sb + 76);
    v->first_ino = rev ? rd32(sb + 84) : 11;
    v->isize = rev ? rd16(sb + 88) : 128;
    v->compat = rd32(sb + 92); v->incompat = rd32(sb + 96); v->rocompat = rd32(sb + 100);
    if (v->bs > 4096 || !v->ipg || !v->bpg || v->isize < 128 || v->isize > 1024) return -22;
    if (v->incompat & 0x80) return -22;                     /* 64bit descriptors: not here */
    v->spb = v->bs / 512;
    v->ngroups = (v->nblocks - v->first_data + v->bpg - 1) / v->bpg;
    v->gdt_blocks = (v->ngroups * 32 + v->bs - 1) / v->bs;
    /* write only what we fully understand: filetype and sparse_super, large_file */
    v->ro = (v->incompat & ~2u) || (v->rocompat & ~3u) || (v->compat & 4);
    v->gdt = kmalloc(v->gdt_blocks * v->bs);
    v->mgdt = kmalloc(v->gdt_blocks * v->bs);
    if (!v->gdt || !v->mgdt) return -12;
    for (uint32_t b = 0; b < v->gdt_blocks; b++)
        if (rblk(v, v->first_data + 1 + b, v->gdt + b * v->bs) < 0) return -5;
    memcpy(v->mgdt, v->gdt, v->gdt_blocks * v->bs);
    v->itb = (v->ipg * v->isize + v->bs - 1) / v->bs;
    /* descriptors that point outside their group: a broken disk, don't
       write a single block to it (the guards below trust these) */
    for (uint32_t g = 0; g < v->ngroups && !v->ro; g++) {
        uint8_t* gd = v->gdt + g * 32;
        uint32_t lo = v->first_data + g * v->bpg, hi = lo + v->bpg;
        uint32_t bb = rd32(gd), ib = rd32(gd + 4), it = rd32(gd + 8);
        if (bb < lo || bb >= hi || ib < lo || ib >= hi || it < lo || it + v->itb > hi || bb >= v->nblocks) {
            klog("ext2: bad group descriptors, mounting read-only. run e2fsck on the host\r\n");
            v->ro = true;
        }
    }
    if (!v->ro) {
        v->bbm = kmalloc_big(v->ngroups * v->bs);
        v->ibm = kmalloc_big(v->ngroups * v->bs);
        v->gdirty = kmalloc(v->ngroups);
        v->bhash = kmalloc_big(v->nblocks * 4);
        if (!v->bbm || !v->ibm || !v->gdirty || !v->bhash) v->ro = true;
    }
    if (!v->ro) {
        memset(v->gdirty, 0, v->ngroups);
        memset(v->bhash, 0, v->nblocks * 4);
        for (uint32_t g = 0; g < v->ngroups && !v->ro; g++) {
            uint8_t* gd = v->gdt + g * 32;
            if (rblk(v, rd32(gd), v->bbm + g * v->bs) < 0 || rblk(v, rd32(gd + 4), v->ibm + g * v->bs) < 0) v->ro = true;
        }
        if (t_rebuild(v) < 0) v->ro = true;
        v->E = kmalloc(1024 * sizeof(ent_t));
        v->ecap = v->E ? 1024 : 0;
        if (!v->E) v->ro = true;
    }
    ind_blk[0] = ind_blk[1] = ind_blk[2] = 0;
    if (load_dir(v, at, 2, 0) < 0) return -5;
    fs_lazy_hook = ext2_lazy;
    heap_reclaim = ext2_reclaim;
    v->used = true;
    v->id = E2_ID0 + s;
    v->root = at;
    at->mount_id = (uint8_t)v->id;
    if (!started && !v->ro) { started = true; task_spawn("e2sync", e2syncd); }
    return v->ro ? 1 : 0;
}
