#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "core/io.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/vmm.h"
#include "drivers/vga.h"

/* virtio over the legacy PCI interface (BAR0 = I/O ports), which QEMU's
   virtio-*-pci devices still offer. Polled, no interrupts: we kick the
   queue and watch the used ring, same as the rest of the drivers here.
   Memory is identity mapped, so buffer addresses go to the device as is. */

#define VIO_DEVF   0x00
#define VIO_GUESTF 0x04
#define VIO_QADDR  0x08
#define VIO_QSIZE  0x0C
#define VIO_QSEL   0x0E
#define VIO_QNOTIFY 0x10
#define VIO_STATUS 0x12
#define VIO_ISR    0x13
#define VIO_CFG    0x14

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_OK 4
#define ST_FAIL 128

#define VQ_NEXT  1
#define VQ_WRITE 2

/* the big heap lives in the direct map above 1 GB: the device wants physical */
static uint32_t pa(void* p) { return (uint32_t)V2P(p); }

/* the device's register block and one queue set up */
int vq_setup(vq_t* q, uint16_t io, int idx) {
    outw(io + VIO_QSEL, (uint16_t)idx);
    int n = inw(io + VIO_QSIZE);
    if (n <= 0 || n > 1024) return -1;
    /* desc + avail, page aligned used ring after (legacy layout) */
    uint32_t a = (uint32_t)(16 * n + 6 + 2 * n);
    a = (a + 4095) & ~4095u;
    uint32_t sz = a + (uint32_t)(6 + 8 * n);
    uint8_t* raw = kmalloc(sz + 4096);
    if (!raw) return -1;
    uint8_t* m = (uint8_t*)(((uintptr_t)raw + 4095) & ~4095ul);
    memset(m, 0, sz);
    q->io = io; q->idx = idx; q->n = n;
    q->desc = (vq_desc_t*)m;
    q->avail = (uint16_t*)(m + 16 * n);          /* flags, idx, ring[n], used_event */
    q->used = (uint16_t*)(m + a);                /* flags, idx, then {id,len} pairs */
    q->last_used = 0;
    q->free_head = 0;
    for (int i = 0; i < n; i++) q->desc[i].next = (uint16_t)(i + 1);
    q->nfree = n;
    outl(io + VIO_QADDR, pa(m) >> 12);
    return n;
}

/* takes a free descriptor chain of k entries; -1 if not enough */
int vq_alloc(vq_t* q, int k) {
    if (q->nfree < k) return -1;
    int h = q->free_head, d = h;
    for (int i = 0; i < k - 1; i++) d = q->desc[d].next;
    q->free_head = q->desc[d].next;
    q->nfree -= k;
    return h;
}

void vq_free(vq_t* q, int head) {
    int d = head, k = 1;
    while (q->desc[d].flags & VQ_NEXT) { d = q->desc[d].next; k++; }
    q->desc[d].next = (uint16_t)q->free_head;
    q->free_head = head;
    q->nfree += k;
}

void vq_set(vq_t* q, int d, void* p, uint32_t len, bool write, bool more) {
    q->desc[d].addr = pa(p);
    q->desc[d].addr_hi = 0;
    q->desc[d].len = len;
    q->desc[d].flags = (uint16_t)((write ? VQ_WRITE : 0) | (more ? VQ_NEXT : 0));
}

void vq_push(vq_t* q, int head) {
    uint16_t* ring = q->avail + 2;
    uint16_t i = q->avail[1];
    ring[i % q->n] = (uint16_t)head;
    __asm__ volatile ("" ::: "memory");
    q->avail[1] = (uint16_t)(i + 1);
    __asm__ volatile ("" ::: "memory");
    outw(q->io + VIO_QNOTIFY, (uint16_t)q->idx);
}

/* next finished chain: head index, or -1 */
int vq_pop(vq_t* q, uint32_t* len) {
    __asm__ volatile ("" ::: "memory");
    if (q->used[1] == q->last_used) return -1;
    uint32_t* e = (uint32_t*)(q->used + 2) + 2 * (q->last_used % q->n);
    if (len) *len = e[1];
    q->last_used++;
    return (int)e[0];
}

