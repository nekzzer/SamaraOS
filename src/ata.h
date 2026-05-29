#ifndef SAMARA_ATA_H
#define SAMARA_ATA_H
#include "types.h"

/* ATA driver — PIO mode, no IRQs. Supports up to 4 drives:
   0 = primary master    (I/O 0x1F0, ctrl 0x3F6, drive 0xA0)
   1 = primary slave     (I/O 0x1F0, ctrl 0x3F6, drive 0xB0)
   2 = secondary master  (I/O 0x170, ctrl 0x376, drive 0xA0)
   3 = secondary slave   (I/O 0x170, ctrl 0x376, drive 0xB0) */

#define ATA_DRIVES 4
#define ATA_DRIVE_PRIMARY_MASTER     0
#define ATA_DRIVE_PRIMARY_SLAVE      1
#define ATA_DRIVE_SECONDARY_MASTER   2
#define ATA_DRIVE_SECONDARY_SLAVE    3

bool     ata_init(void);                                            /* detect drive 0 (legacy alias) */
bool     ata_init_all(void);                                        /* detect all 4 drives */
bool     ata_present(void);                                         /* alias for drive 0 */
uint32_t ata_total_sectors(void);                                   /* alias for drive 0 */
int      ata_read_sectors(uint32_t lba, int count, void* buf);      /* alias for drive 0 */

bool     ata_drive_present(int idx);
uint32_t ata_drive_sectors(int idx);
int      ata_read(int idx, uint32_t lba, int count, void* buf);

#endif
