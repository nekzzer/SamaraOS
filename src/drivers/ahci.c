#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/io.h"
#include "core/vmm.h"

/* AHCI 1.x SATA driver: polled DMA, one command slot per port.

   The HBA's registers (ABAR, PCI BAR5) sit high in the physical address
   space, which the kernel's 4 MiB identity map already covers, and every
   buffer handed to the controller is kernel heap memory whose virtual
   address equals its physical one. */

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VS    0x10

#define GHC_AE    (1u << 31)
#define GHC_HR    (1u << 0)

#define PX_CLB    0x00
#define PX_CLBU   0x04
#define PX_FB     0x08
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_IE     0x14
#define PX_CMD    0x18
#define PX_TFD    0x20
#define PX_SIG    0x24
#define PX_SSTS   0x28
#define PX_SCTL   0x2C
#define PX_SERR   0x30
#define PX_CI     0x38

#define CMD_ST    (1u << 0)
#define CMD_SUD   (1u << 1)
#define CMD_POD   (1u << 2)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)

#define TFD_BSY   0x80
#define TFD_DRQ   0x08
#define TFD_ERR   0x01
#define IS_TFES   (1u << 30)

#define SIG_ATA   0x00000101u

#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT     0xEA
#define ATA_IDENTIFY      0xEC

#define MAX_SECTORS_PER_CMD 128          /* 64 KiB, one PRD */

typedef struct {
    uint8_t  cfl_flags;       /* [4:0] FIS length in dwords, [6] write */
    uint8_t  flags2;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsv[4];
} __attribute__((packed)) cmd_hdr_t;

typedef struct {
    uint32_t dba, dbau, rsv;
    uint32_t dbc;             /* [21:0] byte count - 1, [31] irq on completion */
} __attribute__((packed)) prd_t;

typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    prd_t    prd[1];
} __attribute__((packed)) cmd_table_t;

typedef struct {
    bool       present;
    int        port;
    uint32_t   sectors;
    char       model[41];
    cmd_hdr_t* clb;
    uint8_t*   fis;
    cmd_table_t* tbl;
} ahci_disk_t;

static volatile uint8_t* abar;
static ahci_disk_t disks[AHCI_MAX_DISKS];
static int  n_disks;
static char status_msg[64] = "not probed";

static inline uint32_t hba_r(uint32_t off) { return *(volatile uint32_t*)(abar + off); }
static inline void hba_w(uint32_t off, uint32_t v) { *(volatile uint32_t*)(abar + off) = v; }
static inline uint32_t px_r(int p, uint32_t off) { return hba_r(0x100 + (uint32_t)p * 0x80 + off); }
static inline void px_w(int p, uint32_t off, uint32_t v) { hba_w(0x100 + (uint32_t)p * 0x80 + off, v); }

static void* alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t* raw = (uint8_t*)kmalloc(size + align);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uint32_t)raw + align - 1) & ~(align - 1));
    memset(p, 0, size);
    return p;                                   /* never freed: lives with the port */
}

static void dbg(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }
static void dbg_hex(uint32_t v) { char b[12]; utoa(v, b, 16); dbg(b); }

static void delay(void) { for (int i = 0; i < 1000; i++) io_wait(); }

static bool wait_clear(int p, uint32_t off, uint32_t mask) {
    for (int i = 0; i < 500000; i++) {
        if (!(px_r(p, off) & mask)) return true;
        io_wait();
    }
    return false;
}

static void port_stop(int p) {
    px_w(p, PX_CMD, px_r(p, PX_CMD) & ~CMD_ST);
    wait_clear(p, PX_CMD, CMD_CR);
    px_w(p, PX_CMD, px_r(p, PX_CMD) & ~CMD_FRE);
    wait_clear(p, PX_CMD, CMD_FR);
}

static void port_start(int p) {
    wait_clear(p, PX_CMD, CMD_CR);
    px_w(p, PX_CMD, px_r(p, PX_CMD) | CMD_FRE | CMD_SUD | CMD_POD);
    px_w(p, PX_CMD, px_r(p, PX_CMD) | CMD_ST);
}

