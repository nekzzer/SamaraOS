#include "fs/ext2.h"
#include "drivers/ata.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/io.h"
#include "boot/pit.h"

/* ext2 volumes, the same way FAT works here: the whole tree is read into
   ramfs nodes at mount, changes mark the volume dirty and a task writes the
   tree back. Write-back lays everything out again (inodes from 11 up,
   blocks from the first free one), keeping the metadata and the reserved
   inodes where they are; only blocks whose contents changed are written.
   ext3/ext4 (journal, extents, 64bit, ...) mount read-only: we can read
   extents, we just don't write them. */

#define E2_MAX 4
#define E2_ID0 8                          /* mount ids 8.. (FAT has 1..4) */

typedef struct {
    bool used, ro;
    int id, disk;
    fs_node_t* root;
    uint32_t bs, spb, nblocks, ninodes, ipg, bpg, ngroups, first_data, first_ino, isize;
    uint32_t gdt_blocks, resv_gdt, incompat, rocompat, compat, itb;
    uint8_t sb[1024];
    uint8_t* gdt;                         /* ngroups * 32 */
    uint8_t* keep;                        /* block bitmap: metadata + reserved inodes' blocks */
    uint8_t* resv;                        /* raw reserved inodes 1..first_ino-1 */
    uint32_t* bhash;                      /* per block: hash of what we last wrote */
    volatile bool dirty;
    uint32_t dirty_ms;
} ev_t;

static ev_t vols[E2_MAX];

static void klog(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h | 1;
}

static int rblk(ev_t* v, uint32_t b, void* buf) { return ata_read(v->disk, b * v->spb, (int)v->spb, buf); }
static int wblk(ev_t* v, uint32_t b, const void* buf) {
    uint32_t h = fnv(buf, v->bs);
    if (v->bhash[b] == h) return 0;
    v->bhash[b] = h;
    return ata_write(v->disk, b * v->spb, (int)v->spb, buf);
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

static char* read_data(ev_t* v, const uint8_t* ino, uint32_t size) {
    char* d = kmalloc_big(size + 1);
    if (!d) return NULL;
    static uint8_t blk[4096];
    uint32_t n = (size + v->bs - 1) / v->bs;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pb = bmap(v, ino, i), take = size - i * v->bs < v->bs ? size - i * v->bs : v->bs;
        if (!pb) { memset(d + i * v->bs, 0, take); continue; }
        if (rblk(v, pb, blk) < 0) { kfree(d); return NULL; }
        memcpy(d + i * v->bs, blk, take);
    }
    d[size] = 0;
    return d;
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
        fs_node_t* n = NULL;
        if ((mode & 0xF000) == 0x4000) {
            n = fs_create(dir, name, FS_DIR);
            if (n) load_dir(v, n, ino, depth + 1);
        } else if ((mode & 0xF000) == 0x8000) {
            n = fs_create(dir, name, FS_FILE);
            if (n && csz) {
                n->data = read_data(v, craw, csz);
                if (n->data) { n->size = csz; n->cap = csz + 1; }
            }
        } else if ((mode & 0xF000) == 0xA000) {
            static char tg[4096];
            if (csz < 60 && !rd32(craw + 28)) { memcpy(tg, craw + 40, csz); tg[csz] = 0; }
            else { char* t = read_data(v, craw, csz < 4000 ? csz : 4000); if (!t) continue; memcpy(tg, t, csz < 4000 ? csz : 4000); tg[csz < 4000 ? csz : 4000] = 0; kfree(t); }
            n = fs_symlink(dir, name, tg);
        }
        if (n) { n->mode = mode & 07777; n->mtime = rd32(craw + 16); }
    }
    kfree(d);
    return 0;
}

static bool has_super(ev_t* v, uint32_t g) {
    if (g <= 1 || !(v->rocompat & 1)) return true;        /* sparse_super */
    for (uint32_t p = 3; p <= g; p *= 3) if (p == g) return true;
    for (uint32_t p = 5; p <= g; p *= 5) if (p == g) return true;
    for (uint32_t p = 7; p <= g; p *= 7) if (p == g) return true;
    return false;
}

static void keep_set(ev_t* v, uint32_t b) { if (b < v->nblocks) v->keep[b >> 3] |= (uint8_t)(1 << (b & 7)); }
static bool keep_get(ev_t* v, uint32_t b) { return v->keep[b >> 3] & (1 << (b & 7)); }

