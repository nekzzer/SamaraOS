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

#endif
