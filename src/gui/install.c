#include "gui/login.h"
#include "drivers/ata.h"
#include "fs/fs.h"
#include "fs/fatfs.h"
#include "core/heap.h"
#include "core/string.h"

/* Puts SamaraOS on a disk. Layout: one FAT32 volume over the whole disk, no
   partition table. GRUB's boot.img sits in the FAT boot sector (it leaves the
   BPB alone), core.img goes into the reserved sectors after the FSInfo and
   backup boot sector, and core.img has "multiboot /boot/samara.elf" baked in
   (iso/early.cfg). Both images come with the ISO in install.tar.

   Tested on qemu with SeaBIOS only. Some real BIOSes want a partition table
   to boot a hdd at all, idk. */

#define RSVD 2048          /* reserved sectors, 1 MB, plenty for core.img */
#define CORE_LBA 8

static uint8_t sec[512];

static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }

static const char* format(int disk) {
    fs_node_t* bi = fs_resolve(fs_root(), "/boot/grub/boot.img");
    fs_node_t* ci = fs_resolve(fs_root(), "/boot/grub/core.img");
    if (!bi || !ci || bi->size != 512) return "no grub images in /boot/grub";
    uint32_t core_secs = (ci->size + 511) / 512;
    if (CORE_LBA + core_secs > RSVD) return "grub core.img too big";

    /* geometry like mkfs.fat / the MS spec table */
    uint32_t n = ata_drive_sectors(disk);
    uint32_t spc = n <= 532480 ? 1 : n <= 16777216 ? 8 : n <= 33554432 ? 16 : 32;
    uint32_t t2 = (256 * spc + 2) / 2;
    uint32_t fatsz = (n - RSVD + t2 - 1) / t2;
    uint32_t data = RSVD + 2 * fatsz;
    if (n <= data || (n - data) / spc < 65525) return "disk too small";

    uint8_t* b = sec;
    memcpy(b, bi->data, 512);
    memset(b + 3, 0, 0x5A - 3);
    memcpy(b + 3, "SAMARAOS", 8);
    w16(b + 11, 512);
    b[13] = (uint8_t)spc;
    w16(b + 14, RSVD);
    b[16] = 2;
    b[21] = 0xF8;
    w16(b + 24, 63);
    w16(b + 26, 255);
    w32(b + 32, n);
    w32(b + 36, fatsz);
    w32(b + 44, 2);            /* root dir cluster */
    w16(b + 48, 1);            /* fsinfo */
    w16(b + 50, 6);            /* backup boot */
    b[64] = 0x80;
    b[66] = 0x29;
    w32(b + 67, n * 2654435761u);
    memcpy(b + 71, "SAMARA     ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    w32(b + 0x5C, CORE_LBA);   /* boot.img: where core.img starts */
    w32(b + 0x60, 0);
    if (ata_write(disk, 0, 1, b) < 0 || ata_write(disk, 6, 1, b) < 0) return "disk write error";

    memset(b, 0, 512);
    w32(b, 0x41615252);
    w32(b + 484, 0x61417272);
    w32(b + 488, 0xFFFFFFFF);
    w32(b + 492, 3);
    w32(b + 508, 0xAA550000);
    ata_write(disk, 1, 1, b);
    ata_write(disk, 7, 1, b);

    /* core.img, its first sector loads the rest from the blocklist at the end */
    uint8_t* core = (uint8_t*)kmalloc(core_secs * 512);
    if (!core) return "out of memory";
    memset(core, 0, core_secs * 512);
    memcpy(core, ci->data, ci->size);
    w32(core + 0x1F4, CORE_LBA + 1);
    w32(core + 0x1F8, 0);
    int r = ata_write(disk, CORE_LBA, (int)core_secs, core);
    kfree(core);
    if (r < 0) return "disk write error";

    /* both FATs + the root cluster */
    uint8_t* z = (uint8_t*)kmalloc(64 * 512);
    if (!z) return "out of memory";
    memset(z, 0, 64 * 512);
    uint32_t total = 2 * fatsz + spc;
    for (uint32_t s = 0; s < total; s += 64) {
        uint32_t k = total - s < 64 ? total - s : 64;
        if (ata_write(disk, RSVD + s, (int)k, z) < 0) { kfree(z); return "disk write error"; }
        if (!(s & 2047)) install_progress("formatting", 5 + (int)(s * 45 / total));
    }
    kfree(z);
    memset(b, 0, 512);
    w32(b, 0x0FFFFFF8);
    w32(b + 4, 0x0FFFFFFF);
    w32(b + 8, 0x0FFFFFFF);
    ata_write(disk, RSVD, 1, b);
    ata_write(disk, RSVD + fatsz, 1, b);
    return NULL;
}

static void put(fs_node_t* dir, const char* path, const char* text) {
    fs_node_t* f = fs_create(dir, path, FS_FILE);
    if (f) fs_write(f, text, strlen(text));
}

const char* install_run(int disk, const char* host, const char* user, const char* pass) {
    fs_node_t* elf = fs_resolve(fs_root(), "/boot/samara.elf");
    if (!elf || !elf->size) return "no /boot/samara.elf, boot from the ISO";

    /* the disk is /mnt already (make run disk.img): let go of it first */
    char mt[512], dev[32] = "/dev/";
    fatfs_mounts_text(mt, sizeof(mt));
    strcat(dev, ata_drive_name(disk));
    strcat(dev, " /mnt ");
    if (strstr(mt, dev)) fatfs_umount(fs_resolve(fs_root(), "/mnt"));

    install_progress("formatting", 2);
    const char* e = format(disk);
    if (e) return e;

    install_progress("copying the system", 55);
    fs_node_t* t = fs_resolve(fs_root(), "/target");
    if (!t) t = fs_create(fs_root(), "/target", FS_DIR);
    if (!t || fatfs_mount(disk, t) < 0) return "cant mount the new volume";
    fs_create(t, "boot", FS_DIR);
    fs_create(t, "etc", FS_DIR);
    fs_node_t* k = fs_create(t, "boot/samara.elf", FS_FILE);
    if (!k) { fatfs_umount(t); return "cant create boot/samara.elf"; }
    fs_set_static(k, elf->data, elf->size);    /* module memory, stays around */
    fs_touch(k);

    /* same password for root, otherwise ssh root@ is wide open */
    char salt[9], hash[48];
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    lo ^= hi * 2654435761u;
    for (int i = 0; i < 8; i++) { salt[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[lo % 36]; lo = lo * 1103515245u + 12345u; }
    salt[8] = 0;
    md5crypt(pass, salt, hash);

    char buf[512];
    strcpy(buf, "root:x:0:0:root:/root:/bin/sh\n");
    strcat(buf, user); strcat(buf, ":x:1000:1000:"); strcat(buf, user);
    strcat(buf, ":/home/"); strcat(buf, user); strcat(buf, ":/bin/sh\n");
    put(t, "etc/passwd", buf);
    strcpy(buf, "root:"); strcat(buf, hash); strcat(buf, ":19000:0:99999:7:::\n");
    strcat(buf, user); strcat(buf, ":"); strcat(buf, hash); strcat(buf, ":19000:0:99999:7:::\n");
    put(t, "etc/shadow", buf);
    strcpy(buf, "root:x:0:\n"); strcat(buf, user); strcat(buf, ":x:1000:\n");
    put(t, "etc/group", buf);
    strcpy(buf, host); strcat(buf, "\n");
    put(t, "etc/hostname", buf);

    install_progress("writing to disk (takes a bit)", 70);
    int r = fatfs_umount(t);       /* syncs everything */
    fs_unlink(fs_root(), "/target");
    return r < 0 ? "write back failed" : NULL;
}
