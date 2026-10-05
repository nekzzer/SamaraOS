// samara-mkdisk DISK boot.img core.img: gpt (bios boot 1M, esp 256M, rest root), grub boot.img
// as protective mbr, core.img into the bios boot partition. samara-install calls it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

static uint32_t crc_tab[256];

static uint32_t crc32(const uint8_t* p, int n) {
    uint32_t c = ~0u;
    while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }

// guids in on-disk (mixed endian) form
static const uint8_t g_bios[16] = { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 };
static const uint8_t g_esp[16]  = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t g_lin[16]  = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };

static int fd;

static void put(uint64_t lba, const void* b, int n) {
    if (pwrite(fd, b, n, lba * 512) != n) { perror("write"); exit(1); }
}

static void set_name(uint8_t* e, const char* s) {
    for (int i = 0; s[i]; i++) e[56 + i * 2] = s[i];
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: samara-mkdisk disk boot.img core.img\n"); return 1; }
    for (int i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror(argv[1]); return 1; }
    uint64_t total = 0;
    if (ioctl(fd, 0x1260, &total) < 0 || total < 600000) { fprintf(stderr, "disk too small\n"); return 1; }

    // gpt: 128 entries * 128 bytes = 32 sectors after the header
    static uint8_t ent[128 * 128];
    uint64_t first = 2048, last = total - 34;
    uint64_t bios_s = 2048, bios_e = 4095;
    uint64_t esp_s = 4096, esp_e = esp_s + 262144 - 1;
    uint64_t root_s = esp_e + 1, root_e = last;
    uint8_t guid[16];
    int r = open("/dev/urandom", O_RDONLY);
    uint8_t* e = ent;
    memcpy(e, g_bios, 16); read(r, guid, 16); memcpy(e + 16, guid, 16); w64(e + 32, bios_s); w64(e + 40, bios_e); set_name(e, "bios");
    e += 128;
    memcpy(e, g_esp, 16); read(r, guid, 16); memcpy(e + 16, guid, 16); w64(e + 32, esp_s); w64(e + 40, esp_e); set_name(e, "esp");
    e += 128;
    memcpy(e, g_lin, 16); read(r, guid, 16); memcpy(e + 16, guid, 16); w64(e + 32, root_s); w64(e + 40, root_e); set_name(e, "root");
    uint32_t ecrc = crc32(ent, sizeof(ent));
    read(r, guid, 16);

    uint8_t h[512];
    for (int pass = 0; pass < 2; pass++) {
        memset(h, 0, 512);
        memcpy(h, "EFI PART", 8);
        w32(h + 8, 0x10000);
        w32(h + 12, 92);
        w64(h + 24, pass ? total - 1 : 1);
        w64(h + 32, pass ? 1 : total - 1);
        w64(h + 40, first);
        w64(h + 48, last);
        memcpy(h + 56, guid, 16);
        w64(h + 72, pass ? total - 33 : 2);
        w32(h + 80, 128);
        w32(h + 84, 128);
        w32(h + 88, ecrc);
        w32(h + 16, crc32(h, 92));
        put(pass ? total - 33 : 2, ent, sizeof(ent));
        put(pass ? total - 1 : 1, h, 512);
    }

    // boot.img as the protective mbr, kernel_sector (0x5c) points at core.img
    uint8_t m[512];
    FILE* f = fopen(argv[2], "rb");
    if (!f || fread(m, 1, 512, f) != 512) { fprintf(stderr, "bad boot.img\n"); return 1; }
    fclose(f);
    memset(m + 0x1BE, 0, 64);
    m[0x1C2] = 0xEE;
    m[0x1C1] = 0xFF; m[0x1C3] = 0xFF; m[0x1C4] = 0xFF;
    w32(m + 0x1C6, 1);
    w32(m + 0x1CA, total - 1 > 0xFFFFFFFFull ? 0xFFFFFFFF : (uint32_t)(total - 1));
    w32(m + 0x5C, 2048);
    w32(m + 0x60, 0);
    m[510] = 0x55; m[511] = 0xAA;
    put(0, m, 512);

    // core.img: diskboot blocklist (last 12 bytes of sector 0) = start, len, seg
    f = fopen(argv[3], "rb");
    if (!f) { perror(argv[3]); return 1; }
    static uint8_t core[1024 * 1024];
    int n = fread(core, 1, sizeof(core), f);
    fclose(f);
    if (n < 512 || n > 1024 * 1024 - 512) { fprintf(stderr, "core.img too big\n"); return 1; }
    w64(core + 0x1F4, 2049);
    *(uint16_t*)(core + 0x1FC) = (n + 511) / 512 - 1;
    *(uint16_t*)(core + 0x1FE) = 0x820;
    put(2048, core, (n + 511) / 512 * 512);

    fsync(fd);
    ioctl(fd, 0x125F, 0);
    printf("gpt ok: %llu sectors, esp %llu+%llu, root %llu+%llu\n", (unsigned long long)total,
           (unsigned long long)esp_s, (unsigned long long)(esp_e - esp_s + 1), (unsigned long long)root_s, (unsigned long long)(root_e - root_s + 1));
    return 0;
}
