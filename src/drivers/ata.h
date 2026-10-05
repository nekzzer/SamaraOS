#ifndef SAMARA_ATA_H
#define SAMARA_ATA_H
#include "core/types.h"

/* ATA driver — PIO mode, no IRQs. Supports up to 4 drives:
   0 = primary master    (I/O 0x1F0, ctrl 0x3F6, drive 0xA0)
   1 = primary slave     (I/O 0x1F0, ctrl 0x3F6, drive 0xB0)
   2 = secondary master  (I/O 0x170, ctrl 0x376, drive 0xA0)
   3 = secondary slave   (I/O 0x170, ctrl 0x376, drive 0xB0) */

#define ATA_DRIVES 4                 /* legacy IDE channels */
#define DISK_AHCI_BASE 4             /* indices 4..7: SATA disks behind AHCI */
#define DISK_VIRTIO_BASE 8           /* 8..11: virtio-blk, /dev/vda.. */
#define DISK_USB_BASE 12             /* 12..15: usb mass storage */
#define DISK_MAX 16                  /* whole disks */
#define DISK_PART_BASE 16            /* 16..47: partitions (mbr/gpt), sda1 and friends */
#define DISK_ALL 48
#define ATA_DRIVE_PRIMARY_MASTER     0
#define ATA_DRIVE_PRIMARY_SLAVE      1
#define ATA_DRIVE_SECONDARY_MASTER   2
#define ATA_DRIVE_SECONDARY_SLAVE    3

bool     ata_init(void);                                            /* detect drive 0 (legacy alias) */
bool     ata_init_all(void);                                        /* detect all 4 drives */
bool     ata_present(void);                                         /* alias for drive 0 */
uint32_t ata_total_sectors(void);                                   /* alias for drive 0 */
int      ata_read_sectors(uint32_t lba, int count, void* buf);      /* alias for drive 0 */

/* Indices 0..3 are IDE, 4..7 AHCI (see drivers/ahci.h). The legacy
   "drive 0" aliases above resolve to the first present disk of either kind. */
bool     ata_drive_present(int idx);
uint32_t ata_drive_sectors(int idx);
int      ata_read(int idx, uint32_t lba, int count, void* buf);
int      ata_write(int idx, uint32_t lba, int count, const void* buf);
int      ata_primary(void);                                         /* -1 if none */
uint32_t ata_rdev(int idx);                                         /* st_rdev for the /dev node */
void     ata_usb_added(int i);                                      /* usb disk i is up: partitions, /dev nodes */
void        part_scan(int disk);
const char* ata_drive_name(int idx);                                /* "hda", "sda", ... */

#endif
