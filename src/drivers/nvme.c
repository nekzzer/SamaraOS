#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/io.h"
#include "core/vmm.h"
#include "core/smp.h"

/* nvme, polled: admin queue + one io queue pair, one command at a time.
   prp1/prp2 or a prp list, buffers are kernel heap (physically flat) */

#define QD 32
#define MAX_SEC 256

static volatile uint8_t* regs;
static uint32_t dstrd;
static uint8_t *asq, *acq, *isq, *icq, *plist, *bounce;
static uint16_t atail, ahead, itail, ihead;
static uint8_t aph = 1, iph = 1;
static uint32_t max_sec = MAX_SEC;
static char model[41];
static spin_t nv_lk;

typedef struct {
    bool present;
    uint32_t nsid, shift;
    uint64_t sectors;
} ns_t;
static ns_t ns[NVME_MAX];
static int n_ns;

static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t*)(regs + off); }
static inline void w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(regs + off) = v; }
static inline uint64_t r64(uint32_t off) { return r32(off) | (uint64_t)r32(off + 4) << 32; }
static inline void w64(uint32_t off, uint64_t v) { w32(off, (uint32_t)v); w32(off + 4, (uint32_t)(v >> 32)); }

static void* page_alloc(uint32_t size) {
    uint8_t* raw = kmalloc(size + 4096);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(p, 0, size);
    return p;
}

static void sq_bell(int q, uint16_t t) { w32(0x1000 + (2 * q) * (4 << dstrd), t); }
static void cq_bell(int q, uint16_t h) { w32(0x1000 + (2 * q + 1) * (4 << dstrd), h); }

// returns status (0 = ok), -1 on timeout
static int submit(int q, uint32_t opc, uint32_t nsid, uint64_t prp1, uint64_t prp2, uint32_t d10, uint32_t d11, uint32_t d12) {
    uint8_t* sq = q ? isq : asq;
    uint8_t* cq = q ? icq : acq;
    uint16_t* tail = q ? &itail : &atail;
    uint16_t* head = q ? &ihead : &ahead;
    uint8_t* ph = q ? &iph : &aph;
    uint32_t* e = (uint32_t*)(sq + *tail * 64);
    memset(e, 0, 64);
    e[0] = opc | (*tail + 1) << 16;
    e[1] = nsid;
    *(uint64_t*)(e + 6) = prp1;
    *(uint64_t*)(e + 8) = prp2;
    e[10] = d10; e[11] = d11; e[12] = d12;
    *tail = (*tail + 1) % QD;
    __asm__ volatile ("" ::: "memory");
    sq_bell(q, *tail);
    volatile uint32_t* c = (volatile uint32_t*)(cq + *head * 16);
    for (uint32_t i = 0; i < 200000000; i++) {
        if (((c[3] >> 16) & 1) == *ph) {
            uint32_t st = c[3] >> 17;
            if (++*head == QD) { *head = 0; *ph ^= 1; }
            cq_bell(q, *head);
            return (int)st;
        }
        __asm__ volatile ("pause");
    }
    return -1;
}

static int admin(uint32_t opc, uint32_t nsid, uint64_t prp1, uint32_t d10, uint32_t d11) {
    return submit(0, opc, nsid, prp1, 0, d10, d11, 0);
}

static bool wait_rdy(uint32_t want) {
    for (int i = 0; i < 2000000; i++) {
        if ((r32(0x1c) & 1) == want) return true;
        io_wait();
    }
    return false;
}

static void cpstr(char* out, const uint8_t* s, int n) {
    memcpy(out, s, n);
    while (n > 0 && (out[n - 1] == ' ' || !out[n - 1])) n--;
    out[n] = 0;
}

