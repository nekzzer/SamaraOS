#ifndef SAMARA_VIRTIO_H
#define SAMARA_VIRTIO_H
#include "core/types.h"

/* virtio (legacy PCI, polled): virtio-blk disks and virtio-net cards. */

typedef struct { uint32_t addr, addr_hi; uint32_t len; uint16_t flags, next; } vq_desc_t;
typedef struct {
    uint16_t io;
    int idx, n, free_head, nfree;
    vq_desc_t* desc;
    uint16_t* avail;
    uint16_t* used;
    uint16_t last_used;
} vq_t;

int  vq_setup(vq_t* q, uint16_t io, int idx);
int  vq_alloc(vq_t* q, int k);
void vq_free(vq_t* q, int head);
void vq_set(vq_t* q, int d, void* p, uint32_t len, bool write, bool more);
void vq_push(vq_t* q, int head);
int  vq_pop(vq_t* q, uint32_t* len);
int  virtio_start(uint16_t io, uint32_t want, uint32_t* got);
void virtio_ready(uint16_t io);

/* modern (virtio 1.0) pci transport: caps point into mmio bars, split queues.
   only the gpu uses it so far, blk/net stay on the legacy io bar */
#include "drivers/pci.h"
typedef struct {
    volatile uint8_t *common, *isr, *dev, *notify;
    uint32_t mult;
} vm_t;
typedef struct {
    int n;
    vq_desc_t* desc;
    uint16_t *avail, *used;
    uint16_t last_used;
    volatile uint16_t* kick;
} vmq_t;
int  vm_probe(const pci_dev_t* d, vm_t* v);
int  vm_start(vm_t* v, uint32_t want, uint32_t* got);     /* want/got: feature bits 0..31, VERSION_1 is always taken */
int  vm_queue(vm_t* v, vmq_t* q, int idx);
void vm_ready(vm_t* v);
void vmq_kick(vmq_t* q, int idx);

#define VBLK_MAX 4
int      vblk_init(void);                  /* number of disks: /dev/vda.. */
bool     vblk_present(int i);
uint32_t vblk_sectors(int i);
int      vblk_read(int i, uint32_t lba, int count, void* buf);
int      vblk_write(int i, uint32_t lba, int count, const void* buf);

#define VNET_MAX 4
int  vnet_init(void);                      /* number of cards */
const uint8_t* vnet_mac(int i);
int  vnet_send(int i, const void* frame, int len);
int  vnet_recv(int i, void* buf, int max);

#endif
