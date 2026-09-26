#include "fs/fatfs.h"
#include "drivers/ata.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/io.h"
#include "boot/pit.h"

#define ENOENT  2
#define EIO     5
#define ENOMEM  12
#define EBUSY   16
#define ENOTDIR 20
#define EINVAL  22
#define ENOSPC  28

#define ATTR_RO    0x01
#define ATTR_VOL   0x08
#define ATTR_DIR   0x10
#define ATTR_ARCH  0x20
#define ATTR_LFN   0x0F

typedef struct {
    bool       used;
    int        id;                 /* 1..FATFS_MAX_MOUNTS, stored in root->mount_id */
    int        disk;
    fs_node_t* root;
    bool       fat32;
    uint32_t   spc, csize, reserved, nfats, fatsz, root_entries;
    uint32_t   root_lba, root_secs, data_lba, nclus;
    uint32_t   fsinfo, backup_boot;
    uint8_t    boot[512];
    uint8_t*   fat;                /* in-memory FAT (as loaded, then as last written) */
    uint32_t*  clus_hash;          /* per cluster: hash of what we last wrote, 0 = unknown */
    uint32_t*  fat_hash;           /* per FAT sector */
    uint32_t*  root_hash;          /* per FAT16 root-dir sector */
    volatile bool dirty;
    uint32_t   dirty_ms;
} vol_t;

static vol_t vols[FATFS_MAX_MOUNTS];

static void klog(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}

/* ---------------- time ---------------- */

static uint32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return (uint32_t)(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468);
}

static void civil_from_days(uint32_t z0, int* y, int* m, int* d) {
    int z = (int)z0 + 719468;
    int era = z / 146097, doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static uint32_t fat_to_epoch(uint16_t date, uint16_t time) {
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1) return 0;
    return days_from_civil(y, m, d) * 86400u + (uint32_t)((time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2);
}

static void epoch_to_fat(uint32_t t, uint16_t* date, uint16_t* time) {
    int y, m, d;
    civil_from_days(t / 86400u, &y, &m, &d);
    if (y < 1980) { *date = (1 << 5) | 1; *time = 0; return; }
    uint32_t s = t % 86400u;
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)(((s / 3600) << 11) | (((s / 60) % 60) << 5) | ((s % 60) / 2));
}

/* ---------------- geometry ---------------- */

static int parse_bpb(vol_t* v, const uint8_t* b) {
    if (b[510] != 0x55 || b[511] != 0xAA) return -EINVAL;
    uint32_t bps = rd16(b + 11);
    v->spc = b[13];
    v->reserved = rd16(b + 14);
    v->nfats = b[16];
    v->root_entries = rd16(b + 17);
    uint32_t total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    uint32_t fat16sz = rd16(b + 22);
    if (bps != 512 || !v->spc || (v->spc & (v->spc - 1)) || !v->nfats || !v->reserved || !total)
        return -EINVAL;
    v->fat32 = fat16sz == 0;
    v->fatsz = v->fat32 ? rd32(b + 36) : fat16sz;
    v->root_secs = (v->root_entries * 32 + 511) / 512;
    v->root_lba = v->reserved + v->nfats * v->fatsz;
    v->data_lba = v->root_lba + v->root_secs;
    if (total <= v->data_lba) return -EINVAL;
    v->nclus = (total - v->data_lba) / v->spc;
    v->csize = v->spc * 512;
    uint32_t max_entries = v->fatsz * 512 / (v->fat32 ? 4 : 2);
    if (v->nclus + 2 > max_entries) v->nclus = max_entries - 2;
    if (!v->fat32 && v->nclus < 4085) return -EINVAL;              /* FAT12: not supported */
    v->fsinfo = v->fat32 ? rd16(b + 48) : 0;
    v->backup_boot = v->fat32 ? rd16(b + 50) : 0;
    return 0;
}

bool fatfs_probe(int disk) {
    uint8_t b[512];
    if (!ata_drive_present(disk) || ata_read(disk, 0, 1, b) < 0) return false;
    vol_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    return parse_bpb(&tmp, b) == 0;
}

static uint32_t clus_lba(vol_t* v, uint32_t c) { return v->data_lba + (c - 2) * v->spc; }

static uint32_t fat_get(vol_t* v, uint32_t c) {
    if (v->fat32) return ((uint32_t*)v->fat)[c] & 0x0FFFFFFF;
    return ((uint16_t*)v->fat)[c];
}

