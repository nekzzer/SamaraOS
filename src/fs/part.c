#include "fs/part.h"
#include "drivers/ata.h"
#include "core/string.h"

static uint32_t r32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// returns number of partitions found
static int scan_gpt(int d, uint32_t total) {
    static uint8_t h[512], e[512];
    int n = 0;
    if (ata_read(d, 1, 1, h) < 0 || memcmp(h, "EFI PART", 8)) return 0;
    uint32_t lba = r32(h + 72), cnt = r32(h + 80), sz = r32(h + 84);
    if (sz < 128 || sz > 512 || cnt > 256) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t off = i * sz;
        if (i == 0 || off % 512 == 0) if (ata_read(d, lba + off / 512, 1, e) < 0) break;
        uint8_t* p = e + off % 512;
        int z = 1;
        for (int k = 0; k < 16; k++) if (p[k]) z = 0;
        if (z) continue;
        uint32_t st = r32(p + 32), en = r32(p + 40);
        if (r32(p + 36) || r32(p + 44) || en < st || en >= total) continue;
        if (ata_part_add(d, st, en - st + 1, (int)i + 1) >= 0) n++;
    }
    return n;
}

int part_scan(int d) {
    static uint8_t s[512];
    uint32_t total = ata_drive_sectors(d);
    if (ata_read(d, 0, 1, s) < 0 || s[510] != 0x55 || s[511] != 0xAA) return 0;
    if (!memcmp(s + 54, "FAT", 3) || !memcmp(s + 82, "FAT32", 5)) return 0;     // a bare fat volume
    uint8_t m[64];
    memcpy(m, s + 446, 64);
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t* p = m + i * 16;
        if (p[0] & 0x7f) return 0;                  // not an mbr
        if (p[4] == 0xEE) return scan_gpt(d, total);
    }
    for (int i = 0; i < 4; i++) {
        uint8_t* p = m + i * 16;
        uint32_t st = r32(p + 8), len = r32(p + 12);
        if (!p[4] || !len || st + len > total || st + len < st) continue;
        if (p[4] == 5 || p[4] == 0x0f || p[4] == 0x85) {
            // extended: chain of ebrs
            uint32_t eb = st, cur = st;
            for (int k = 0; k < 32; k++) {
                static uint8_t b[512];
                if (ata_read(d, cur, 1, b) < 0 || b[510] != 0x55 || b[511] != 0xAA) break;
                uint32_t ls = r32(b + 446 + 8), ll = r32(b + 446 + 12);
                if (b[446 + 4] && ll && cur + ls + ll <= total)
                    if (ata_part_add(d, cur + ls, ll, 5 + k) >= 0) n++;
                uint32_t ns = r32(b + 462 + 8);
                if (!b[462 + 4] || !ns) break;
                cur = eb + ns;
            }
            continue;
        }
        if (ata_part_add(d, st, len, i + 1) >= 0) n++;
    }
    return n;
}

void part_scan_all(void) {
    for (int i = 0; i < DISK_PART_BASE; i++)
        if (ata_drive_present(i)) part_scan(i);
}
