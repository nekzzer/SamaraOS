#include "core/vmm.h"
#include "drivers/usb.h"
#include "drivers/pci.h"
#include "drivers/mouse.h"
#include "core/io.h"
#include "core/task.h"
#include "core/string.h"
#include "boot/pit.h"
#include "drivers/xhci.h"
#include "drivers/input.h"

/* UHCI only (qemu -usb, piix3 and friends), no irq, everything polled.
   xhci/ehci later, maybe. Only HID boot mice for now. */

#define USBCMD  0
#define USBSTS  2
#define USBINTR 4
#define FRNUM   6
#define FLBASE  8
#define SOFMOD  0x0C
#define PORTSC  0x10

typedef struct { uint32_t link, st, tok, buf; } td_t;
typedef struct { uint32_t link, elem, pad[2]; } qh_t;

static uint32_t fl[1024] __attribute__((aligned(4096)));
static qh_t qh_i __attribute__((aligned(16)));
static qh_t qh_c __attribute__((aligned(16)));
static td_t tds[24] __attribute__((aligned(16)));
static uint8_t setup[8] __attribute__((aligned(16)));
static uint8_t dbuf[128] __attribute__((aligned(16)));
static uint8_t rep[8] __attribute__((aligned(16)));

static uint16_t base;
static char status[48] = "no uhci";

// mouse state
static int m_addr, m_ep, m_ls, m_len, m_tog, m_port = -1;
#define itd (&tds[23])

void slog(const char* s) {
    for (const char* m = "samara: usb: "; *m; m++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m); }
    for (; *s; s++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s); }
    while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, '\r');
    while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, '\n');
}

static void setst(const char* s) { strncpy(status, s, 47); status[47] = 0; slog(s); }

const char* usb_status(void) { return status; }

// returns 0 ok, -1 error/timeout
static int ctl(int addr, int ls, int mps, uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, uint16_t len, void* out) {
    int n = 0, in = (rt & 0x80) && len;
    uint32_t st = (3 << 27) | (ls << 26) | (1 << 23);
    setup[0] = rt; setup[1] = rq;
    setup[2] = val; setup[3] = val >> 8;
    setup[4] = idx; setup[5] = idx >> 8;
    setup[6] = len; setup[7] = len >> 8;
    tds[n].st = st;
    tds[n].tok = 0x2D | (addr << 8) | (7 << 21);
    tds[n].buf = (uint32_t)V2P(setup);
    n++;
    int tog = 1;
    if (in) for (int off = 0; off < len; off += mps) {
        int c = len - off < mps ? len - off : mps;
        tds[n].st = st;
        tds[n].tok = 0x69 | (addr << 8) | (tog << 19) | ((c - 1) << 21);
        tds[n].buf = (uint32_t)V2P(dbuf) + off;
        tog ^= 1;
        n++;
    }
    tds[n].st = st | (1 << 24);
    tds[n].tok = (in ? 0xE1 : 0x69) | (addr << 8) | (1 << 19) | (0x7FF << 21);
    tds[n].buf = 0;
    n++;
    for (int i = 0; i < n; i++) tds[i].link = i == n - 1 ? 1 : ((uint32_t)V2P(&tds[i + 1]) | 4);
    qh_c.elem = (uint32_t)V2P(&tds[0]);

    int ret = -1;
    uint32_t end = pit_uptime_ms() + 500;
    while ((int32_t)(pit_uptime_ms() - end) < 0) {
        int bad = 0;
        for (int i = 0; i < n; i++) if (tds[i].st & 0x760000) bad = 1;
        if (bad) break;
        if (!(tds[n - 1].st & (1 << 23))) { ret = 0; break; }
        task_sleep_ms(1);
    }
    qh_c.elem = 1;
    if (ret == 0 && in && out) memcpy(out, dbuf, len);
    return ret;
}

static void arm(void) {
    itd->link = 1;
    itd->st = (3 << 27) | (m_ls << 26) | (1 << 23);
    itd->tok = 0x69 | (m_addr << 8) | ((m_ep & 15) << 15) | (m_tog << 19) | ((m_len - 1) << 21);
    itd->buf = (uint32_t)V2P(rep);
    qh_i.elem = (uint32_t)V2P(itd);
}