static bool is_eoc(vol_t* v, uint32_t c) {
    return v->fat32 ? c >= 0x0FFFFFF8 : c >= 0xFFF8;
}

/* ---------------- loading ---------------- */

/* Read a cluster chain (at most `max` bytes, 0 = whole chain). */
static uint8_t* read_chain(vol_t* v, uint32_t first, uint32_t max, uint32_t* out_len) {
    uint32_t n = 0;
    for (uint32_t c = first; c >= 2 && c < v->nclus + 2 && n < 1u << 20; c = fat_get(v, c)) {
        n++;
        if (max && n * v->csize >= max) break;
        uint32_t nx = fat_get(v, c);
        if (is_eoc(v, nx) || nx < 2) break;
    }
    uint32_t bytes = n * v->csize;
    uint8_t* buf = (uint8_t*)kmalloc(bytes ? bytes : 1);
    if (!buf) return NULL;
    uint32_t i = 0;
    for (uint32_t c = first; i < n; i++) {
        if (ata_read(v->disk, clus_lba(v, c), (int)v->spc, buf + i * v->csize) < 0) { kfree(buf); return NULL; }
        c = fat_get(v, c);
    }
    *out_len = bytes;
    return buf;
}

static bool short_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'()-@^_`{}~", c);
}

static int load_dir(vol_t* v, fs_node_t* dir, const uint8_t* ents, uint32_t bytes, int depth);

static int load_entry(vol_t* v, fs_node_t* dir, const char* name, const uint8_t* e, int depth) {
    uint8_t attr = e[11];
    uint32_t first = (uint32_t)rd16(e + 26) | (v->fat32 ? (uint32_t)rd16(e + 20) << 16 : 0);
    uint32_t size = rd32(e + 28);
    if (strlen(name) >= FS_NAME_MAX) { klog("[fatfs] skipping long name: "); klog(name); klog("\r\n"); return 0; }
    if (fs_child(dir, name)) return 0;
    fs_node_t* n = fs_create(dir, name, (attr & ATTR_DIR) ? FS_DIR : FS_FILE);
    if (!n) return -ENOMEM;
    n->mode = (attr & ATTR_DIR) ? 0755 : (attr & ATTR_RO) ? 0555 : 0755;
    n->mtime = fat_to_epoch(rd16(e + 24), rd16(e + 22));
    if (attr & ATTR_DIR) {
        if (depth > 32 || first < 2) return 0;
        uint32_t len;
        uint8_t* d = read_chain(v, first, 0, &len);
        if (!d) return -EIO;
        int r = load_dir(v, n, d, len, depth + 1);
        kfree(d);
        return r;
    }
    if (size && first >= 2) {
        uint32_t len;
        uint8_t* d = read_chain(v, first, size, &len);
        if (!d) return -ENOMEM;
        n->data = (char*)kmalloc_big(size + 1);
        if (!n->data) { kfree(d); return -ENOMEM; }
        memcpy(n->data, d, size < len ? size : len);
        kfree(d);
        n->size = size < len ? size : len;
        n->cap = size + 1;
        n->data[n->size] = 0;
    }
    return 0;
}

