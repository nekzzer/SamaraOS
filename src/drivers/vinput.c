#include "drivers/virtio.h"
#include "drivers/input.h"
#include "drivers/pci.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/vmm.h"

// virtio-input, modern only, polled from the usb task. events go to evdev as is

#define VI_MAX 4
#define NBUF 64

static struct {
    vm_t vm;
    vmq_t q;
    int id;
    uint8_t* buf;     // NBUF * 8, device writes events here
} vi[VI_MAX];
static int nvi;

// config space: select, subsel, size, pad[5], data at 8
static int vi_cfg(vm_t* v, uint8_t sel, uint8_t sub, void* out, int max) {
    v->dev[0] = sel;
    v->dev[1] = sub;
    __asm__ volatile ("" ::: "memory");
    int n = v->dev[2];
    if (n > max) n = max;
    for (int i = 0; i < n; i++) ((uint8_t*)out)[i] = v->dev[8 + i];
    return v->dev[2];
}

static void give(int i, int b) {
    vmq_t* q = &vi[i].q;
    q->desc[b].addr = V2P(vi[i].buf + b * 8);
    q->desc[b].addr_hi = 0;
    q->desc[b].len = 8;
    q->desc[b].flags = 2;
    q->avail[2 + q->avail[1] % q->n] = b;
    __asm__ volatile ("" ::: "memory");
    q->avail[1]++;
}

int vinput_up(void) {
    static pci_dev_t devs[32];
    int nd = pci_scan(devs, 32);
    for (int k = 0; k < nd && nvi < VI_MAX; k++) {
        pci_dev_t* p = &devs[k];
        if (p->vendor != 0x1AF4 || p->device != 0x1052) continue;
        int i = nvi;
        if (vm_probe(p, &vi[i].vm) < 0 || !vi[i].vm.dev) continue;
        if (vm_start(&vi[i].vm, 0, NULL) < 0) continue;
        if (vm_queue(&vi[i].vm, &vi[i].q, 0) < 0) continue;

        input_dev_t d;
        memset(&d, 0, sizeof d);
        vm_t* v = &vi[i].vm;
        vi_cfg(v, 1, 0, d.name, 47);
        if (!d.name[0]) strcpy(d.name, "Virtio Input");
        d.bus = 6;
        for (int t = 1; t < 32; t++) {
            uint8_t tmp[128];
            int n = vi_cfg(v, 0x11, t, tmp, 128);
            if (!n) continue;
            d.ev |= 1u << t;
            if (t == 1) memcpy(d.key, tmp, n > 96 ? 96 : n);
            if (t == 2) memcpy(&d.rel, tmp, n > 4 ? 4 : n);
            if (t == 3) memcpy(&d.abs, tmp, n > 4 ? 4 : n);
        }
        for (int a = 0; a < 8; a++) {
            if (!(d.abs & (1u << a))) continue;
            uint32_t ai[5];
            if (vi_cfg(v, 0x12, a, ai, 20) < 8) continue;
            d.absmin[a] = (int)ai[0];
            d.absmax[a] = (int)ai[1];
        }
        vi[i].buf = kmalloc(NBUF * 8);
        memset(vi[i].buf, 0, NBUF * 8);
        for (int b = 0; b < NBUF && b < vi[i].q.n; b++) give(i, b);
        vm_ready(v);
        vmq_kick(&vi[i].q, 0);
        vi[i].id = input_register(&d);
        if (vi[i].id < 0) continue;
        nvi++;
    }
    return nvi;
}

void vinput_poll(void) {
    for (int i = 0; i < nvi; i++) {
        vmq_t* q = &vi[i].q;
        int got = 0;
        while (*(volatile uint16_t*)(q->used + 1) != q->last_used) {
            uint32_t* e = (uint32_t*)(q->used + 2) + 2 * (q->last_used % q->n);
            int b = e[0];
            uint8_t* ev = vi[i].buf + b * 8;
            input_report(vi[i].id, *(uint16_t*)ev, *(uint16_t*)(ev + 2), *(int32_t*)(ev + 4));
            q->last_used++;
            give(i, b);
            got = 1;
        }
        if (got) vmq_kick(q, 0);
    }
}
