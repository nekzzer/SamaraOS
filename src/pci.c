#include "pci.h"
#include "io.h"

#define CF8 0xCF8
#define CFC 0xCFC

static uint32_t addr_of(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)dev << 11)
         | ((uint32_t)fn  << 8)
         | (off & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(CF8, addr_of(bus, dev, fn, off));
    return inl(CFC);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, dev, fn, off);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_cfg_read32(bus, dev, fn, off);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(CF8, addr_of(bus, dev, fn, off));
    outl(CFC, v);
}

void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v) {
    uint32_t cur = pci_cfg_read32(bus, dev, fn, off);
    uint32_t shift = (off & 2) * 8;
    uint32_t mask  = 0xFFFFu << shift;
    cur = (cur & ~mask) | ((uint32_t)v << shift);
    pci_cfg_write32(bus, dev, fn, off, cur);
}

static void fill_dev(pci_dev_t* d, uint8_t bus, uint8_t dev, uint8_t fn) {
    d->bus = bus; d->dev = dev; d->fn = fn;
    uint32_t v0 = pci_cfg_read32(bus, dev, fn, 0x00);
    d->vendor = (uint16_t)v0;
    d->device = (uint16_t)(v0 >> 16);
    uint32_t v8 = pci_cfg_read32(bus, dev, fn, 0x08);
    d->revision = (uint8_t)v8;
    d->prog_if  = (uint8_t)(v8 >> 8);
    d->subclass = (uint8_t)(v8 >> 16);
    d->class_c  = (uint8_t)(v8 >> 24);
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_cfg_read32(bus, dev, fn, 0x10 + i*4);
    d->irq = pci_cfg_read8(bus, dev, fn, 0x3C);
}

int pci_scan(pci_dev_t* out, int max) {
    int n = 0;
    for (int bus = 0; bus < 256 && n < max; bus++) {
        for (int dev = 0; dev < 32 && n < max; dev++) {
            uint16_t vid = pci_cfg_read16((uint8_t)bus, (uint8_t)dev, 0, 0);
            if (vid == 0xFFFF) continue;
            int max_fn = 1;
            uint8_t hdr = pci_cfg_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            if (hdr & 0x80) max_fn = 8;
            for (int fn = 0; fn < max_fn && n < max; fn++) {
                uint16_t fv = pci_cfg_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if (fv == 0xFFFF) continue;
                fill_dev(&out[n++], (uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
            }
        }
    }
    return n;
}

int pci_find(uint16_t vendor, uint16_t device, pci_dev_t* out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint16_t vid = pci_cfg_read16((uint8_t)bus, (uint8_t)dev, 0, 0);
            if (vid == 0xFFFF) continue;
            int max_fn = 1;
            uint8_t hdr = pci_cfg_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            if (hdr & 0x80) max_fn = 8;
            for (int fn = 0; fn < max_fn; fn++) {
                uint16_t fv = pci_cfg_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if (fv != vendor) continue;
                uint16_t did = pci_cfg_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 2);
                if (did != device) continue;
                fill_dev(out, (uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
                return 1;
            }
        }
    }
    return 0;
}

void pci_enable_io_busmaster(const pci_dev_t* d) {
    uint16_t cmd = pci_cfg_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= (1 << 0)   /* I/O space */
         | (1 << 1)   /* memory space */
         | (1 << 2);  /* bus master */
    pci_cfg_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}