static int load_dir(vol_t* v, fs_node_t* dir, const uint8_t* ents, uint32_t bytes, int depth) {
    char lfn[264];
    int lfn_len = 0;
    uint8_t lfn_sum = 0;
    for (uint32_t off = 0; off + 32 <= bytes; off += 32) {
        const uint8_t* e = ents + off;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { lfn_len = 0; continue; }
        if (e[11] == ATTR_LFN) {
            int ord = e[0] & 0x3F;
            if (ord < 1 || ord > 20) { lfn_len = 0; continue; }
            if (e[0] & 0x40) { memset(lfn, 0, sizeof(lfn)); lfn_len = ord * 13; lfn_sum = e[13]; }
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) {
                uint16_t ch = rd16(e + pos[k]);
                int idx = (ord - 1) * 13 + k;
                if (idx >= 263) continue;
                lfn[idx] = ch == 0 || ch == 0xFFFF ? 0 : ch < 0x80 ? (char)ch : '_';
            }
            continue;
        }
        if (e[11] & ATTR_VOL) { lfn_len = 0; continue; }
        if (e[0] == '.') { lfn_len = 0; continue; }
        char name[264];
        uint8_t sum = 0;
        for (int k = 0; k < 11; k++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + e[k]);
        if (lfn_len && sum == lfn_sum && lfn[0]) {
            strncpy(name, lfn, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        } else {
            int n = 0;
            bool low_base = e[12] & 0x08, low_ext = e[12] & 0x10;
            for (int k = 0; k < 8 && e[k] != ' '; k++) {
                char c = (char)(k == 0 && e[k] == 0x05 ? 0xE5 : e[k]);
                name[n++] = (low_base && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            if (e[8] != ' ') {
                name[n++] = '.';
                for (int k = 8; k < 11 && e[k] != ' '; k++)
                    name[n++] = (low_ext && e[k] >= 'A' && e[k] <= 'Z') ? (char)(e[k] + 32) : (char)e[k];
            }
            name[n] = 0;
        }
        lfn_len = 0;
        int r = load_entry(v, dir, name, e, depth);
        if (r < 0) return r;
    }
    return 0;
}

/* ---------------- writing: name encoding ---------------- */

/* Fits 8.3 as-is (per part all-upper or all-lower)? Fills the 11-byte name
   and the NT case byte. */
static bool as_short(const char* name, uint8_t out[11], uint8_t* nt) {
    int len = (int)strlen(name);
    if (!len || name[0] == '.' || len > 12) return false;
    const char* dot = NULL;
    for (const char* p = name; *p; p++) if (*p == '.') { if (dot) return false; dot = p; }
    int blen = dot ? (int)(dot - name) : len, elen = dot ? len - blen - 1 : 0;
    if (blen < 1 || blen > 8 || elen > 3 || (dot && elen == 0)) return false;
    memset(out, ' ', 11);
    *nt = 0;
    for (int part = 0; part < 2; part++) {
        const char* s = part ? dot + 1 : name;
        int n = part ? elen : blen;
        bool up = false, low = false;
        for (int i = 0; i < n; i++) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') { low = true; c = (char)(c - 32); }
            else if (c >= 'A' && c <= 'Z') up = true;
            if (!short_char_ok(c)) return false;
            out[(part ? 8 : 0) + i] = (uint8_t)c;
        }
        if (up && low) return false;
        if (low) *nt |= part ? 0x10 : 0x08;
    }
    return true;
}

static int lfn_slots(const char* name) {
    uint8_t sn[11], nt;
    if (as_short(name, sn, &nt)) return 0;
    return ((int)strlen(name) + 12) / 13;
}

static uint32_t dir_bytes(fs_node_t* d, bool is_root) {
    uint32_t n = is_root ? 0 : 2;
    for (fs_node_t* c = d->child; c; c = c->next) {
        if (c->dev) continue;
        n += 1 + (uint32_t)lfn_slots(c->name);
    }
    return n * 32;
}

/* ---------------- writing: layout ---------------- */

typedef struct { fs_node_t* n; uint32_t first, count; } place_t;

typedef struct {
    vol_t*   v;
    place_t* pl;
    uint32_t npl, cap;
    uint32_t* map;                 /* open-addressed node -> place index + 1 */
    uint32_t mapcap;
    uint32_t next_clus;
    int      err;
} layout_t;

static uint32_t hash_ptr(const void* p, uint32_t cap) { return (((uint32_t)p >> 3) * 2654435761u) & (cap - 1); }

static place_t* find_place(layout_t* L, fs_node_t* n) {
    for (uint32_t h = hash_ptr(n, L->mapcap);; h = (h + 1) & (L->mapcap - 1)) {
        uint32_t i = L->map[h];
        if (!i) return NULL;
        if (L->pl[i - 1].n == n) return &L->pl[i - 1];
    }
}

static uint32_t count_nodes(fs_node_t* d) {
    uint32_t n = 1;
    for (fs_node_t* c = d->child; c; c = c->next) n += c->type == FS_DIR ? count_nodes(c) : 1;
    return n;
}

static void assign(layout_t* L, fs_node_t* n, bool is_root) {
    if (L->err) return;
    vol_t* v = L->v;
    uint32_t count = 0;
    if (n->type == FS_DIR) {
        uint32_t b = dir_bytes(n, is_root);
        if (is_root && !v->fat32) {
            if (b > v->root_entries * 32) { L->err = -ENOSPC; return; }
        } else {
            count = (b + v->csize - 1) / v->csize;
            if (!count) count = 1;
        }
    } else if (!n->dev) {
        count = (uint32_t)((n->size + v->csize - 1) / v->csize);
    }
    if (L->next_clus + count > v->nclus + 2) { L->err = -ENOSPC; return; }
    place_t* p = &L->pl[L->npl++];
    p->n = n;
    p->first = count ? L->next_clus : 0;
    p->count = count;
    L->next_clus += count;
    for (uint32_t h = hash_ptr(n, L->mapcap);; h = (h + 1) & (L->mapcap - 1))
        if (!L->map[h]) { L->map[h] = L->npl; break; }
    if (n->type == FS_DIR)
        for (fs_node_t* c = n->child; c; c = c->next) if (!c->dev) assign(L, c, false);
}

/* ---------------- writing: output ---------------- */

static int write_clusters(vol_t* v, uint32_t first, uint32_t count, const uint8_t* data, uint32_t len,
                          uint8_t* scratch) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = first + i, off = i * v->csize;
        uint32_t n = len > off ? len - off : 0;
        if (n > v->csize) n = v->csize;
        const uint8_t* src = data + off;
        if (n < v->csize) {
            memset(scratch, 0, v->csize);
            if (n) memcpy(scratch, data + off, n);
            src = scratch;
        }
        uint32_t h = fnv(src, v->csize);
        if (v->clus_hash[c] == h) continue;
        if (ata_write(v->disk, clus_lba(v, c), (int)v->spc, src) < 0) return -EIO;
        v->clus_hash[c] = h;
    }
    return 0;
}