/* reset, ack, take the features we can live with; 0 on success */
int virtio_start(uint16_t io, uint32_t want, uint32_t* got) {
    outb(io + VIO_STATUS, 0);
    outb(io + VIO_STATUS, ST_ACK);
    outb(io + VIO_STATUS, ST_ACK | ST_DRIVER);
    uint32_t f = inl(io + VIO_DEVF) & want;
    outl(io + VIO_GUESTF, f);
    if (got) *got = f;
    return 0;
}
void virtio_ready(uint16_t io) { outb(io + VIO_STATUS, ST_ACK | ST_DRIVER | ST_OK); }

/* ---------- virtio-blk ---------- */

typedef struct { uint32_t type, prio; uint32_t sec_lo, sec_hi; } blk_hdr_t;

static struct {
    bool present;
    uint16_t io;
    uint32_t sectors;
    vq_t q;
    blk_hdr_t hdr;
    volatile uint8_t st;
} vb[VBLK_MAX];
static int n_vb;

int vblk_init(void) {
    static pci_dev_t devs[32];
    int nd = pci_scan(devs, 32);
    for (int i = 0; i < nd && n_vb < VBLK_MAX; i++) {
        pci_dev_t* d = &devs[i];
        if (d->vendor != 0x1AF4 || (d->device != 0x1001 && d->device != 0x1042)) continue;
        if (!(d->bar[0] & 1)) continue;                 /* modern-only device: not for us */
        pci_enable_io_busmaster(d);
        uint16_t io = (uint16_t)(d->bar[0] & ~3u);
        virtio_start(io, 0, NULL);
        if (vq_setup(&vb[n_vb].q, io, 0) < 3) continue;
        virtio_ready(io);
        vb[n_vb].io = io;
        uint32_t lo = inl(io + VIO_CFG), hi = inl(io + VIO_CFG + 4);
        vb[n_vb].sectors = hi ? 0xFFFFFFFFu : lo;       /* 2 TB is plenty */
        vb[n_vb].present = true;
        n_vb++;
    }
    return n_vb;
}

bool     vblk_present(int i) { return i >= 0 && i < n_vb && vb[i].present; }
uint32_t vblk_sectors(int i) { return vblk_present(i) ? vb[i].sectors : 0; }

static int vblk_rw(int i, uint32_t lba, int count, void* buf, bool wr) {
    if (!vblk_present(i) || count <= 0) return -1;
    uint8_t* p = buf;
    while (count > 0) {
        int chunk = count > 128 ? 128 : count;            /* 64 KB per request */
        uint32_t f = irq_save();
        vq_t* q = &vb[i].q;
        int h = vq_alloc(q, 3);
        if (h < 0) { irq_restore(f); return -1; }
        int d1 = q->desc[h].next, d2 = q->desc[d1].next;
        vb[i].hdr.type = wr ? 1 : 0;
        vb[i].hdr.prio = 0;
        vb[i].hdr.sec_lo = lba; vb[i].hdr.sec_hi = 0;
        vb[i].st = 0xFF;
        vq_set(q, h, &vb[i].hdr, sizeof vb[i].hdr, false, true);
        q->desc[h].next = (uint16_t)d1;
        vq_set(q, d1, p, (uint32_t)chunk * 512, !wr, true);
        q->desc[d1].next = (uint16_t)d2;
        vq_set(q, d2, (void*)&vb[i].st, 1, true, false);
        vq_push(q, h);
        /* wait as long as it takes. there was a 50M spin limit: under load
           (gtk qemu, 512k reads) a request outlived it, we called it an error,
           leaked its descriptors and reused the shared header for the next
           one - the device then put the old request's data at the new
           request's sector. that's what kept smashing the root disk */
        int got;
        while ((got = vq_pop(q, NULL)) < 0) __asm__ volatile ("pause");
        vq_free(q, got);
        irq_restore(f);
        if (vb[i].st != 0) return -1;
        p += chunk * 512; lba += (uint32_t)chunk; count -= chunk;
    }
    return 0;
}

