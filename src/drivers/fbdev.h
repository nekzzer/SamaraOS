#ifndef SAMARA_FBDEV_H
#define SAMARA_FBDEV_H
#include "core/types.h"

/* /dev/fb0: exclusive access to the screen. While a process holds it open
   the window manager stops drawing; the screen is restored when it closes.
     write()            raw 32bpp pixels (XRGB) at the file offset
     FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO   (Linux layouts, partial)
     FBIO_BLIT8         scale an 8-bit paletted frame to the screen */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIO_BLIT8          0x4680

typedef struct {
    uint32_t pixels;     /* user pointer, w*h bytes */
    uint32_t w, h;
    uint32_t palette;    /* user pointer, 256 x 0x00RRGGBB */
} fb_blit8_t;

bool fbdev_open(void);
void fbdev_close(void);
bool fbdev_active(void);            /* true while a process owns the screen */
void fbdev_force_release(void);     /* ISR-safe: stop the owner from drawing */
void fbdev_restore(void);           /* put the saved screen back (idempotent) */

int  fbdev_write(uint32_t off, const char* buf, uint32_t n);
void fbdev_vscreeninfo(uint32_t* out /* 40 words */);
void fbdev_fscreeninfo(uint32_t* out /* 17 words */);
int  fbdev_blit8(const uint8_t* src, int w, int h, const uint32_t* pal);

#endif