static void put_dirent(uint8_t* e, const uint8_t sn[11], uint8_t nt, uint8_t attr,
                       uint32_t first, uint32_t size, uint32_t mtime, bool fat32) {
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = attr;
    e[12] = nt;
    uint16_t d, t;
    epoch_to_fat(mtime, &d, &t);
    wr16(e + 14, t); wr16(e + 16, d); wr16(e + 18, d);
    wr16(e + 22, t); wr16(e + 24, d);
    wr16(e + 20, fat32 ? (uint16_t)(first >> 16) : 0);
    wr16(e + 26, (uint16_t)first);
    wr32(e + 28, size);
}

static void make_alias(const char* name, uint8_t sn[11], uint8_t (*used)[11], int nused) {
    memset(sn, ' ', 11);
    const char* dot = NULL;
    for (const char* p = name; *p; p++) if (*p == '.' && p != name) dot = p;
    int b = 0;
    for (const char* p = name; *p && p != dot && b < 6; p++) {
        char c = *p;
        if (c == '.' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        sn[b++] = short_char_ok(c) ? (uint8_t)c : '_';
    }
    if (!b) sn[b++] = '_';
    if (dot) {
        int x = 0;
        for (const char* p = dot + 1; *p && x < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            sn[8 + x++] = short_char_ok(c) ? (uint8_t)c : '_';
        }
    }
    for (int num = 1; num < 1000000; num++) {
        char tail[8];
        itoa(num, tail + 1, 10);
        tail[0] = '~';
        int tl = (int)strlen(tail);
        int keep = b + tl > 8 ? 8 - tl : b;
        for (int i = keep; i < 8; i++) sn[i] = ' ';
        memcpy(sn + keep, tail, (uint32_t)tl);
        bool clash = false;
        for (int i = 0; i < nused && !clash; i++) clash = memcmp(used[i], sn, 11) == 0;
        if (!clash) return;
    }
}

static int emit_dir(layout_t* L, fs_node_t* d, place_t* self, uint32_t parent_first,
                    bool is_root, uint8_t* scratch) {
    vol_t* v = L->v;
    uint32_t bytes = is_root && !v->fat32 ? v->root_secs * 512 : self->count * v->csize;
    uint8_t* buf = (uint8_t*)kmalloc(bytes ? bytes : 32);
    int nchild = 0;
    for (fs_node_t* c = d->child; c; c = c->next) nchild++;
    uint8_t (*used)[11] = (uint8_t (*)[11])kmalloc((uint32_t)(nchild + 1) * 11);
    if (!buf || !used) { if (buf) kfree(buf); if (used) kfree(used); return -ENOMEM; }
    memset(buf, 0, bytes);
    uint32_t off = 0;
    int nused = 0;
    if (!is_root) {
        uint8_t dot[11], dotdot[11];
        memset(dot, ' ', 11); dot[0] = '.';
        memset(dotdot, ' ', 11); dotdot[0] = '.'; dotdot[1] = '.';
        put_dirent(buf, dot, 0, ATTR_DIR, self->first, 0, d->mtime, v->fat32);
        put_dirent(buf + 32, dotdot, 0, ATTR_DIR, parent_first, 0, d->mtime, v->fat32);
        off = 64;
    }
    /* Pass A: real 8.3 names reserve their short names first. */
    for (fs_node_t* c = d->child; c; c = c->next) {
        uint8_t sn[11], nt;
        if (!c->dev && as_short(c->name, sn, &nt)) memcpy(used[nused++], sn, 11);
    }
    for (fs_node_t* c = d->child; c; c = c->next) {
        if (c->dev) continue;
        place_t* p = find_place(L, c);
        uint8_t sn[11], nt = 0;
        int nl = 0;
        if (!as_short(c->name, sn, &nt)) {
            make_alias(c->name, sn, used, nused);
            memcpy(used[nused++], sn, 11);
            nt = 0;
            int len = (int)strlen(c->name);
            nl = (len + 12) / 13;
            uint8_t sum = 0;
            for (int k = 0; k < 11; k++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[k]);
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = nl; k >= 1; k--) {
                uint8_t* e = buf + off;
                memset(e, 0, 32);
                e[0] = (uint8_t)(k | (k == nl ? 0x40 : 0));
                e[11] = ATTR_LFN;
                e[13] = sum;
                for (int j = 0; j < 13; j++) {
                    int idx = (k - 1) * 13 + j;
                    uint16_t ch = idx < len ? (uint8_t)c->name[idx] : idx == len ? 0 : 0xFFFF;
                    wr16(e + pos[j], ch);
                }
                off += 32;
            }
        }
        bool dir = c->type == FS_DIR;
        uint8_t attr = dir ? ATTR_DIR : (uint8_t)(ATTR_ARCH | ((c->mode & 0222) ? 0 : ATTR_RO));
        put_dirent(buf + off, sn, nt, attr, p ? p->first : 0, dir ? 0 : (uint32_t)c->size, c->mtime, v->fat32);
        off += 32;
    }
    int r = 0;
    if (is_root && !v->fat32) {
        for (uint32_t s = 0; s < v->root_secs && r == 0; s++) {
            uint32_t h = fnv(buf + s * 512, 512);
            if (v->root_hash[s] == h) continue;
            if (ata_write(v->disk, v->root_lba + s, 1, buf + s * 512) < 0) r = -EIO;
            else v->root_hash[s] = h;
        }
    } else {
        r = write_clusters(v, self->first, self->count, buf, bytes, scratch);
    }
    kfree(used);
    kfree(buf);
    if (r < 0) return r;

    for (fs_node_t* c = d->child; c; c = c->next) {
        if (c->dev) continue;
        place_t* p = find_place(L, c);
        if (!p) continue;
        if (c->type == FS_DIR) {
            r = emit_dir(L, c, p, is_root ? 0 : self->first, false, scratch);
        } else if (p->count) {
            r = write_clusters(v, p->first, p->count, (const uint8_t*)c->data, (uint32_t)c->size, scratch);
        }
        if (r < 0) return r;
    }
    return 0;
}

