#include "drivers/usb.h"
#include "drivers/xhci.h"
#include "drivers/ata.h"
#include "core/string.h"
#include "core/task.h"

void slog(const char* s);
void part_scan(int disk);
void fs_add_disk_nodes(void);

static struct { xdev_t* d; int ei, eo, ok; uint32_t sectors; } ms[USB_DISKS];
static volatile int busy;
static uint32_t tag;

static void lock(void) { while (busy) task_yield(); busy = 1; }

static int bot(int i, bool in, const uint8_t* cb, int cbl, void* data, int len) {
    uint8_t w[31];
    uint8_t csw[13];
    memset(w, 0, 31);
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    tag++;
    memcpy(w + 4, &tag, 4);
    memcpy(w + 8, &len, 4);
    w[12] = in ? 0x80 : 0;
    w[14] = cbl;
    memcpy(w + 15, cb, cbl);
    int r = xhci_bulk(ms[i].d, ms[i].eo, false, w, 31);
    if (r == -6) { xhci_unstall(ms[i].d, ms[i].eo); return -1; }
    if (r < 0) return -1;
    int got = 0;
    if (len) {
        got = xhci_bulk(ms[i].d, in ? ms[i].ei : ms[i].eo, in, data, len);
        if (got == -6) xhci_unstall(ms[i].d, in ? ms[i].ei : ms[i].eo);
        else if (got < 0) return -1;
    }
    r = xhci_bulk(ms[i].d, ms[i].ei, true, csw, 13);
    if (r == -6) {
        xhci_unstall(ms[i].d, ms[i].ei);
        r = xhci_bulk(ms[i].d, ms[i].ei, true, csw, 13);
    }
    if (r != 13 || csw[0] != 'U' || csw[1] != 'S') return -1;
    if (csw[12]) return -2;                       /* command failed, sense says why */
    return got < 0 ? -1 : 0;
}

static int sense(int i) {
    uint8_t cb[6] = { 3, 0, 0, 0, 18, 0 }, s[18];
    if (bot(i, true, cb, 6, s, 18) < 0) return -1;
    return s[2] & 15;
}

static int scsi(int i, bool in, const uint8_t* cb, int cbl, void* data, int len) {
    for (int t = 0; t < 3; t++) {
        int r = bot(i, in, cb, cbl, data, len);
        if (r == 0) return 0;
        if (r == -2) { sense(i); task_sleep_ms(20); continue; }
        return -1;
    }
    return -1;
}

void usbms_attach(xdev_t* d, int ein, int eout) {
    int i;
    for (i = 0; i < USB_DISKS && ms[i].d; i++);
    if (i == USB_DISKS) return;
    lock();
    ms[i].d = d; ms[i].ei = ein; ms[i].eo = eout; ms[i].ok = 0;
    uint8_t cb[10], buf[36];
    memset(cb, 0, 10);
    cb[0] = 0x12; cb[4] = 36;
    if (scsi(i, true, cb, 6, buf, 36) < 0) { slog("ms: inquiry failed"); goto bad; }
    int ready = 0;
    for (int t = 0; t < 20 && !ready; t++) {
        memset(cb, 0, 10);
        if (scsi(i, false, cb, 6, 0, 0) == 0) ready = 1;
        else task_sleep_ms(50);
    }
    if (!ready) { slog("ms: not ready"); goto bad; }
    uint8_t cap[8];
    memset(cb, 0, 10);
    cb[0] = 0x25;
    if (scsi(i, true, cb, 10, cap, 8) < 0) goto bad;
    uint32_t last = cap[0] << 24 | cap[1] << 16 | cap[2] << 8 | cap[3];
    uint32_t bs = cap[4] << 24 | cap[5] << 16 | cap[6] << 8 | cap[7];
    if (bs != 512) { slog("ms: not 512b blocks"); goto bad; }
    ms[i].sectors = last + 1;
    ms[i].ok = 1;
    busy = 0;
    slog("usb disk up");
    ata_usb_added(i);
    return;
bad:
    ms[i].d = 0;
    busy = 0;
}

void usbms_detach(xdev_t* d) {
    for (int i = 0; i < USB_DISKS; i++)
        if (ms[i].d == d) { ms[i].ok = 0; ms[i].d = 0; }
}

bool usbms_present(int i) { return i >= 0 && i < USB_DISKS && ms[i].ok; }
uint32_t usbms_sectors(int i) { return ms[i].sectors; }

static int rw(int i, bool wr, uint32_t lba, int count, uint8_t* buf) {
    if (!usbms_present(i)) return -1;
    lock();
    int ret = 0;
    while (count > 0 && ret == 0) {
        int n = count > 128 ? 128 : count;
        uint8_t cb[10] = { wr ? 0x2A : 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, n >> 8, n, 0 };
        if (!usbms_present(i) || scsi(i, !wr, cb, 10, buf, n * 512) < 0) ret = -1;
        lba += n; count -= n; buf += n * 512;
    }
    busy = 0;
    return ret;
}

int usbms_read(int i, uint32_t lba, int count, void* buf) { return rw(i, false, lba, count, buf); }
int usbms_write(int i, uint32_t lba, int count, const void* buf) { return rw(i, true, lba, count, (uint8_t*)buf); }
