#include "core/vmm.h"
#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"

/* Intel 8254x. legacy descriptors, no irq, we just poll like the rtl.
   82540EM = qemu -device e1000 and virtualbox default, 82545EM = vmware,
   82574L (e1000e) mostly works with the same legacy stuff */

#define NRX 32
#define NTX 16

typedef struct __attribute__((packed)) {
    uint32_t addr, addr_hi;
    uint16_t len, csum;
    uint8_t status, err;
    uint16_t special;
} rxd_t;

typedef struct __attribute__((packed)) {
    uint32_t addr, addr_hi;
    uint16_t len;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} txd_t;

static volatile uint8_t* mmio;
static uint8_t mac[6];
static rxd_t* rx;
static txd_t* tx;
static uint8_t* rxbuf[NRX];
static uint8_t* txbuf[NTX];
static int rx_cur, tx_cur;

static uint32_t rd(uint32_t r) { return *(volatile uint32_t*)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t*)(mmio + r) = v; }

static uint16_t eeprom(int a) {
    // 82540/82545 layout. 82574 has addr at bit 2, but there RAL is filled anyway
    wr(0x14, 1 | (a << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(0x14);
        if (v & 0x10) return v >> 16;
    }
    return 0;
}

/* descriptor rings must be 16 aligned, kmalloc doesnt promise that */
static void* amalloc(int n) {
    uintptr_t p = (uintptr_t)kmalloc(n + 16);
    return (void*)((p + 15) & ~15ul);
}

const uint8_t* e1000_mac(void) { return mac; }

int e1000_init(void) {
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x10D3, 0x153A, 0 };
    // one scan, pci_find walks all 256 buses every call and that took seconds
    static pci_dev_t devs[32];
    pci_dev_t d;
    int n = pci_scan(devs, 32), ok = 0;
    for (int i = 0; i < n && !ok; i++)
        for (int j = 0; ids[j]; j++)
            if (devs[i].vendor == 0x8086 && devs[i].device == ids[j]) { d = devs[i]; ok = 1; break; }
    if (!ok) return -1;
    pci_enable_io_busmaster(&d);
    mmio = (volatile uint8_t*)P2V(d.bar[0] & ~0xFu);

    wr(0xD8, 0xFFFFFFFF);          /* IMC: no irqs */
    wr(0x0000, rd(0x0000) | (1u << 26));   /* reset */
    for (volatile int i = 0; i < 100000; i++) ;
    while (rd(0x0000) & (1u << 26)) ;
    wr(0xD8, 0xFFFFFFFF);
    rd(0xC0);
    wr(0x0000, (rd(0x0000) | (1 << 6) | (1 << 5)) & ~(1u << 3)); /* SLU, ASDE, no LRST */

    uint32_t lo = rd(0x5400), hi = rd(0x5404);
    if (hi & 0x80000000u) {
        for (int i = 0; i < 4; i++) mac[i] = lo >> (i * 8);
        mac[4] = hi; mac[5] = hi >> 8;
    } else {
        for (int i = 0; i < 3; i++) { uint16_t w = eeprom(i); mac[i * 2] = w; mac[i * 2 + 1] = w >> 8; }
        wr(0x5400, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
        wr(0x5404, mac[4] | mac[5] << 8 | 0x80000000u);
    }
    for (int i = 0; i < 128; i++) wr(0x5200 + i * 4, 0);

    rx = amalloc(sizeof(rxd_t) * NRX);
    memset(rx, 0, sizeof(rxd_t) * NRX);
    for (int i = 0; i < NRX; i++) {
        rxbuf[i] = kmalloc(2048);
        rx[i].addr = V2P(rxbuf[i]);
    }
    wr(0x2800, (uint32_t)V2P(rx));
    wr(0x2804, 0);
    wr(0x2808, sizeof(rxd_t) * NRX);
    wr(0x2810, 0);
    wr(0x2818, NRX - 1);
    rx_cur = 0;
    /* EN | BAM | BSIZE 2048 | SECRC */
    wr(0x0100, (1 << 1) | (1 << 15) | (1 << 26));

    tx = amalloc(sizeof(txd_t) * NTX);
    memset(tx, 0, sizeof(txd_t) * NTX);
    for (int i = 0; i < NTX; i++) {
        txbuf[i] = kmalloc(2048);
        tx[i].status = 1;          /* DD, free */
    }
    wr(0x3800, (uint32_t)V2P(tx));
    wr(0x3804, 0);
    wr(0x3808, sizeof(txd_t) * NTX);
    wr(0x3810, 0);
    wr(0x3818, 0);
    tx_cur = 0;
    wr(0x0410, 0x0060200A);        /* TIPG, magic from the manual */
    wr(0x0400, (1 << 1) | (1 << 3) | (0x0F << 4) | (0x40 << 12));

    // wait for link, real cards take a while
    uint32_t t0 = pit_uptime_ms();
    while (!(rd(0x0008) & 2) && pit_uptime_ms() - t0 < 3000) task_yield();
    return 0;
}

int e1000_send(const void* data, int len) {
    if (!mmio || len > 2048) return -1;
    txd_t* t = &tx[tx_cur];
    for (int i = 0; !(t->status & 1); i++)
        if (i > 1000000) return -3;   // ring stuck
    memcpy(txbuf[tx_cur], data, len);
    t->addr = V2P(txbuf[tx_cur]);
    t->addr_hi = 0;
    t->len = len < 60 ? 60 : len;   // card pads anyway but whatever
    t->cmd = 1 | 2 | 8;            /* EOP IFCS RS */
    t->status = 0;
    tx_cur = (tx_cur + 1) % NTX;
    wr(0x3818, tx_cur);
    return 0;
}

int e1000_recv(void* buf, int max) {
    if (!mmio) return 0;
    rxd_t* r = &rx[rx_cur];
    if (!(r->status & 1)) return 0;
    int n = r->len;
    if (n > max) n = max;
    memcpy(buf, rxbuf[rx_cur], n);
    r->status = 0;
    wr(0x2818, rx_cur);            /* give it back */
    rx_cur = (rx_cur + 1) % NRX;
    return n;
}