int vblk_read(int i, uint32_t lba, int count, void* buf) { return vblk_rw(i, lba, count, buf, false); }
int vblk_write(int i, uint32_t lba, int count, const void* buf) { return vblk_rw(i, lba, count, (void*)buf, true); }

/* ---------- virtio-net ---------- */

#define RXN 64
#define FRAME 1536
typedef struct { uint8_t flags, gso; uint16_t hlen, gsz, cs, co; } net_hdr_t;   /* 10 bytes legacy */

static struct {
    uint16_t io;
    uint8_t mac[6];
    vq_t rx, tx;
    uint8_t* rxbuf;                 /* RXN * FRAME */
    net_hdr_t* rxh;                 /* RXN headers */
    uint8_t* txbuf;                 /* one frame per tx descriptor */
    net_hdr_t* txh;
    int rxhead[RXN];                /* chain head -> slot */
} vn[VNET_MAX];
static int n_vn;

static void rx_post(int i, int slot) {
    vq_t* q = &vn[i].rx;
    int h = vq_alloc(q, 2);
    if (h < 0) return;
    int d = q->desc[h].next;
    vq_set(q, h, &vn[i].rxh[slot], 10, true, true);
    q->desc[h].next = (uint16_t)d;
    vq_set(q, d, vn[i].rxbuf + slot * FRAME, FRAME, true, false);
    vn[i].rxhead[slot] = h;
    vq_push(q, h);
}

int vnet_init(void) {
    static pci_dev_t devs[32];
    int nd = pci_scan(devs, 32);
    for (int k = 0; k < nd && n_vn < VNET_MAX; k++) {
        pci_dev_t* d = &devs[k];
        if (d->vendor != 0x1AF4 || (d->device != 0x1000 && d->device != 0x1041)) continue;
        if (!(d->bar[0] & 1)) continue;
        pci_enable_io_busmaster(d);
        uint16_t io = (uint16_t)(d->bar[0] & ~3u);
        int i = n_vn;
        virtio_start(io, 1u << 5, NULL);                  /* MAC in config */
        if (vq_setup(&vn[i].rx, io, 0) < 4 || vq_setup(&vn[i].tx, io, 1) < 2) continue;
        vn[i].io = io;
        for (int b = 0; b < 6; b++) vn[i].mac[b] = inb(io + VIO_CFG + b);
        vn[i].rxbuf = kmalloc(RXN * FRAME);
        vn[i].rxh = kmalloc(RXN * sizeof(net_hdr_t));
        vn[i].txbuf = kmalloc((uint32_t)vn[i].tx.n * FRAME);
        vn[i].txh = kmalloc((uint32_t)vn[i].tx.n * sizeof(net_hdr_t));
        if (!vn[i].rxbuf || !vn[i].rxh || !vn[i].txbuf || !vn[i].txh) continue;
        virtio_ready(io);
        n_vn++;
        for (int s = 0; s < RXN && s < vn[i].rx.n / 2; s++) rx_post(i, s);
    }
    return n_vn;
}

const uint8_t* vnet_mac(int i) { return vn[i].mac; }

int vnet_send(int i, const void* frame, int len) {
    if (i < 0 || i >= n_vn || len > FRAME) return -1;
    vq_t* q = &vn[i].tx;
    int h;
    uint32_t l;
    while ((h = vq_pop(q, &l)) >= 0) vq_free(q, h);   /* sent ones back */
    h = vq_alloc(q, 2);
    if (h < 0) return -1;
    int slot = h, d = q->desc[h].next;
    memset(&vn[i].txh[slot], 0, sizeof(net_hdr_t));
    memcpy(vn[i].txbuf + slot * FRAME, frame, (size_t)len);
    vq_set(q, h, &vn[i].txh[slot], 10, false, true);
    q->desc[h].next = (uint16_t)d;
    vq_set(q, d, vn[i].txbuf + slot * FRAME, (uint32_t)len, false, false);
    vq_push(q, h);
    return 0;
}

