/* Loads Doom1.WAD from ATA disk into kernel heap once, then registers it
   with the libc-shim so DOOM's fopen("doom1.wad") returns it. */

#include "../core/types.h"
#include "../core/heap.h"
#include "../drivers/ata.h"
#include "../core/string.h"

extern void libc_set_wad(const uint8_t* buf, size_t sz, const char* name);
extern int  printf(const char* fmt, ...);

static uint8_t* g_buf  = 0;
static uint32_t g_size = 0;
static char     g_name[16] = "doom1.wad";

const uint8_t* samara_wad_buf(void)  { return g_buf; }
uint32_t       samara_wad_size(void) { return g_size; }
const char*    samara_wad_name(void) { return g_name; }

/* Lookup a lump by name in our cached WAD directory. Returns pointer to
   lump bytes inside g_buf, or NULL if not found. *out_size receives size. */
const uint8_t* samara_wad_find_lump(const char* name, uint32_t* out_size) {
    if (!g_buf) return 0;
    if (out_size) *out_size = 0;
    uint32_t numlumps = *(uint32_t*)(g_buf + 4);
    uint32_t dirof    = *(uint32_t*)(g_buf + 8);
    for (uint32_t i = 0; i < numlumps; i++) {
        const uint8_t* entry = g_buf + dirof + i * 16;
        const char* lumpname = (const char*)(entry + 8);
        int match = 1;
        for (int k = 0; k < 8; k++) {
            char a = lumpname[k];
            char b = name[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) { match = 0; break; }
            if (b == 0) break;
        }
        if (match) {
            uint32_t off = *(uint32_t*)entry;
            uint32_t sz  = *(uint32_t*)(entry + 4);
            if (out_size) *out_size = sz;
            return g_buf + off;
        }
    }
    return 0;
}

int samara_wad_load(void) {
    if (g_buf) return 0;
    if (!ata_present()) {
        if (!ata_init()) return -1;
    }
    /* read header first to learn lump count -> size */
    uint8_t hdr[512];
    if (ata_read_sectors(0, 1, hdr) != 0) return -2;
    if ((hdr[0] != 'I' && hdr[0] != 'P') || hdr[1] != 'W' || hdr[2] != 'A' || hdr[3] != 'D') {
        return -3;
    }
    uint32_t numlumps = *(uint32_t*)(hdr + 4);
    uint32_t dir_off  = *(uint32_t*)(hdr + 8);
    /* total file size = max(end of last lump, dir end) — we just round up to a
       generous size: dir_off + numlumps*16 is the trailing directory. */
    uint32_t total = dir_off + numlumps * 16;
    /* Doom1.WAD shareware is ~4.2 MB; pad to be safe */
    uint32_t sectors = (total + 511) / 512;
    /* read whole WAD */
    g_buf = (uint8_t*)kmalloc(sectors * 512);
    if (!g_buf) return -4;
    if (ata_read_sectors(0, (int)sectors, g_buf) != 0) {
        return -5;
    }
    g_size = total;
    libc_set_wad(g_buf, g_size, g_name);
    return 0;
}