/* every block an inode owns, indirect ones too */
static void keep_inode(ev_t* v, const uint8_t* ino) {
    if (rd32(ino + 32) & 0x80000) return;                 /* extents: ro anyway */
    uint32_t per = v->bs / 4;
    uint32_t* buf = kmalloc(v->bs);
    uint32_t* buf2 = kmalloc(v->bs);
    if (!buf || !buf2) return;
    for (int i = 0; i < 12; i++) keep_set(v, rd32(ino + 40 + i * 4));
    uint32_t s = rd32(ino + 88), dd = rd32(ino + 92);      /* i_block[12], [13] */
    if (s) { keep_set(v, s); if (rblk(v, s, buf) == 0) for (uint32_t i = 0; i < per; i++) keep_set(v, buf[i]); }
    if (dd) {
        keep_set(v, dd);
        if (rblk(v, dd, buf) == 0)
            for (uint32_t i = 0; i < per; i++) if (buf[i]) {
                keep_set(v, buf[i]);
                if (rblk(v, buf[i], buf2) == 0) for (uint32_t k = 0; k < per; k++) keep_set(v, buf2[k]);
            }
    }
    kfree(buf); kfree(buf2);
}

/* ---------- write back ---------- */

typedef struct { fs_node_t* n; uint32_t ino, parent, nb, nind, blk[15], links; char* dirbuf; uint32_t dirlen, dlast; } wn_t;

static wn_t* W;
static int nW, capW;
static uint32_t cur_blk;

static uint32_t alloc_blk(ev_t* v) {
    while (cur_blk < v->nblocks && keep_get(v, cur_blk)) cur_blk++;
    return cur_blk < v->nblocks ? cur_blk++ : 0;
}

static void collect(fs_node_t* d, uint32_t parent) {
    for (fs_node_t* c = d->child; c; c = c->next) {
        if (c->dev || c->unlinked) continue;
        if (nW == capW) {
            int nc = capW ? capW * 2 : 1024;
            wn_t* nw = kmalloc_big((uint32_t)nc * sizeof(wn_t));
            if (!nw) return;
            if (W) { memcpy(nw, W, (size_t)nW * sizeof(wn_t)); kfree(W); }
            W = nw; capW = nc;
        }
        int me = nW++;
        memset(&W[me], 0, sizeof(wn_t));
        W[me].n = c;
        W[me].parent = parent;
        if (c->type == FS_DIR) collect(c, (uint32_t)me);
    }
}

/* data blocks for len bytes (plus indirect ones), written out */
static int put_data(ev_t* v, wn_t* w, const char* data, uint32_t len) {
    uint32_t per = v->bs / 4, n = (len + v->bs - 1) / v->bs;
    static uint8_t blk[4096];
    uint32_t* ind = NULL, *dind = NULL, *ind2 = NULL;
    uint32_t ind_b = 0, dind_b = 0, ind2_b = 0;
    w->nb = n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = alloc_blk(v);
        if (!b) return -1;
        uint32_t take = len - i * v->bs < v->bs ? len - i * v->bs : v->bs;
        memset(blk, 0, v->bs);
        memcpy(blk, data + i * v->bs, take);
        wblk(v, b, blk);
        if (i < 12) { w->blk[i] = b; continue; }
        uint32_t j = i - 12;
        if (j < per) {
            if (!ind) { ind = kmalloc(v->bs); memset(ind, 0, v->bs); ind_b = alloc_blk(v); w->blk[12] = ind_b; w->nind++; }
            ind[j] = b;
            continue;
        }
        j -= per;
        if (j >= per * per) return -1;                 /* > ~4 GB with 4k blocks, no */
        if (!dind) { dind = kmalloc(v->bs); memset(dind, 0, v->bs); dind_b = alloc_blk(v); w->blk[13] = dind_b; w->nind++; }
        if (j % per == 0) {
            if (ind2) { wblk(v, ind2_b, ind2); kfree(ind2); }
            ind2 = kmalloc(v->bs); memset(ind2, 0, v->bs);
            ind2_b = alloc_blk(v); w->nind++;
            dind[j / per] = ind2_b;
        }
        ind2[j % per] = b;
    }
    if (ind) { wblk(v, ind_b, ind); kfree(ind); }
    if (ind2) { wblk(v, ind2_b, ind2); kfree(ind2); }
    if (dind) { wblk(v, dind_b, dind); kfree(dind); }
    return 0;
}

