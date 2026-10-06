#ifndef SAMARA_VGPU_H
#define SAMARA_VGPU_H
#include "core/types.h"

/* virtio-gpu, 2d only (modern transport, polled). resources are ids the
   caller picks, backing memory comes as a list of physical pages */

#define VG_FMT_XRGB 2          /* B8G8R8X8: drm XRGB8888 on little endian */
#define VG_FMT_ARGB 1          /* B8G8R8A8 */

bool vg_init(void);
bool vg_present(void);
int  vg_pref_w(void);          /* what the host window wants, 0 = nothing enabled */
int  vg_pref_h(void);
bool vg_events(void);          /* host changed the display: info refreshed, true once */
int  vg_edid(uint8_t* buf);    /* bytes, 0 if the host has none */

int  vg_create(uint32_t res, uint32_t fmt, uint32_t w, uint32_t h);
int  vg_attach(uint32_t res, const uintptr_t* pages, int n);   /* page addrs, 4k each */
int  vg_unref(uint32_t res);
int  vg_scanout(uint32_t res, uint32_t w, uint32_t h);           /* res 0 turns it off */
int  vg_xfer(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pitch);
int  vg_flush(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  vg_cursor(uint32_t res, int x, int y, int hx, int hy);      /* update: new image + pos */
int  vg_cursor_move(uint32_t res, int x, int y);


/* virgl 3d (VIRTIO_GPU_F_VIRGL), all synchronous */
bool vg_virgl(void);
int  vg_ncaps(void);
int  vg_ctx_create(uint32_t ctx, uint32_t init);          /* init = capset id for venus */
int  vg_ctx_destroy(uint32_t ctx);
int  vg_ctx_attach(uint32_t ctx, uint32_t res);
int  vg_create3d(uint32_t res, const uint32_t* p);      /* target fmt bind w h depth array last nsamples flags */
int64_t vg_xfer3d(uint32_t ctx, uint32_t res, bool to_host, const uint32_t* box, uint64_t off, uint32_t level, uint32_t stride, uint32_t lstride);
int64_t vg_submit(uint32_t ctx, const void* buf, uint32_t size, int ring);
uint64_t vg_done(void);                 /* reap, last finished fence */
bool vg_wait(uint64_t f);
bool vg_fence_done(uint64_t f);
bool vg_venus(void);
void vg_idle(void);
uint64_t vg_shm(void);
uint64_t vg_shm_len(void);
int  vg_blob(uint32_t ctx, uint32_t res, uint32_t mem, uint32_t flags, uint64_t id, uint64_t size, const uintptr_t* pages, int n);
int  vg_blob_map(uint32_t res, uint64_t off);
int  vg_blob_unmap(uint32_t res);
int  vg_capset_info(uint32_t idx, uint32_t* id, uint32_t* ver, uint32_t* size);
int  vg_capset(uint32_t id, uint32_t ver, uint8_t* out, uint32_t max);

int64_t vg_flush_async(uint32_t res, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#endif
