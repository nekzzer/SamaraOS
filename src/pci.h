#ifndef SAMARA_PCI_H
#define SAMARA_PCI_H
#include "types.h"

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class_c, subclass, prog_if, revision;
    uint32_t bar[6];
    uint8_t  irq;
} pci_dev_t;

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint8_t  pci_cfg_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
void     pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v);

/* Scan all bus/dev/fn, write up to `max` entries into `out`. Returns count. */
int  pci_scan(pci_dev_t* out, int max);

/* Find first device with matching vendor:device pair. Returns 1 on hit. */
int  pci_find(uint16_t vendor, uint16_t device, pci_dev_t* out);

/* Enable IO + bus-master in COMMAND register. */
void pci_enable_io_busmaster(const pci_dev_t* d);

#endif