static void dir_add(ev_t* v, wn_t* d, uint32_t ino, const char* name, uint8_t type) {
    uint32_t* last = &d->dlast;
    uint32_t nl = (uint32_t)strlen(name), rl = (8 + nl + 3) & ~3u;
    uint32_t off = d->dirlen;
    if (off / v->bs != (off + rl - 1) / v->bs) {           /* would cross a block: stretch the last one */
        uint32_t end = (off / v->bs + 1) * v->bs;
        wr16((uint8_t*)d->dirbuf + *last + 4, (uint16_t)(end - *last));
        off = end;
    }
    uint8_t* e = (uint8_t*)d->dirbuf + off;
    wr32(e, ino);
    wr16(e + 4, (uint16_t)rl);
    e[6] = (uint8_t)nl;
    e[7] = type;
    memcpy(e + 8, name, nl);
    *last = off;
    d->dirlen = off + rl;
}

static int sync_vol(ev_t* v) {
    nW = 0;
    /* W[0] is the root */
    if (!capW) { capW = 1024; W = kmalloc_big((uint32_t)capW * sizeof(wn_t)); if (!W) return -1; }
    memset(&W[0], 0, sizeof(wn_t));
    W[0].n = v->root;
    nW = 1;
    collect(v->root, 0);
    if ((uint32_t)nW + v->first_ino > v->ninodes) { klog("ext2: out of inodes\r\n"); return -1; }
    W[0].ino = 2;
    for (int i = 1; i < nW; i++) W[i].ino = v->first_ino + (uint32_t)i - 1;
    bool ft = v->incompat & 2;
    /* directories: entries first, their blocks go with the rest */
    for (int i = 0; i < nW; i++) {
        fs_node_t* n = W[i].n;
        if (n->type != FS_DIR) continue;
        uint32_t cnt = 2, bytes = 0;
        for (fs_node_t* c = n->child; c; c = c->next) if (!c->dev && !c->unlinked) { cnt++; bytes += (uint32_t)strlen(c->name) + 12; }
        W[i].dirbuf = kmalloc_big(bytes + 24 + cnt * 4 + v->bs * 2);
        if (!W[i].dirbuf) return -1;
        memset(W[i].dirbuf, 0, bytes + 24 + cnt * 4 + v->bs * 2);
        W[i].links = 2;
    }
    for (int i = 0; i < nW; i++) {
        if (W[i].n->type != FS_DIR) continue;
        dir_add(v, &W[i], W[i].ino, ".", ft ? 2 : 0);
        dir_add(v, &W[i], W[W[i].parent].ino, "..", ft ? 2 : 0);
    }
    for (int i = 1; i < nW; i++) {
        wn_t* p = &W[W[i].parent];
        fs_node_t* n = W[i].n;
        uint8_t t = n->type == FS_DIR ? 2 : n->type == FS_LINK ? 7 : 1;
        dir_add(v, p, W[i].ino, n->name, ft ? t : 0);
        if (n->type == FS_DIR) p->links++;
    }
    for (int i = 0; i < nW; i++) {                          /* last entry of each dir fills its block */
        if (W[i].n->type != FS_DIR) continue;
        uint32_t o = W[i].dlast;
        uint32_t end = ((W[i].dirlen + v->bs - 1) / v->bs) * v->bs;
        wr16((uint8_t*)W[i].dirbuf + o + 4, (uint16_t)(end - o));
        W[i].dirlen = end;
    }
    /* blocks */
    cur_blk = v->first_data;
    for (int i = 0; i < nW; i++) {
        fs_node_t* n = W[i].n;
        int r = 0;
        if (n->type == FS_DIR) r = put_data(v, &W[i], W[i].dirbuf, W[i].dirlen);
        else if (n->type == FS_LINK && n->size >= 60) r = put_data(v, &W[i], n->data, (uint32_t)n->size);
        else if (n->type == FS_FILE && n->size) r = put_data(v, &W[i], n->data, (uint32_t)n->size);
        if (r < 0) { klog("ext2: disk full\r\n"); return -1; }
        W[i].links = n->type == FS_DIR ? W[i].links : 1;
    }
    for (int i = 0; i < nW; i++) if (W[i].dirbuf) { kfree(W[i].dirbuf); W[i].dirbuf = NULL; }
    /* inode tables, bitmaps and group descriptors, group by group */
    uint8_t* tab = kmalloc_big(v->itb * v->bs);
    uint8_t* bmp = kmalloc_big(v->bs);
    if (!tab || !bmp) return -1;
    uint32_t free_b = 0, free_i = 0;
    int wi = 0;                                             /* W index walking in ino order */
    for (uint32_t g = 0; g < v->ngroups; g++) {
        uint8_t* gd = v->gdt + g * 32;
        memset(tab, 0, v->itb * v->bs);
        uint32_t i0 = g * v->ipg + 1, dirs = 0, ui = 0;
        for (uint32_t k = 0; k < v->ipg; k++) {
            uint32_t ino = i0 + k;
            uint8_t* e = tab + k * v->isize;
            wn_t* w = NULL;
            if (ino < v->first_ino) {                     /* reserved: as they were, but root is ours */
                memcpy(e, v->resv + (ino - 1) * v->isize, v->isize);
                if (ino != 2) { ui++; continue; }
                w = &W[0];
            } else {
                while (wi < nW && W[wi].ino < ino) wi++;
                if (wi < nW && W[wi].ino == ino) w = &W[wi];
            }
            if (w) {
                {
                    fs_node_t* n = w->n;
                    memset(e, 0, 128);
                    uint16_t ty = n->type == FS_DIR ? 0x4000 : n->type == FS_LINK ? 0xA000 : 0x8000;
                    wr16(e, (uint16_t)(ty | (n->mode & 07777)));
                    uint32_t sz = n->type == FS_DIR ? w->nb * v->bs : (uint32_t)n->size;
                    wr32(e + 4, sz);
                    wr32(e + 8, n->mtime); wr32(e + 12, n->mtime); wr32(e + 16, n->mtime);
                    wr16(e + 26, (uint16_t)w->links);
                    wr32(e + 28, (w->nb + w->nind) * v->spb);
                    if (n->type == FS_LINK && n->size < 60) memcpy(e + 40, n->data, n->size);
                    else for (int b = 0; b < 15; b++) wr32(e + 40 + b * 4, w->blk[b]);
                    if (v->isize > 128) wr16(e + 128, 32);           /* i_extra_isize */
                    if (n->type == FS_DIR) dirs++;
                    ui++;
                }
            }
        }
        uint32_t tb = rd32(gd + 8);
        for (uint32_t b = 0; b < v->itb; b++) wblk(v, tb + b, tab + b * v->bs);
        /* inode bitmap */
        memset(bmp, 0xFF, v->bs);
        for (uint32_t k = 0; k < v->ipg; k++) {
            uint32_t ino = i0 + k;
            bool used = ino < v->first_ino;
            if (!used) {
                /* W is sorted by ino, ino = first_ino + i - 1 */
                uint32_t i = ino - v->first_ino + 1;
                used = i < (uint32_t)nW;
            }
            if (!used) bmp[k >> 3] &= (uint8_t)~(1 << (k & 7));
        }
        wblk(v, rd32(gd + 4), bmp);
        /* block bitmap */
        memset(bmp, 0xFF, v->bs);
        uint32_t b0 = v->first_data + g * v->bpg, fb = 0;
        for (uint32_t k = 0; k < v->bpg; k++) {
            uint32_t b = b0 + k;
            if (b >= v->nblocks) break;
            if (!keep_get(v, b) && b >= cur_blk) { bmp[k >> 3] &= (uint8_t)~(1 << (k & 7)); fb++; }
        }
        wblk(v, rd32(gd), bmp);
        wr16(gd + 12, (uint16_t)fb);
        wr16(gd + 14, (uint16_t)(v->ipg - ui));
        wr16(gd + 16, (uint16_t)dirs);
        wr16(gd + 18, 0);                                   /* bg_flags: no uninit tricks */
        free_b += fb; free_i += v->ipg - ui;
    }
    kfree(tab); kfree(bmp);
    /* group descriptors (primary copy) and the superblock */
    uint32_t gb = v->first_data + 1;
    uint8_t* gbuf = kmalloc_big(v->gdt_blocks * v->bs);
    if (gbuf) {
        memset(gbuf, 0, v->gdt_blocks * v->bs);
        memcpy(gbuf, v->gdt, v->ngroups * 32);
        for (uint32_t b = 0; b < v->gdt_blocks; b++) wblk(v, gb + b, gbuf + b * v->bs);
        kfree(gbuf);
    }
    wr32(v->sb + 12, free_b);
    wr32(v->sb + 16, free_i);
    extern uint32_t fs_now(void);
    wr32(v->sb + 48, fs_now());                             /* s_wtime */
    wr16(v->sb + 58, 1);                                    /* clean */
    ata_write(v->disk, 2, 2, v->sb);
    return 0;
}