static int sync_vol(vol_t* v) {
    layout_t L;
    memset(&L, 0, sizeof(L));
    L.v = v;
    uint32_t nodes = count_nodes(v->root);
    L.cap = nodes;
    L.mapcap = 16;
    while (L.mapcap < nodes * 2) L.mapcap <<= 1;
    L.pl = (place_t*)kmalloc(nodes * sizeof(place_t));
    L.map = (uint32_t*)kmalloc(L.mapcap * 4);
    uint8_t* scratch = (uint8_t*)kmalloc(v->csize);
    uint8_t* fat = (uint8_t*)kmalloc(v->fatsz * 512);
    int r = 0;
    if (!L.pl || !L.map || !scratch || !fat) { r = -ENOMEM; goto out; }
    memset(L.map, 0, L.mapcap * 4);
    L.next_clus = 2;
    assign(&L, v->root, true);
    if (L.err) { r = L.err; goto out; }

    /* FAT: contiguous chains in allocation order. */
    memset(fat, 0, v->fatsz * 512);
    uint8_t media = v->boot[21];
    if (v->fat32) {
        uint32_t* f = (uint32_t*)fat;
        f[0] = 0x0FFFFF00u | media; f[1] = 0x0FFFFFFFu;
        for (uint32_t i = 0; i < L.npl; i++)
            for (uint32_t k = 0; k < L.pl[i].count; k++)
                f[L.pl[i].first + k] = k + 1 < L.pl[i].count ? L.pl[i].first + k + 1 : 0x0FFFFFFFu;
    } else {
        uint16_t* f = (uint16_t*)fat;
        f[0] = (uint16_t)(0xFF00u | media); f[1] = 0xFFFF;
        for (uint32_t i = 0; i < L.npl; i++)
            for (uint32_t k = 0; k < L.pl[i].count; k++)
                f[L.pl[i].first + k] = (uint16_t)(k + 1 < L.pl[i].count ? L.pl[i].first + k + 1 : 0xFFFF);
    }

    place_t* rootp = find_place(&L, v->root);
    if (v->fat32 && rd32(v->boot + 44) != rootp->first) {
        wr32(v->boot + 44, rootp->first);
        if (ata_write(v->disk, 0, 1, v->boot) < 0) { r = -EIO; goto out; }
        if (v->backup_boot && v->backup_boot < v->reserved) ata_write(v->disk, v->backup_boot, 1, v->boot);
    }

    r = emit_dir(&L, v->root, rootp, 0, true, scratch);
    if (r < 0) goto out;

    for (uint32_t s = 0; s < v->fatsz; s++) {
        uint32_t h = fnv(fat + s * 512, 512);
        if (v->fat_hash[s] == h) continue;
        for (uint32_t k = 0; k < v->nfats; k++)
            if (ata_write(v->disk, v->reserved + k * v->fatsz + s, 1, fat + s * 512) < 0) { r = -EIO; goto out; }
        v->fat_hash[s] = h;
    }
    memcpy(v->fat, fat, v->fatsz * 512);

    if (v->fat32 && v->fsinfo && v->fsinfo < v->reserved) {
        uint8_t fi[512];
        if (ata_read(v->disk, v->fsinfo, 1, fi) == 0 && rd32(fi) == 0x41615252u) {
            wr32(fi + 488, v->nclus + 2 - L.next_clus);
            wr32(fi + 492, L.next_clus);
            ata_write(v->disk, v->fsinfo, 1, fi);
        }
    }
out:
    if (L.pl) kfree(L.pl);
    if (L.map) kfree(L.map);
    if (scratch) kfree(scratch);
    if (fat) kfree(fat);
    return r;
}

