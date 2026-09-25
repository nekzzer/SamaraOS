#include "drivers/ata.h"
#include "core/io.h"
#include "core/string.h"
#include "drivers/ahci.h"

#define SR_BSY  0x80
#define SR_DRDY 0x40
#define SR_DRQ  0x08
#define SR_ERR  0x01

typedef struct {
    uint16_t io;        /* I/O base: 0x1F0 or 0x170 */
    uint16_t ctrl;      /* control base: 0x3F6 or 0x376 */
    uint8_t  drv_sel;   /* 0xA0 master, 0xB0 slave */
    bool     present;
    uint32_t sectors;
} ata_t;

/* drv_sel: 0xE0 = master LBA, 0xF0 = slave LBA. The LBA bit (0x40) is
   harmless during IDENTIFY (which ignores CHS/LBA), so we keep one value
   per drive and OR in lba[27:24] during reads. */
static ata_t drives[ATA_DRIVES] = {
    { 0x1F0, 0x3F6, 0xE0, false, 0 },
    { 0x1F0, 0x3F6, 0xF0, false, 0 },
    { 0x170, 0x376, 0xE0, false, 0 },
    { 0x170, 0x376, 0xF0, false, 0 },
};

static void delay_400ns(const ata_t* d) {
    for (int i = 0; i < 4; i++) inb(d->ctrl);
}

static int wait_not_busy(const ata_t* d) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(inb(d->io + 7) & SR_BSY)) return 0;
    }
    return -1;
}

static int wait_drq(const ata_t* d) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t s = inb(d->io + 7);
        if (s & SR_ERR) return -1;
        if (!(s & SR_BSY) && (s & SR_DRQ)) return 0;
    }
    return -1;
}

static bool probe_drive(ata_t* d) {
    /* disable IRQs */
    outb(d->ctrl, 0x02);

    outb(d->io + 6, d->drv_sel);
    delay_400ns(d);

    uint8_t s = inb(d->io + 7);
    if (s == 0xFF) return false;          /* floating bus, no controller */

    /* zero registers */
    outb(d->io + 2, 0);
    outb(d->io + 3, 0);
    outb(d->io + 4, 0);
    outb(d->io + 5, 0);

    /* IDENTIFY */
    outb(d->io + 7, 0xEC);
    delay_400ns(d);
    s = inb(d->io + 7);
    if (s == 0) return false;             /* no drive */

    if (wait_not_busy(d) < 0) return false;

    /* ATAPI / SATAPI signature in LBA1/LBA2 → not ATA */
    if (inb(d->io + 4) != 0 || inb(d->io + 5) != 0) return false;

    if (wait_drq(d) < 0) return false;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(d->io + 0);

    d->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    d->present = true;
    return true;
}

/* ---- public API ---- */

int ata_primary(void) {
    for (int i = 0; i < ATA_DRIVES; i++) if (drives[i].present) return i;
    for (int i = 0; i < AHCI_MAX_DISKS; i++) if (ahci_present(i)) return DISK_AHCI_BASE + i;
    return -1;
}

bool ata_init(void) {
    if (probe_drive(&drives[0])) return true;
    return ata_primary() >= 0;
}
bool ata_present(void)               { return ata_primary() >= 0; }
uint32_t ata_total_sectors(void)     { return ata_drive_sectors(ata_primary()); }
int ata_read_sectors(uint32_t lba, int count, void* buf) {
    return ata_read(ata_primary(), lba, count, buf);
}

bool ata_init_all(void) {
    bool any = false;
    for (int i = 0; i < ATA_DRIVES; i++) {
        if (probe_drive(&drives[i])) any = true;
    }
    if (ahci_init() > 0) any = true;
    return any;
}

bool ata_drive_present(int idx) {
    if (idx >= DISK_AHCI_BASE && idx < DISK_MAX) return ahci_present(idx - DISK_AHCI_BASE);
    if (idx < 0 || idx >= ATA_DRIVES) return false;
    return drives[idx].present;
}

uint32_t ata_drive_sectors(int idx) {
    if (idx >= DISK_AHCI_BASE && idx < DISK_MAX) return ahci_sectors(idx - DISK_AHCI_BASE);
    if (idx < 0 || idx >= ATA_DRIVES) return 0;
    return drives[idx].sectors;
}

const char* ata_drive_name(int idx) {
    static const char* const names[DISK_MAX] = { "hda", "hdb", "hdc", "hdd", "sda", "sdb", "sdc", "sdd" };
    return (idx >= 0 && idx < DISK_MAX) ? names[idx] : "?";
}

int ata_read(int idx, uint32_t lba, int count, void* buf) {
    if (idx >= DISK_AHCI_BASE && idx < DISK_MAX) return ahci_read(idx - DISK_AHCI_BASE, lba, count, buf);
    if (idx < 0 || idx >= ATA_DRIVES) return -1;
    ata_t* d = &drives[idx];
    if (!d->present || count <= 0) return -1;
    uint16_t* p = (uint16_t*)buf;

    while (count > 0) {
        int chunk = count > 255 ? 255 : count;

        if (wait_not_busy(d) < 0) return -1;
        outb(d->io + 6, (uint8_t)((d->drv_sel & 0xF0) | ((lba >> 24) & 0x0F)));
        delay_400ns(d);
        outb(d->io + 1, 0);
        outb(d->io + 2, (uint8_t)chunk);
        outb(d->io + 3, (uint8_t)(lba & 0xFF));
        outb(d->io + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(d->io + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(d->io + 7, 0x20);                       /* READ SECTORS */

        for (int sector = 0; sector < chunk; sector++) {
            if (wait_drq(d) < 0) return -1;
            for (int w = 0; w < 256; w++) *p++ = inw(d->io + 0);
            delay_400ns(d);
        }

        lba += chunk;
        count -= chunk;
    }
    return 0;
}

int ata_write(int idx, uint32_t lba, int count, const void* buf) {
    if (idx >= DISK_AHCI_BASE && idx < DISK_MAX) return ahci_write(idx - DISK_AHCI_BASE, lba, count, buf);
    if (idx < 0 || idx >= ATA_DRIVES) return -1;
    ata_t* d = &drives[idx];
    if (!d->present || count <= 0) return -1;
    const uint16_t* p = (const uint16_t*)buf;

    while (count > 0) {
        int chunk = count > 255 ? 255 : count;

        if (wait_not_busy(d) < 0) return -1;
        outb(d->io + 6, (uint8_t)((d->drv_sel & 0xF0) | ((lba >> 24) & 0x0F)));
        delay_400ns(d);
        outb(d->io + 1, 0);
        outb(d->io + 2, (uint8_t)chunk);
        outb(d->io + 3, (uint8_t)(lba & 0xFF));
        outb(d->io + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(d->io + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(d->io + 7, 0x30);                       /* WRITE SECTORS */

        for (int sector = 0; sector < chunk; sector++) {
            if (wait_drq(d) < 0) return -1;
            for (int w = 0; w < 256; w++) outw(d->io + 0, *p++);
            delay_400ns(d);
        }
        lba += chunk;
        count -= chunk;
    }
    if (wait_not_busy(d) < 0) return -1;
    outb(d->io + 7, 0xE7);                           /* CACHE FLUSH */
    return wait_not_busy(d);
}