/* Build a register H2D FIS in slot 0 and run it to completion. */
static int issue(ahci_disk_t* d, uint8_t cmd, uint64_t lba, uint16_t count,
                 void* buf, uint32_t bytes, bool write) {
    int p = d->port;
    if (!wait_clear(p, PX_TFD, TFD_BSY | TFD_DRQ)) return -1;

    cmd_hdr_t* h = &d->clb[0];
    h->cfl_flags = 5 | (write ? 0x40 : 0);         /* 20-byte FIS */
    h->flags2 = 0;
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;

    cmd_table_t* t = d->tbl;
    memset(t->cfis, 0, sizeof(t->cfis));
    if (bytes) {
        t->prd[0].dba = V2P(buf);           /* big-arena buffers live in the direct map */
        t->prd[0].dbau = 0;
        t->prd[0].rsv = 0;
        t->prd[0].dbc = (bytes - 1) | (1u << 31);
    }
    uint8_t* f = t->cfis;
    f[0] = 0x27;                                    /* Register H2D */
    f[1] = 0x80;                                    /* C: command */
    f[2] = cmd;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                                    /* LBA mode */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);

    px_w(p, PX_IS, 0xFFFFFFFFu);
    px_w(p, PX_CI, 1);
    for (uint32_t i = 0; i < 20000000; i++) {
        if (!(px_r(p, PX_CI) & 1)) break;
        if (px_r(p, PX_IS) & IS_TFES) return -1;
    }
    if (px_r(p, PX_CI) & 1) return -1;
    if ((px_r(p, PX_IS) & IS_TFES) || (px_r(p, PX_TFD) & TFD_ERR)) return -1;
    return 0;
}

static void ata_string(char* out, const uint16_t* id, int first, int words) {
    int n = 0;
    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)(id[first + i] & 0xFF);
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static bool port_init(int p, ahci_disk_t* d) {
    uint32_t ssts = px_r(p, PX_SSTS);

    if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1) return false;   /* no active device */

    port_stop(p);
    d->clb = (cmd_hdr_t*)alloc_aligned(1024, 1024);
    d->fis = (uint8_t*)alloc_aligned(256, 256);
    d->tbl = (cmd_table_t*)alloc_aligned(sizeof(cmd_table_t), 128);
    if (!d->clb || !d->fis || !d->tbl) return false;
    d->clb[0].ctba = (uint32_t)d->tbl;
    d->clb[0].ctbau = 0;
    px_w(p, PX_CLB, (uint32_t)d->clb);
    px_w(p, PX_CLBU, 0);
    px_w(p, PX_FB, (uint32_t)d->fis);
    px_w(p, PX_FBU, 0);
    px_w(p, PX_SERR, 0xFFFFFFFFu);
    px_w(p, PX_IS, 0xFFFFFFFFu);
    px_w(p, PX_IE, 0);                              /* polled */
    port_start(p);

    /* The signature only lands once FIS receive is on and the device has
       sent its first D2H register FIS. */
    for (int i = 0; i < 200000 && (px_r(p, PX_SIG) == 0xFFFFFFFFu || (px_r(p, PX_TFD) & TFD_BSY)); i++)
        io_wait();
    if (px_r(p, PX_SIG) != SIG_ATA) {                                     /* ATAPI etc. */
        dbg("  not an ATA disk, sig="); dbg_hex(px_r(p, PX_SIG)); dbg("\r\n");
        port_stop(p);
        return false;
    }

    d->port = p;
    static uint16_t id[256] __attribute__((aligned(16)));
    if (issue(d, ATA_IDENTIFY, 0, 0, id, 512, false) < 0) {
        dbg("  identify failed is="); dbg_hex(px_r(p, PX_IS)); dbg(" tfd="); dbg_hex(px_r(p, PX_TFD));
        dbg(" ci="); dbg_hex(px_r(p, PX_CI)); dbg(" cmd="); dbg_hex(px_r(p, PX_CMD)); dbg("\r\n");
        return false;
    }
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    d->sectors = lba48 ? (lba48 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)lba48) : lba28;
    ata_string(d->model, id, 27, 20);
    d->present = true;
    return true;
}