int nvme_init(void) {
    n_ns = 0;
    pci_dev_t devs[32];
    int n = pci_scan(devs, 32);
    pci_dev_t* d = NULL;
    for (int i = 0; i < n; i++)
        if (devs[i].class_c == 1 && devs[i].subclass == 8 && devs[i].prog_if == 2) { d = &devs[i]; break; }
    if (!d) return 0;
    pci_enable_io_busmaster(d);
    uint64_t pa = d->bar[0] & ~0xfu;
    if ((d->bar[0] & 6) == 4) pa |= (uint64_t)d->bar[1] << 32;
    if (!pa) return 0;
    regs = mmio_map(pa, 0x2000);
    uint64_t cap = r64(0);
    dstrd = (cap >> 32) & 0xf;
    if (!(cap & (1ull << 37))) return 0;              // no nvm command set
    w32(0x14, 0);
    if (!wait_rdy(0)) return 0;
    asq = page_alloc(QD * 64); acq = page_alloc(QD * 16);
    isq = page_alloc(QD * 64); icq = page_alloc(QD * 16);
    plist = page_alloc(4096); bounce = kmalloc(MAX_SEC * 512 + 8);
    uint8_t* id = page_alloc(4096);
    if (!asq || !acq || !isq || !icq || !plist || !bounce || !id) return 0;
    w32(0x24, (QD - 1) << 16 | (QD - 1));
    w64(0x28, V2P(asq));
    w64(0x30, V2P(acq));
    w32(0x14, 1 | 6 << 16 | 4 << 20);                 // en, 64 byte sqe, 16 byte cqe, 4k pages
    if (!wait_rdy(1)) return 0;

    if (admin(6, 0, V2P(id), 1, 0)) return 0;
    cpstr(model, id + 24, 40);
    uint32_t mdts = id[77];
    if (mdts && (1u << mdts) * 8 < max_sec) max_sec = (1u << mdts) * 8;
    // io queues, qid 1
    if (admin(5, 0, V2P(icq), (QD - 1) << 16 | 1, 1)) return 0;
    if (admin(1, 0, V2P(isq), (QD - 1) << 16 | 1, 1 << 16 | 1)) return 0;

    for (uint32_t nsid = 1; nsid <= NVME_MAX; nsid++) {
        memset(id, 0, 4096);
        if (admin(6, nsid, V2P(id), 0, 0)) continue;
        uint64_t nsze = *(uint64_t*)id;
        if (!nsze) continue;
        uint32_t fl = id[26] & 0xf;
        uint32_t lbads = id[128 + fl * 4 + 2];
        if (lbads != 9 && lbads != 12) continue;
        ns_t* s = &ns[n_ns++];
        s->present = true;
        s->nsid = nsid;
        s->shift = lbads - 9;
        s->sectors = nsze << s->shift;
    }
    return n_ns;
}

bool nvme_present(int i) { return i >= 0 && i < n_ns && ns[i].present; }
uint32_t nvme_sectors(int i) {
    if (!nvme_present(i)) return 0;
    return ns[i].sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ns[i].sectors;
}
const char* nvme_model(void) { return model; }

static int xfer(int i, uint32_t lba, int count, void* buf, bool write) {
    if (!nvme_present(i) || count <= 0) return -1;
    ns_t* s = &ns[i];
    if ((uint64_t)lba + count > s->sectors) return -1;
    uint8_t* p = buf;
    uint8_t* bnc = NULL;
    if (((uintptr_t)p & 3) || !dma_ok(p)) bnc = (uint8_t*)(((uintptr_t)bounce + 7) & ~7ul);
    int rc = 0;
    uint64_t fl = spin_lock(&nv_lk);
    while (count > 0) {
        int chunk = count > (int)max_sec ? (int)max_sec : count;
        if (s->shift && ((lba | chunk) & ((1u << s->shift) - 1))) { rc = -1; break; }
        uint8_t* t = bnc ? bnc : p;
        if (bnc && write) memcpy(bnc, p, chunk * 512);
        uint64_t pa = V2P(t), prp2 = 0;
        uint32_t bytes = chunk * 512, first = 4096 - (pa & 4095);
        if (bytes > first) {
            if (bytes - first <= 4096) prp2 = pa + first;
            else {
                uint32_t np = (bytes - first + 4095) / 4096;
                uint64_t* l = (uint64_t*)plist;
                for (uint32_t k = 0; k < np; k++) l[k] = pa + first + k * 4096ull;
                prp2 = V2P(plist);
            }
        }
        uint64_t sl = (uint64_t)lba >> s->shift;
        uint32_t nlb = (chunk >> s->shift) - 1;
        if (submit(1, write ? 1 : 2, s->nsid, pa, prp2, (uint32_t)sl, (uint32_t)(sl >> 32), nlb)) { rc = -1; break; }
        if (bnc && !write) memcpy(p, bnc, chunk * 512);
        p += bytes; lba += chunk; count -= chunk;
    }
    if (write && rc == 0) submit(1, 0, s->nsid, 0, 0, 0, 0, 0);       // flush
    spin_unlock(&nv_lk, fl);
    return rc;
}

int nvme_read(int i, uint32_t lba, int count, void* buf) { return xfer(i, lba, count, buf, false); }
int nvme_write(int i, uint32_t lba, int count, const void* buf) { return xfer(i, lba, count, (void*)buf, true); }