/* ---------------- mount / sync task ---------------- */

static void mark_dirty(int id) {
    if (id < 1 || id > FATFS_MAX_MOUNTS || !vols[id - 1].used) return;
    vols[id - 1].dirty = true;
    vols[id - 1].dirty_ms = pit_uptime_ms();
}

static int sync_one(vol_t* v) {
    uint32_t f = irq_save();
    v->dirty = false;
    int r = sync_vol(v);
    if (r < 0) {
        v->dirty = true;
        v->dirty_ms = pit_uptime_ms() + 5000;         /* back off after an error */
        klog("[fatfs] sync failed\r\n");
    }
    irq_restore(f);
    return r;
}

int fatfs_sync_all(void) {
    int r = 0;
    for (int i = 0; i < FATFS_MAX_MOUNTS; i++)
        if (vols[i].used && vols[i].dirty) { int e = sync_one(&vols[i]); if (e < 0) r = e; }
    return r;
}

static void syncd(void) {
    for (;;) {
        task_sleep_ms(250);
        for (int i = 0; i < FATFS_MAX_MOUNTS; i++) {
            vol_t* v = &vols[i];
            if (v->used && v->dirty && pit_uptime_ms() - v->dirty_ms >= 800) sync_one(v);
        }
    }
}

void fatfs_init(void) {
    fs_dirty_hook = mark_dirty;
    task_spawn("fatsync", syncd);
}

static void free_tree(fs_node_t* n) {
    while (n->child) {
        fs_node_t* c = n->child;
        free_tree(c);
        fs_detach(c);
        if (c->refs > 0) c->unlinked = true;
        else { fs_data_free(c); kfree(c); }
    }
}

