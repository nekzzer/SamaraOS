#ifndef SAMARA_GFX_H
#define SAMARA_GFX_H
#include "types.h"
#include "multiboot.h"

#define RGB(r,g,b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

bool     gfx_init(multiboot_info_t* mbi);     /* true if framebuffer ready */
bool     gfx_init_vbe(int w, int h, int bpp); /* Bochs VBE — used by QEMU stdvga */
bool     gfx_init_mode13h(void);              /* fallback: 320x200 indexed */
bool     gfx_ready(void);

/* Double buffering — render scene to back buffer, then present atomically */
bool     gfx_enable_double_buffer(void);      /* allocates back buffer */
void     gfx_disable_double_buffer(void);     /* frees, restores direct draw */
void     gfx_target_back(void);               /* subsequent draws -> back buffer */
void     gfx_target_front(void);              /* subsequent draws -> real FB */
void     gfx_present(void);                   /* memcpy back -> front */
int      gfx_w(void);
int      gfx_h(void);

void     gfx_clear(uint32_t color);
void     gfx_pixel(int x, int y, uint32_t color);
uint32_t gfx_get_pixel(int x, int y);
void     gfx_rect_fill(int x, int y, int w, int h, uint32_t color);
void     gfx_rect(int x, int y, int w, int h, uint32_t color);
void     gfx_line(int x0, int y0, int x1, int y1, uint32_t color);
void     gfx_circle(int cx, int cy, int r, uint32_t color);
void     gfx_disc(int cx, int cy, int r, uint32_t color);
void     gfx_glyph(int x, int y, char c, uint32_t fg, uint32_t bg, bool draw_bg);
void     gfx_string(int x, int y, const char* s, uint32_t fg, uint32_t bg, bool draw_bg);

void     gfx_save_rect(int x, int y, int w, int h, uint32_t* dst);
void     gfx_restore_rect(int x, int y, int w, int h, const uint32_t* src);

/* Fast 32-bit RGB blit: copies src[src_w*src_h] (row-major) to current target
   at (dst_x, dst_y). On 32bpp framebuffers uses a row-wise memcpy; on other
   bit depths falls back to per-pixel gfx_pixel. */
void     gfx_blit_argb(int dst_x, int dst_y, int src_w, int src_h, const uint32_t* src);

#endif