int vnet_recv(int i, void* buf, int max) {
    if (i < 0 || i >= n_vn) return 0;
    vq_t* q = &vn[i].rx;
    uint32_t len;
    int h = vq_pop(q, &len);
    if (h < 0) return 0;
    int slot = -1;
    for (int s = 0; s < RXN; s++) if (vn[i].rxhead[s] == h) { slot = s; break; }
    vq_free(q, h);
    if (slot < 0) return 0;
    int n = (int)len - 10;
    if (n > max) n = max;
    if (n > 0) memcpy(buf, vn[i].rxbuf + slot * FRAME, (size_t)n);
    rx_post(i, slot);
    return n > 0 ? n : 0;
}

/* ---------- modern transport ---------- */

#define VM_COMMON 1
#define VM_NOTIFY 2
#define VM_ISR    3
#define VM_DEVCFG 4

/* seabios drops the small bars above 512G when hostmem eats the top of the hole, and the cpu only has 39 bits.
   move them down ourselves */
static void bar_fix(pci_dev_t* d) {
    static uint64_t nxt = 0x4000000000ull;
    for (int i = 0; i < 5; i++) {
        uint32_t lo = d->bar[i];
        if ((lo & 7) != 4 || (uint64_t)d->bar[i + 1] < 0x80) continue;      // 64 bit mem, bit 39 is bit 7 of the high dword
        uint16_t cmd = pci_cfg_read16(d->bus, d->dev, d->fn, 0x04);
        pci_cfg_write16(d->bus, d->dev, d->fn, 0x04, cmd & ~7);
        pci_cfg_write32(d->bus, d->dev, d->fn, 0x10 + i * 4, 0xffffffff);
        uint32_t sz = ~(pci_cfg_read32(d->bus, d->dev, d->fn, 0x10 + i * 4) & ~0xfu) + 1;
        nxt = (nxt + sz - 1) & ~(uint64_t)(sz - 1);
        pci_cfg_write32(d->bus, d->dev, d->fn, 0x10 + i * 4, (uint32_t)nxt | (lo & 0xf));
        pci_cfg_write32(d->bus, d->dev, d->fn, 0x14 + i * 4, (uint32_t)(nxt >> 32));
        d->bar[i] = (uint32_t)nxt | (lo & 0xf);
        d->bar[i + 1] = (uint32_t)(nxt >> 32);
        nxt += sz;
        pci_cfg_write16(d->bus, d->dev, d->fn, 0x04, cmd);
        i++;
    }
}

int vm_probe(const pci_dev_t* d0, vm_t* v) {
    pci_dev_t dd = *d0;
    const pci_dev_t* d = &dd;
    bar_fix(&dd);
    memset(v, 0, sizeof(*v));
    uint16_t st = pci_cfg_read16(d->bus, d->dev, d->fn, 0x06);
    if (!(st & 0x10)) return -1;                      /* no cap list */
    uint8_t c = pci_cfg_read8(d->bus, d->dev, d->fn, 0x34) & ~3;
    int hops = 0;
    while (c && hops++ < 48) {
        uint8_t id = pci_cfg_read8(d->bus, d->dev, d->fn, c);
        uint8_t next = pci_cfg_read8(d->bus, d->dev, d->fn, c + 1);
        if (id == 9) {
            uint8_t type = pci_cfg_read8(d->bus, d->dev, d->fn, c + 3);
            uint8_t bar = pci_cfg_read8(d->bus, d->dev, d->fn, c + 4);
            uint32_t off = pci_cfg_read32(d->bus, d->dev, d->fn, c + 8);
            uint32_t len = pci_cfg_read32(d->bus, d->dev, d->fn, c + 12);
            if (type == 8 && pci_cfg_read8(d->bus, d->dev, d->fn, c + 5) == 1 && bar < 5) {
                // not mapped here, user mmap goes straight to the pages
                uint64_t base = d->bar[bar] & ~0xFu;
                if ((d->bar[bar] & 6) == 4) base |= (uint64_t)d->bar[bar + 1] << 32;
                v->shm = base + off + ((uint64_t)pci_cfg_read32(d->bus, d->dev, d->fn, c + 16) << 32);
                v->shm_len = len | ((uint64_t)pci_cfg_read32(d->bus, d->dev, d->fn, c + 20) << 32);
            }
            if (type >= 1 && type <= 4 && bar < 6 && !(d->bar[bar] & 1)) {
                uint64_t base = d->bar[bar] & ~0xFu;
                if ((d->bar[bar] & 6) == 4 && bar < 5) base |= (uint64_t)d->bar[bar + 1] << 32;
                volatile uint8_t* p = mmio_map(base + off, len);
                if (!p) { vga_printf("virtio: bar%d unmappable\n", bar); return -1; }
                if (type == VM_COMMON) v->common = p;
                else if (type == VM_NOTIFY) { v->notify = p; v->mult = pci_cfg_read32(d->bus, d->dev, d->fn, c + 16); }
                else if (type == VM_ISR) v->isr = p;
                else if (type == VM_DEVCFG) v->dev = p;
            }
        }
        c = next & ~3;
    }
    if (!v->common || !v->notify || !v->isr) return -1;
    uint16_t cmd = pci_cfg_read16(d->bus, d->dev, d->fn, 0x04);
    pci_cfg_write16(d->bus, d->dev, d->fn, 0x04, cmd | 6);      /* mem + bus master */
    return 0;
}

