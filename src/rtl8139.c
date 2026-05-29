#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "string.h"

/* === RTL8139 (10EC:8139) — polling driver, no IRQ. ===
   Reference: 0x00..05 = IDR0..5 (MAC), 0x10..1F = TSD0..3,
   0x20..2F = TSAD0..3, 0x30 = RBSTART, 0x37 = CR, 0x38 = CAPR,
   0x3C = IMR, 0x3E = ISR, 0x40 = TCR, 0x44 = RCR, 0x52 = CONFIG1. */

#define RTL_IDR0    0x00
#define RTL_TSD0    0x10
#define RTL_TSAD0   0x20
#define RTL_RBSTART 0x30
#define RTL_CR      0x37
#define RTL_CAPR    0x38
#define RTL_IMR     0x3C
#define RTL_ISR     0x3E
#define RTL_TCR     0x40
#define RTL_RCR     0x44
#define RTL_CONFIG1 0x52

#define CR_BUFE     0x01
#define CR_TE       0x04
#define CR_RE       0x08
#define CR_RST      0x10

/* RCR: AAP(1)|APM(2)|AM(4)|AB(8)|WRAP(1<<7)|MXDMA=7(1<<8..10) */
#define RCR_VAL     ((1<<0)|(1<<1)|(1<<2)|(1<<3)|(7<<8)|(1<<7))

#define RX_BUF_SIZE (8192 + 16 + 1500)   /* WRAP=1 needs trailing pad */
#define TX_BUF_SIZE 2048

static bool        g_present = false;
static uint16_t    g_io      = 0;
static uint8_t     g_mac[6];
static char        g_status[64] = "rtl8139: not initialized";
static uint8_t*    g_rx_buf  = 0;
static uint32_t    g_rx_off  = 0;        /* sw read offset within rx buffer */
static uint8_t*    g_tx_buf[4];
static int         g_tx_cur  = 0;

bool rtl8139_present(void) { return g_present; }
const uint8_t* rtl8139_mac(void) { return g_mac; }
const char*    rtl8139_status(void) { return g_status; }

static void set_status(const char* s) {
    int i; for (i = 0; i < 63 && s[i]; i++) g_status[i] = s[i];
    g_status[i] = 0;
}

int rtl8139_init(void) {
    if (g_present) return 0;

    pci_dev_t dev;
    if (!pci_find(0x10EC, 0x8139, &dev)) {
        set_status("rtl8139: PCI device not found");
        return -1;
    }
    pci_enable_io_busmaster(&dev);

    /* BAR0 = I/O base (LSB set to 1) */
    if (!(dev.bar[0] & 1)) {
        set_status("rtl8139: BAR0 not I/O");
        return -2;
    }
    g_io = (uint16_t)(dev.bar[0] & ~3u);

    /* power on */
    outb(g_io + RTL_CONFIG1, 0x00);

    /* soft reset */
    outb(g_io + RTL_CR, CR_RST);
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(g_io + RTL_CR) & CR_RST)) break;
    }
    if (inb(g_io + RTL_CR) & CR_RST) {
        set_status("rtl8139: reset timeout");
        return -3;
    }

    /* read MAC */
    for (int i = 0; i < 6; i++) g_mac[i] = inb(g_io + RTL_IDR0 + i);

    /* allocate rings */
    g_rx_buf = (uint8_t*)kmalloc(RX_BUF_SIZE);
    if (!g_rx_buf) { set_status("rtl8139: rx alloc fail"); return -4; }
    memset(g_rx_buf, 0, RX_BUF_SIZE);
    for (int i = 0; i < 4; i++) {
        g_tx_buf[i] = (uint8_t*)kmalloc(TX_BUF_SIZE);
        if (!g_tx_buf[i]) { set_status("rtl8139: tx alloc fail"); return -5; }
    }

    /* RBSTART = physical addr (1:1 paging) */
    outl(g_io + RTL_RBSTART, (uint32_t)g_rx_buf);

    /* mask all IRQs (we poll) */
    outw(g_io + RTL_IMR, 0);
    outw(g_io + RTL_ISR, 0xFFFF);   /* clear */

    /* configure RX */
    outl(g_io + RTL_RCR, RCR_VAL);

    /* TX configure: default works fine */
    outl(g_io + RTL_TCR, 0x03000700);

    /* enable RX + TX */
    outb(g_io + RTL_CR, CR_TE | CR_RE);

    /* set CAPR to 0 (sw offset 0 means register holds 0xFFF0 i.e. -16) */
    g_rx_off = 0;
    outw(g_io + RTL_CAPR, 0xFFF0);

    g_tx_cur = 0;
    g_present = true;

    /* print MAC into status */
    static const char* hex = "0123456789abcdef";
    char buf[64];
    int p = 0;
    const char* pre = "rtl8139: MAC ";
    for (int i = 0; pre[i]; i++) buf[p++] = pre[i];
    for (int i = 0; i < 6; i++) {
        buf[p++] = hex[g_mac[i] >> 4];
        buf[p++] = hex[g_mac[i] & 0xF];
        if (i != 5) buf[p++] = ':';
    }
    buf[p] = 0;
    set_status(buf);
    return 0;
}

