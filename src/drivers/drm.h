#ifndef SAMARA_DRM_H
#define SAMARA_DRM_H
#include "core/types.h"

/* /dev/dri/card0 (226:0) and renderD128 (226:128): the linux kms uapi on
   top of virtio-gpu or the bochs vga (qemu -vga std). the screen belongs to
   the master once it set a mode, console and wm keep their hands off */

struct drm_fd;

void  drm_init(void);                       /* after pci + fs, makes the nodes and /sys */
bool  drm_ready(void);
struct drm_fd* drm_open(bool render);
void  drm_close(struct drm_fd* d);
int   drm_ioctl(struct drm_fd* d, uint32_t req, void* arg);
bool  drm_readable(struct drm_fd* d);
int   drm_read(struct drm_fd* d, char* buf, uint32_t n);
int   drm_mmap(struct drm_fd* d, uint32_t pd, uint32_t va, uint32_t len, uint32_t off, bool rw);
bool  drm_active(void);                     /* a client drives the screen */

/* virtio-gpu backed console framebuffer for gfx.c (NULL when there is no gpu) */
uint8_t* drm_console_fb(int w, int h);
void  drm_console_flush(int x, int y, int w, int h);
bool  drm_virtio(void);
bool  drm_pref_size(int* w, int* h);

#endif
