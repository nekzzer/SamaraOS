#ifndef SAMARA_AHCI_H
#define SAMARA_AHCI_H
#include "core/types.h"

/* AHCI (SATA) host controller driver: polled DMA, LBA48. Disks are indexed
   0..ahci_disk_count()-1 in port order. */

#define AHCI_MAX_DISKS 4

int         ahci_init(void);                 /* returns number of disks */
const char* ahci_status(void);
int         ahci_disk_count(void);
bool        ahci_present(int i);
uint32_t    ahci_sectors(int i);
const char* ahci_model(int i);
int         ahci_port(int i);
int         ahci_read(int i, uint32_t lba, int count, void* buf);
int         ahci_write(int i, uint32_t lba, int count, const void* buf);

#endif
