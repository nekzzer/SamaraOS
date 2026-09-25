/* FAT12/FAT16 read-only driver. Auto-detects whether sector 0 is an MBR
   (with partition table) or a direct boot sector (BPB), then locates the
   first FAT partition. Designed for QEMU's virtual-FAT (`-drive file=fat:dir`),
   which presents an MBR + FAT16 partition. */

#include "fs/fat.h"
#include "drivers/ata.h"
#include "core/heap.h"
#include "core/string.h"

#define SECTOR 512

static bool          g_mounted = false;
static const char*   g_status = "not mounted";
static int           g_ata_idx = -1;

static uint32_t g_part_lba_start = 0;
static uint16_t g_bps = 512;
static uint8_t  g_spc = 1;
static uint16_t g_reserved = 1;
static uint8_t  g_num_fats = 2;
static uint16_t g_root_ents = 512;
static uint32_t g_total_sec = 0;
static uint32_t g_fat_size_sec = 0;
static bool     g_is_fat32 = false;
static uint32_t g_root_dir_lba = 0;
static uint32_t g_root_dir_sectors = 0;
static uint32_t g_data_start_lba = 0;
static uint32_t g_fat_start_lba = 0;
static uint32_t g_fat32_root_cluster = 0;     /* if FAT32 */

static uint16_t rd_u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_sector(uint32_t lba, void* buf) {
    return ata_read(g_ata_idx, lba, 1, buf);
}

bool fat_mounted(void)         { return g_mounted; }
const char* fat_status(void)   { return g_status; }

/* Returns true if buf looks like a FAT boot sector (sane BPB). */
static bool looks_like_bpb(const uint8_t* sec) {
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
    uint16_t bps = rd_u16(sec + 0x0B);
    uint8_t  spc = sec[0x0D];
    uint8_t  fats = sec[0x10];
    /* sane: bytes/sec = 512, sec/cluster power-of-two 1..128, fats 1 or 2 */
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) return false;
    if (fats != 1 && fats != 2) return false;
    return true;
}

bool fat_mount(int ata_idx) {
    g_mounted = false;
    if (!ata_drive_present(ata_idx)) { g_status = "no such drive"; return false; }
    g_ata_idx = ata_idx;

    uint8_t sec0[SECTOR];
    if (read_sector(0, sec0) != 0) { g_status = "read LBA 0 failed"; return false; }

    uint32_t part_lba = 0;
    if (looks_like_bpb(sec0)) {
        part_lba = 0;
    } else {
        /* try MBR partition table at offset 0x1BE */
        if (sec0[510] != 0x55 || sec0[511] != 0xAA) {
            g_status = "no MBR signature";
            return false;
        }
        for (int i = 0; i < 4; i++) {
            const uint8_t* p = sec0 + 0x1BE + i * 16;
            uint8_t type = p[4];
            uint32_t lba = rd_u32(p + 8);
            if (lba == 0) continue;
            /* FAT12=0x01, FAT16=0x04/0x06/0x0E, FAT32=0x0B/0x0C */
            if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0E ||
                type == 0x0B || type == 0x0C) {
                part_lba = lba;
                break;
            }
        }
        if (part_lba == 0) { g_status = "no FAT partition in MBR"; return false; }
    }
    g_part_lba_start = part_lba;

    /* Read the partition's boot sector */
    uint8_t bs[SECTOR];
    if (read_sector(part_lba, bs) != 0) { g_status = "read BPB failed"; return false; }
    if (!looks_like_bpb(bs))            { g_status = "bad BPB";          return false; }

    g_bps        = rd_u16(bs + 0x0B);
    g_spc        = bs[0x0D];
    g_reserved   = rd_u16(bs + 0x0E);
    g_num_fats   = bs[0x10];
    g_root_ents  = rd_u16(bs + 0x11);

    uint16_t totsec16 = rd_u16(bs + 0x13);
    uint32_t totsec32 = rd_u32(bs + 0x20);
    g_total_sec  = totsec16 ? totsec16 : totsec32;

    uint16_t fatsz16 = rd_u16(bs + 0x16);
    uint32_t fatsz32 = rd_u32(bs + 0x24);
    g_fat_size_sec = fatsz16 ? fatsz16 : fatsz32;
    g_is_fat32   = (fatsz16 == 0);

    g_fat_start_lba    = part_lba + g_reserved;
    g_root_dir_sectors = ((uint32_t)g_root_ents * 32 + g_bps - 1) / g_bps;

    if (g_is_fat32) {
        g_fat32_root_cluster = rd_u32(bs + 0x2C);
        g_data_start_lba = g_fat_start_lba + (uint32_t)g_num_fats * g_fat_size_sec;
    } else {
        g_root_dir_lba   = g_fat_start_lba + (uint32_t)g_num_fats * g_fat_size_sec;
        g_data_start_lba = g_root_dir_lba + g_root_dir_sectors;
    }

    g_mounted = true;
    g_status  = "mounted";
    return true;
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return g_data_start_lba + (cluster - 2) * g_spc;
}

/* Read one FAT entry */
static uint32_t fat_next(uint32_t cluster) {
    static uint8_t  fat_cache[SECTOR];
    static uint32_t cached_lba = 0xFFFFFFFFu;

    uint32_t off, lba;
    if (g_is_fat32) {
        off = cluster * 4;
        lba = g_fat_start_lba + off / g_bps;
        if (lba != cached_lba) {
            if (read_sector(lba, fat_cache) != 0) return 0x0FFFFFFFu;
            cached_lba = lba;
        }
        uint32_t v = rd_u32(fat_cache + (off % g_bps)) & 0x0FFFFFFFu;
        return v;
    }
    /* FAT16 */
    off = cluster * 2;
    lba = g_fat_start_lba + off / g_bps;
    if (lba != cached_lba) {
        if (read_sector(lba, fat_cache) != 0) return 0xFFFFu;
        cached_lba = lba;
    }
    return rd_u16(fat_cache + (off % g_bps));
}