/* ---------- mount, sync task ---------- */

void ext2_dirty(int id) {
    for (int i = 0; i < E2_MAX; i++)
        if (vols[i].used && vols[i].id == id && !vols[i].ro) { vols[i].dirty = true; vols[i].dirty_ms = pit_uptime_ms(); }
}

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

int ext2_sync_all(void) {
    lock();
    for (int i = 0; i < E2_MAX; i++)
        if (vols[i].used && vols[i].dirty) { vols[i].dirty = false; sync_vol(&vols[i]); }
    unlock();
    return 0;
}

static void e2syncd(void) {
    for (;;) {
        task_sleep_ms(300);
        for (int i = 0; i < E2_MAX; i++) {
            ev_t* v = &vols[i];
            if (v->used && v->dirty && pit_uptime_ms() - v->dirty_ms > 1000) {
                lock();
                v->dirty = false;
                if (sync_vol(v) < 0) klog("ext2: sync failed\r\n");
                unlock();
            }
        }
    }
}

static bool started;

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
    v->resv_gdt = (v->compat & 0x10) ? rd16(sb + 206) : 0;
    if (v->bs > 4096 || !v->ipg || !v->bpg || v->isize < 128 || v->isize > 1024) return -22;
    if (v->incompat & 0x80) return -22;                     /* 64bit descriptors: not here */
    v->spb = v->bs / 512;
    v->ngroups = (v->nblocks - v->first_data + v->bpg - 1) / v->bpg;
    v->gdt_blocks = (v->ngroups * 32 + v->bs - 1) / v->bs;
    v->itb = (v->ipg * v->isize + v->bs - 1) / v->bs;
    /* write only what we fully understand: filetype and sparse_super, large_file */
    v->ro = (v->incompat & ~2u) || (v->rocompat & ~3u) || (v->compat & 4);
    v->gdt = kmalloc(v->gdt_blocks * v->bs);
    if (!v->gdt) return -12;
    for (uint32_t b = 0; b < v->gdt_blocks; b++)
        if (rblk(v, v->first_data + 1 + b, v->gdt + b * v->bs) < 0) return -5;
    ind_blk[0] = ind_blk[1] = ind_blk[2] = 0;
    if (load_dir(v, at, 2, 0) < 0) return -5;
    if (!v->ro) {
        v->keep = kmalloc_big(v->nblocks / 8 + 1);
        v->bhash = kmalloc_big(v->nblocks * 4);
        v->resv = kmalloc_big(v->first_ino * v->isize);
        if (!v->keep || !v->bhash || !v->resv) v->ro = true;
    }
    if (!v->ro) {
        memset(v->keep, 0, v->nblocks / 8 + 1);
        memset(v->bhash, 0, v->nblocks * 4);
        for (uint32_t b = 0; b <= v->first_data; b++) keep_set(v, b);
        for (uint32_t g = 0; g < v->ngroups; g++) {
            uint32_t base = v->first_data + g * v->bpg;
            if (has_super(v, g)) for (uint32_t b = 0; b < 1 + v->gdt_blocks + v->resv_gdt; b++) keep_set(v, base + b);
            uint8_t* gd = v->gdt + g * 32;
            keep_set(v, rd32(gd)); keep_set(v, rd32(gd + 4));
            for (uint32_t b = 0; b < v->itb; b++) keep_set(v, rd32(gd + 8) + b);
        }
        static uint8_t ib[4096];
        for (uint32_t ino = 1; ino < v->first_ino; ino++) {
            uint8_t* p = inode_ptr(v, ino, ib);
            if (!p) continue;
            memcpy(v->resv + (ino - 1) * v->isize, p, v->isize);
            if (ino != 2) keep_inode(v, v->resv + (ino - 1) * v->isize);
        }
    }
    v->used = true;
    v->id = E2_ID0 + s;
    v->root = at;
    at->mount_id = (uint8_t)v->id;
    if (!started && !v->ro) { started = true; task_spawn("e2sync", e2syncd); }
    return v->ro ? 1 : 0;
}