int ahci_init(void) {
    n_disks = 0;
    pci_dev_t devs[32];
    int n = pci_scan(devs, 32);
    pci_dev_t* hba = NULL;
    for (int i = 0; i < n; i++)
        if (devs[i].class_c == 0x01 && devs[i].subclass == 0x06 && devs[i].prog_if == 0x01) {
            hba = &devs[i];
            break;
        }
    if (!hba) { strcpy(status_msg, "no AHCI controller"); return 0; }

    pci_enable_io_busmaster(hba);
    abar = (volatile uint8_t*)(hba->bar[5] & ~0xFu);
    if (!abar) { strcpy(status_msg, "AHCI BAR5 unset"); return 0; }

    hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_AE);
    hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_HR);          /* reset the HBA */
    for (int i = 0; i < 100000 && (hba_r(HBA_GHC) & GHC_HR); i++) io_wait();
    hba_w(HBA_GHC, hba_r(HBA_GHC) | GHC_AE);
    delay();

    uint32_t pi = hba_r(HBA_PI);
    for (int p = 0; p < 32 && n_disks < AHCI_MAX_DISKS; p++) {
        if (!(pi & (1u << p))) continue;
        /* COMRESET so the link comes back up after the HBA reset. */
        px_w(p, PX_SCTL, (px_r(p, PX_SCTL) & ~0xFu) | 1);
        delay();
        px_w(p, PX_SCTL, px_r(p, PX_SCTL) & ~0xFu);
        for (int i = 0; i < 100000 && (px_r(p, PX_SSTS) & 0x0F) != 3; i++) io_wait();
        if (port_init(p, &disks[n_disks])) n_disks++;
    }

    uint32_t vs = hba_r(HBA_VS);
    char tmp[12];
    strcpy(status_msg, "AHCI ");
    utoa(vs >> 16, tmp, 10); strcat(status_msg, tmp); strcat(status_msg, ".");
    utoa((vs >> 8) & 0xFF, tmp, 10); strcat(status_msg, tmp);
    strcat(status_msg, ", ");
    itoa(n_disks, tmp, 10); strcat(status_msg, tmp);
    strcat(status_msg, n_disks == 1 ? " disk" : " disks");
    return n_disks;
}

const char* ahci_status(void) { return status_msg; }
int  ahci_disk_count(void) { return n_disks; }

bool ahci_present(int i) { return i >= 0 && i < n_disks && disks[i].present; }
uint32_t ahci_sectors(int i) { return ahci_present(i) ? disks[i].sectors : 0; }
const char* ahci_model(int i) { return ahci_present(i) ? disks[i].model : ""; }
int  ahci_port(int i) { return ahci_present(i) ? disks[i].port : -1; }

/* Controller DMA wants word-aligned, physically contiguous buffers (kernel
   identity or direct-map memory); bounce anything else. */
static int xfer(int i, uint32_t lba, int count, void* buf, bool write) {
    if (!ahci_present(i) || count <= 0) return -1;
    ahci_disk_t* d = &disks[i];
    if (lba + (uint32_t)count > d->sectors || lba + (uint32_t)count < lba) return -1;
    uint8_t* p = (uint8_t*)buf;
    uint8_t* bounce = NULL;
    if (((uint32_t)p & 1) || !dma_ok(p)) {
        bounce = (uint8_t*)kmalloc(MAX_SECTORS_PER_CMD * 512);   /* 8-byte aligned */
        if (!bounce) return -1;
    }
    int rc = 0;
    while (count > 0) {
        int chunk = count > MAX_SECTORS_PER_CMD ? MAX_SECTORS_PER_CMD : count;
        uint32_t bytes = (uint32_t)chunk * 512;
        uint8_t* target = bounce ? bounce : p;
        if (write && bounce) memcpy(bounce, p, bytes);
        if (issue(d, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, (uint16_t)chunk,
                  target, bytes, write) < 0) { rc = -1; break; }
        if (!write && bounce) memcpy(p, bounce, bytes);
        p += bytes;
        lba += (uint32_t)chunk;
        count -= chunk;
    }
    if (write && rc == 0) issue(d, ATA_FLUSH_EXT, 0, 0, NULL, 0, false);
    if (bounce) kfree(bounce);
    return rc;
}

int ahci_read(int i, uint32_t lba, int count, void* buf)        { return xfer(i, lba, count, buf, false); }
int ahci_write(int i, uint32_t lba, int count, const void* buf) { return xfer(i, lba, count, (void*)buf, true); }