/* Returns true if cluster value is an end-of-chain marker */
static bool is_eoc(uint32_t c) {
    if (g_is_fat32) return (c & 0x0FFFFFFFu) >= 0x0FFFFFF8u;
    return c >= 0xFFF8u;
}

/* Format a directory entry's name as "NAME    EXT" → "NAME.EXT", trimmed,
   uppercased. Returns true if it's a usable entry (regular file). */
static bool format_name(const uint8_t* ent, char out[13]) {
    if (ent[0] == 0x00) return false;       /* end-of-dir */
    if (ent[0] == 0xE5) return false;       /* deleted */
    uint8_t attr = ent[11];
    if (attr == 0x0F) return false;         /* LFN entry */
    if (attr & 0x18)  return false;         /* dir or volume label */

    int o = 0;
    for (int i = 0; i < 8; i++) {
        char c = (char)ent[i];
        if (c == ' ') break;
        out[o++] = c;
    }
    if (ent[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11; i++) {
            char c = (char)ent[i];
            if (c == ' ') break;
            out[o++] = c;
        }
    }
    out[o] = 0;
    return true;
}

static uint8_t up(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
}
static bool name_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (up((uint8_t)*a) != up((uint8_t)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* Iterate root directory (FAT16). For FAT32, root is a cluster chain. */
static void walk_root_fat16(void (*cb)(const uint8_t* ent, void* user), void* user) {
    uint8_t sec[SECTOR];
    for (uint32_t s = 0; s < g_root_dir_sectors; s++) {
        if (read_sector(g_root_dir_lba + s, sec) != 0) return;
        for (uint32_t e = 0; e < g_bps; e += 32) {
            if (sec[e] == 0x00) return;     /* end */
            cb(sec + e, user);
        }
    }
}

static void walk_root_fat32(void (*cb)(const uint8_t* ent, void* user), void* user) {
    uint32_t cl = g_fat32_root_cluster;
    uint8_t sec[SECTOR];
    while (cl >= 2 && !is_eoc(cl)) {
        uint32_t lba = cluster_to_lba(cl);
        for (uint32_t s = 0; s < g_spc; s++) {
            if (read_sector(lba + s, sec) != 0) return;
            for (uint32_t e = 0; e < g_bps; e += 32) {
                if (sec[e] == 0x00) return;
                cb(sec + e, user);
            }
        }
        cl = fat_next(cl);
    }
}

static void walk_root(void (*cb)(const uint8_t* ent, void* user), void* user) {
    if (g_is_fat32) walk_root_fat32(cb, user);
    else            walk_root_fat16(cb, user);
}

/* fat_list helpers */
typedef struct {
    void (*user_cb)(const char* name, uint32_t size, void* user);
    void* user_ud;
} list_ctx_t;

static void list_visit(const uint8_t* ent, void* ud) {
    list_ctx_t* c = (list_ctx_t*)ud;
    char name[13];
    if (!format_name(ent, name)) return;
    uint32_t sz = rd_u32(ent + 28);
    c->user_cb(name, sz, c->user_ud);
}

void fat_list(void (*cb)(const char* name, uint32_t size, void* user), void* user) {
    if (!g_mounted) return;
    list_ctx_t ctx = { cb, user };
    walk_root(list_visit, &ctx);
}

/* find_file helpers */
typedef struct {
    const char* want;
    uint32_t first_cluster;
    uint32_t size;
    bool found;
} find_ctx_t;

static void find_visit(const uint8_t* ent, void* ud) {
    find_ctx_t* c = (find_ctx_t*)ud;
    if (c->found) return;
    char name[13];
    if (!format_name(ent, name)) return;
    if (!name_eq_ci(name, c->want)) return;
    uint16_t hi = rd_u16(ent + 20);
    uint16_t lo = rd_u16(ent + 26);
    c->first_cluster = ((uint32_t)hi << 16) | lo;
    c->size  = rd_u32(ent + 28);
    c->found = true;
}

int fat_read_file(const char* name, uint8_t** out_buf, uint32_t* out_size) {
    if (!g_mounted || !name || !out_buf || !out_size) return -1;
    find_ctx_t fc = { name, 0, 0, false };
    walk_root(find_visit, &fc);
    if (!fc.found) return -2;

    uint8_t* buf = (uint8_t*)kmalloc(fc.size ? fc.size : 1);
    if (!buf) return -3;

    uint32_t bytes_left = fc.size;
    uint32_t off = 0;
    uint32_t cl = fc.first_cluster;
    uint32_t cluster_bytes = (uint32_t)g_spc * g_bps;
    uint8_t  sec[SECTOR];

    while (bytes_left > 0 && cl >= 2 && !is_eoc(cl)) {
        uint32_t lba = cluster_to_lba(cl);
        for (uint32_t s = 0; s < g_spc && bytes_left > 0; s++) {
            if (read_sector(lba + s, sec) != 0) { kfree(buf); return -4; }
            uint32_t take = bytes_left < g_bps ? bytes_left : g_bps;
            memcpy(buf + off, sec, take);
            off += take;
            bytes_left -= take;
        }
        cl = fat_next(cl);
        (void)cluster_bytes;
    }

    *out_buf = buf;
    *out_size = fc.size;
    return 0;
}