int fatfs_mount(int disk, fs_node_t* at) {
    if (!at || at->type != FS_DIR) return -ENOTDIR;
    if (at->child || at->mount_id) return -EBUSY;
    for (int i = 0; i < FATFS_MAX_MOUNTS; i++) if (vols[i].used && vols[i].disk == disk) return -EBUSY;
    vol_t* v = NULL;
    for (int i = 0; i < FATFS_MAX_MOUNTS; i++) if (!vols[i].used) { v = &vols[i]; v->id = i + 1; break; }
    if (!v) return -EBUSY;
    int id = v->id;
    memset(v, 0, sizeof(*v));
    v->id = id;
    v->disk = disk;
    if (!ata_drive_present(disk) || ata_read(disk, 0, 1, v->boot) < 0) return -EIO;
    int r = parse_bpb(v, v->boot);
    if (r < 0) return r;
    v->fat = (uint8_t*)kmalloc(v->fatsz * 512);
    v->clus_hash = (uint32_t*)kmalloc((v->nclus + 2) * 4);
    v->fat_hash = (uint32_t*)kmalloc(v->fatsz * 4);
    v->root_hash = (uint32_t*)kmalloc((v->root_secs + 1) * 4);
    if (!v->fat || !v->clus_hash || !v->fat_hash || !v->root_hash) { r = -ENOMEM; goto fail; }
    memset(v->clus_hash, 0, (v->nclus + 2) * 4);
    memset(v->fat_hash, 0, v->fatsz * 4);
    memset(v->root_hash, 0, (v->root_secs + 1) * 4);
    if (ata_read(disk, v->reserved, (int)v->fatsz, v->fat) < 0) { r = -EIO; goto fail; }

    uint32_t len;
    uint8_t* rootd;
    if (v->fat32) {
        rootd = read_chain(v, rd32(v->boot + 44), 0, &len);
    } else {
        len = v->root_secs * 512;
        rootd = (uint8_t*)kmalloc(len);
        if (rootd && ata_read(disk, v->root_lba, (int)v->root_secs, rootd) < 0) { kfree(rootd); rootd = NULL; }
    }
    if (!rootd) { r = -EIO; goto fail; }
    r = load_dir(v, at, rootd, len, 0);
    kfree(rootd);
    if (r < 0) { free_tree(at); goto fail; }

    v->root = at;
    v->used = true;
    v->dirty = false;
    at->mount_id = (uint8_t)v->id;
    return 0;
fail:
    if (v->fat) kfree(v->fat);
    if (v->clus_hash) kfree(v->clus_hash);
    if (v->fat_hash) kfree(v->fat_hash);
    if (v->root_hash) kfree(v->root_hash);
    memset(v, 0, sizeof(*v));
    return r;
}

int fatfs_umount(fs_node_t* at) {
    if (!at || !at->mount_id) return -EINVAL;
    vol_t* v = &vols[at->mount_id - 1];
    if (v->dirty) {
        int r = sync_one(v);
        if (r < 0) return r;
    }
    at->mount_id = 0;
    free_tree(at);
    kfree(v->fat); kfree(v->clus_hash); kfree(v->fat_hash); kfree(v->root_hash);
    memset(v, 0, sizeof(*v));
    return 0;
}

int fatfs_mounts_text(char* out, int cap) {
    int n = 0;
    for (int i = 0; i < FATFS_MAX_MOUNTS; i++) {
        vol_t* v = &vols[i];
        if (!v->used) continue;
        char path[256], line[320];
        fs_path(v->root, path, sizeof(path));
        strcpy(line, "/dev/");
        strcat(line, ata_drive_name(v->disk));
        strcat(line, " ");
        strcat(line, path);
        strcat(line, " vfat rw 0 0\n");
        int l = (int)strlen(line);
        if (n + l >= cap) break;
        memcpy(out + n, line, (uint32_t)l);
        n += l;
    }
    out[n] = 0;
    return n;
}

int fatfs_owner(fs_node_t* n) {
    for (; n; n = n->parent) if (n->mount_id) return n->mount_id;
    return 0;
}

bool fatfs_statfs(int id, uint32_t* csize, uint32_t* total, uint32_t* free_clus) {
    if (id < 1 || id > FATFS_MAX_MOUNTS || !vols[id - 1].used) return false;
    vol_t* v = &vols[id - 1];
    uint32_t used = 0;
    for (uint32_t c = 2; c < v->nclus + 2; c++) if (fat_get(v, c)) used++;
    *csize = v->csize;
    *total = v->nclus;
    *free_clus = v->nclus - used;
    return true;
}