#define MC32(o) (*(volatile uint32_t*)(v->common + (o)))
#define MC16(o) (*(volatile uint16_t*)(v->common + (o)))
#define MC8(o)  (*(volatile uint8_t*)(v->common + (o)))

int vm_start(vm_t* v, uint32_t want, uint32_t* got) {
    MC8(0x14) = 0;
    while (MC8(0x14)) {}
    MC8(0x14) = ST_ACK;
    MC8(0x14) = ST_ACK | ST_DRIVER;
    MC32(0x00) = 0;
    uint32_t lo = MC32(0x04) & want;
    MC32(0x00) = 1;
    if (!(MC32(0x04) & 1)) return -1;                 /* not a 1.0 device */
    MC32(0x08) = 0; MC32(0x0C) = lo;
    MC32(0x08) = 1; MC32(0x0C) = 1;                   /* VERSION_1 */
    MC8(0x14) = ST_ACK | ST_DRIVER | 8;               /* FEATURES_OK */
    if (!(MC8(0x14) & 8)) return -1;
    if (got) *got = lo;
    return 0;
}

int vm_queue(vm_t* v, vmq_t* q, int idx) {
    MC16(0x16) = (uint16_t)idx;
    int n = MC16(0x18);
    if (n <= 0) return -1;
    if (n > 256) { n = 256; MC16(0x18) = 256; }
    uint8_t* m = kmalloc(4096 * 4);
    if (!m) return -1;
    m = (uint8_t*)(((uintptr_t)m + 4095) & ~(uintptr_t)4095);   // leaks 4k, who cares, queues live forever
    memset(m, 0, 4096 * 3);
    q->n = n;
    q->desc = (vq_desc_t*)m;
    q->avail = (uint16_t*)(m + 4096);
    q->used = (uint16_t*)(m + 8192);
    q->last_used = 0;
    uint64_t d = V2P(q->desc), a = V2P(q->avail), u = V2P(q->used);
    MC32(0x20) = (uint32_t)d; MC32(0x24) = (uint32_t)(d >> 32);
    MC32(0x28) = (uint32_t)a; MC32(0x2C) = (uint32_t)(a >> 32);
    MC32(0x30) = (uint32_t)u; MC32(0x34) = (uint32_t)(u >> 32);
    q->kick = (volatile uint16_t*)(v->notify + MC16(0x1E) * v->mult);
    MC16(0x1C) = 1;
    return n;
}

void vm_ready(vm_t* v) { MC8(0x14) = ST_ACK | ST_DRIVER | 8 | ST_OK; }

void vmq_kick(vmq_t* q, int idx) { *q->kick = (uint16_t)idx; }
