#include "drivers/ata.h"
#include "core/heap.h"
#include "core/string.h"

#define NPART (DISK_ALL - DISK_PART_BASE)

static struct { int disk; uint32_t start, len; int nr; char name[12]; } pt[NPART];

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void add(int disk, int nr, uint32_t start, uint32_t len) {
    if (!len || start >= ata_drive_sectors(disk) || start + len > ata_drive_sectors(disk)) return;
    for (int i = 0; i < NPART; i++) {
        if (pt[i].len) continue;
        pt[i].disk = disk; pt[i].start = start; pt[i].len = len; pt[i].nr = nr;
        const char* b = ata_drive_name(disk);
        int n = strlen(b);
        memcpy(pt[i].name, b, n);
        if (b[n - 1] >= '0' && b[n - 1] <= '9') pt[i].name[n++] = 'p';
        if (nr >= 10) pt[i].name[n++] = '0' + nr / 10;
        pt[i].name[n++] = '0' + nr % 10;
        pt[i].name[n] = 0;
        return;
    }
}

void part_scan(int disk) {
    for (int i = 0; i < NPART; i++) if (pt[i].len && pt[i].disk == disk) pt[i].len = 0;
    uint8_t* s = kmalloc(512 * 32);
    if (!s) return;
    if (ata_read(disk, 0, 1, s) < 0 || s[510] != 0x55 || s[511] != 0xAA) goto out;
    if (s[11] == 0 && s[12] == 2 && s[13] && (!memcmp(s + 82, "FAT", 3) || !memcmp(s + 54, "FAT", 3))) goto out;
    int gpt = 0;
    uint32_t ext = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t* e = s + 446 + i * 16;
        if (e[0] != 0 && e[0] != 0x80) goto out;
        if (e[4] == 0xEE) gpt = 1;
        else if (e[4] == 5 || e[4] == 0xF || e[4] == 0x85) ext = le32(e + 8);
        else if (e[4]) add(disk, i + 1, le32(e + 8), le32(e + 12));
    }
    if (gpt) {
        if (ata_read(disk, 1, 1, s) < 0 || memcmp(s, "EFI PART", 8)) goto out;
        uint32_t lba = le32(s + 72), cnt = le32(s + 80), sz = le32(s + 84);
        if (sz < 128 || sz > 512 || cnt > 128) goto out;
        int secs = (cnt * sz + 511) / 512;
        if (secs > 32 || ata_read(disk, lba, secs, s) < 0) goto out;
        for (uint32_t i = 0; i < cnt; i++) {
            uint8_t* e = s + i * sz;
            int z = 1;
            for (int k = 0; k < 16; k++) if (e[k]) z = 0;
            if (!z) add(disk, i + 1, le32(e + 32), le32(e + 40) - le32(e + 32) + 1);
        }
    } else if (ext) {
        uint32_t cur = ext;
        for (int nr = 5; nr < 13 && cur; nr++) {
            if (ata_read(disk, cur, 1, s) < 0 || s[510] != 0x55) break;
            uint8_t* e = s + 446;
            if (e[4]) add(disk, nr, cur + le32(e + 8), le32(e + 12));
            uint32_t nx = le32(e + 16 + 8);
            cur = s[446 + 16 + 4] ? ext + nx : 0;
        }
    }
out:
    kfree(s);
}

bool part_present(int idx) { return idx >= DISK_PART_BASE && idx < DISK_ALL && pt[idx - DISK_PART_BASE].len && ata_drive_present(pt[idx - DISK_PART_BASE].disk); }
uint32_t part_sectors(int idx) { return part_present(idx) ? pt[idx - DISK_PART_BASE].len : 0; }
const char* part_name(int idx) { return pt[idx - DISK_PART_BASE].len ? pt[idx - DISK_PART_BASE].name : "?"; }
int part_disk(int idx) { return pt[idx - DISK_PART_BASE].disk; }
uint32_t part_start(int idx) { return pt[idx - DISK_PART_BASE].start; }
int part_nr(int idx) { return pt[idx - DISK_PART_BASE].nr; }