int rtl8139_send(const void* data, int len) {
    if (!g_present) return -1;
    if (len < 14 || len > TX_BUF_SIZE) return -2;

    int idx = g_tx_cur;
    /* wait until this descriptor is free (OWN bit set means ready) */
    for (int i = 0; i < 1000000; i++) {
        uint32_t st = inl(g_io + RTL_TSD0 + idx * 4);
        if (st & (1 << 13)) break;   /* OWN=1 => idle */
        if (i == 999999) return -3;
    }

    memcpy(g_tx_buf[idx], data, len);
    /* pad runt frames */
    int padded = len < 60 ? 60 : len;
    if (padded > len) memset(g_tx_buf[idx] + len, 0, padded - len);

    outl(g_io + RTL_TSAD0 + idx * 4, (uint32_t)g_tx_buf[idx]);
    /* writing TSD with size and OWN=0 starts transmit */
    outl(g_io + RTL_TSD0 + idx * 4, (uint32_t)padded);

    g_tx_cur = (idx + 1) & 3;
    return 0;
}

int rtl8139_recv(void* buf, int max) {
    if (!g_present) return 0;
    if (inb(g_io + RTL_CR) & CR_BUFE) return 0;     /* nothing */

    /* status at sw offset; packet immediately after */
    uint32_t st = *(uint32_t*)(g_rx_buf + g_rx_off);
    uint16_t pkt_len = (uint16_t)(st >> 16);        /* incl 4-byte CRC */
    uint16_t rok     = (uint16_t)(st & 0xFFFF);
    if (!(rok & 1) || pkt_len < 4 || pkt_len > 1518) {
        /* RX error — reset the chip's RX (rare) */
        uint8_t cr = inb(g_io + RTL_CR);
        outb(g_io + RTL_CR, cr & ~CR_RE);
        outl(g_io + RTL_RCR, RCR_VAL);
        outb(g_io + RTL_CR, cr);
        g_rx_off = 0;
        outw(g_io + RTL_CAPR, 0xFFF0);
        return 0;
    }

    int data_len = pkt_len - 4;     /* strip CRC */
    if (data_len > max) data_len = max;
    /* copy from g_rx_off+4, possibly wrapping (extra 1500 padding prevents wrap on packet level) */
    memcpy(buf, g_rx_buf + g_rx_off + 4, data_len);

    /* advance sw read pointer; align to 4 */
    g_rx_off = (g_rx_off + pkt_len + 4 + 3) & ~3u;
    if (g_rx_off >= 8192) g_rx_off -= 8192;        /* wrap */

    outw(g_io + RTL_CAPR, (uint16_t)(g_rx_off - 0x10));
    return data_len;
}