static int enum_port(int p) {
    uint16_t io = base + PORTSC + 2 * p;
    outw(io, 0x200);
    task_sleep_ms(60);
    outw(io, 0);
    task_sleep_ms(10);
    outw(io, 4);
    task_sleep_ms(10);
    outw(io, inw(io) | 0xA);
    if (!(inw(io) & 4)) return -1;
    int ls = (inw(io) >> 8) & 1;
    int mps = 8, addr = 1 + p;
    uint8_t d[128];

    if (ctl(0, ls, 8, 0x80, 6, 0x0100, 0, 8, d) < 0) return -1;
    if (d[7] >= 8 && d[7] <= 64) mps = d[7];
    if (ctl(0, ls, mps, 0, 5, addr, 0, 0, 0) < 0) return -1;
    task_sleep_ms(5);
    if (ctl(addr, ls, mps, 0x80, 6, 0x0200, 0, 9, d) < 0) return -1;
    int total = d[2] | d[3] << 8, cfg = d[5];
    if (total > 120) total = 120;
    if (ctl(addr, ls, mps, 0x80, 6, 0x0200, 0, total, d) < 0) return -1;

    int ifn = -1, ep = 0, epmps = 0, ok = 0;
    for (int i = 0; i + 1 < total && d[i]; i += d[i]) {
        if (d[i + 1] == 4) ok = d[i + 5] == 3 && d[i + 6] == 1 && d[i + 7] == 2, ifn = d[i + 2];
        else if (d[i + 1] == 5 && ok && (d[i + 2] & 0x80) && (d[i + 3] & 3) == 3) {
            ep = d[i + 2]; epmps = d[i + 4];
            break;
        }
    }
    if (!ep) { slog("not a hid mouse"); return -1; }
    if (ctl(addr, ls, mps, 0, 9, cfg, 0, 0, 0) < 0) return -1;
    ctl(addr, ls, mps, 0x21, 0x0B, 0, ifn, 0, 0);        // boot protocol
    ctl(addr, ls, mps, 0x21, 0x0A, 0, ifn, 0, 0);        // idle 0, some devs stall it, dont care
    m_addr = addr; m_ep = ep; m_ls = ls; m_tog = 0;
    m_len = epmps > 8 ? 8 : epmps;
    m_port = p;
    arm();
    return 0;
}

static void uhci_up(pci_dev_t* d) {
    pci_enable_io_busmaster(d);
    pci_cfg_write16(d->bus, d->dev, d->fn, 0xC0, 0x8F00);   // legacy smi off
    pci_cfg_write16(d->bus, d->dev, d->fn, 0xC0, 0x2000);
    base = d->bar[4] & 0xFFFC;
    outw(base + USBCMD, 4);
    task_sleep_ms(50);
    outw(base + USBCMD, 0);
    task_sleep_ms(10);
    outw(base + USBCMD, 2);
    for (int i = 0; i < 100 && (inw(base + USBCMD) & 2); i++) task_sleep_ms(1);
    outw(base + USBINTR, 0);
    for (int i = 0; i < 1024; i++) fl[i] = (uint32_t)V2P(&qh_i) | 2;
    qh_i.link = (uint32_t)V2P(&qh_c) | 2;
    qh_c.link = 1;
    qh_i.elem = qh_c.elem = 1;
    outw(base + FRNUM, 0);
    outl(base + FLBASE, (uint32_t)V2P(fl));
    outb(base + SOFMOD, 0x40);
    outw(base + USBSTS, 0xFFFF);
    outw(base + USBCMD, 1 | 0x40 | 0x80);
}

static void usb_task(void) {
    pci_dev_t devs[32], *d = 0;
    int n = pci_scan(devs, 32);
    for (int i = 0; i < n; i++)
        if (devs[i].class_c == 0x0C && devs[i].subclass == 3 && devs[i].prog_if == 0) { d = &devs[i]; break; }
    input_init();
    int xh = xhci_up();
    if (!d && !xh) { setst("no usb"); return; }
    if (xh) setst("xhci up");
    if (d) {
        uhci_up(d);
        setst("uhci up");
    }
    int tick = 0;
    for (;;) {
        xhci_poll();
        if (!d) {
            task_sleep_ms(1);
            continue;
        }
        if (m_port < 0) {
            if (++tick >= 100) {                          // ~400ms
                tick = 0;
                for (int p = 0; p < 2 && m_port < 0; p++) {
                    if (!(inw(base + PORTSC + 2 * p) & 1)) continue;
                    if (enum_port(p) == 0) setst("uhci: usb mouse");
                    else slog("enum failed");
                }
            }
        } else {
            if (!(inw(base + PORTSC + 2 * m_port) & 1)) {   // unplugged
                m_port = -1;
                qh_i.elem = 1;
                setst("uhci: mouse gone");
            } else if (!(itd->st & (1 << 23))) {
                if (itd->st & 0x760000) {
                    slog("mouse td error");
                    m_port = -1;
                    qh_i.elem = 1;
                    continue;
                }
                int b = rep[0] & 7, dx = (int8_t)rep[1], dy = (int8_t)rep[2];
                int dz = m_len >= 4 ? -(int8_t)rep[3] : 0;
                m_tog ^= 1;
                arm();
                mouse_feed(dx, -dy, dz, b);
            }
        }
        task_sleep_ms(4);
    }
}

void usb_init(void) {
    task_spawn("usbd", usb_task);
}
