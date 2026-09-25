#ifndef SAMARA_UIFONT_H
#define SAMARA_UIFONT_H
/* Anti-aliased desktop fonts (Golos Text), pre-rasterized by tools/mkfont.py.
   Strings are CP866 like the rest of the OS. y is the TOP of the line box
   (ascent + descent tall), not the baseline. */
#include "core/types.h"

typedef enum {
    UIF_REG,        /* 15px regular — body text, menus            */
    UIF_MED,        /* 15px semibold — titles, labels             */
    UIF_SMALL,      /* 12px medium — hints, dates, captions       */
    UIF_BIG,        /* 20px bold — headings                       */
    UIF_HUGE,       /* 84px extrabold — wordmark ("samarOS" only) */
    UIF_COUNT
} uif_t;

typedef struct { int8_t x, y; uint8_t w, h; uint16_t adv; uint32_t off; } uif_glyph_t;
typedef struct { int ascent, descent; const uif_glyph_t* gl; const uint8_t* px; } uif_face_t;

int  uif_height(uif_t f);                          /* line box height */
int  uif_width(uif_t f, const char* s);
int  uif_width_n(uif_t f, const char* s, int n);
/* Draws s, returns the x just past the last glyph. */
int  uif_draw(int x, int y, uif_t f, const char* s, uint32_t color);
/* Like uif_draw but clipped to max_w with a trailing "..." if needed. */
void uif_draw_fit(int x, int y, uif_t f, const char* s, int max_w, uint32_t color);
/* Vertical placement by the middle of the capital letters: y chosen so caps
   sit centred on mid_y (what buttons and bars want, independent of size). */
int  uif_top_for_mid(uif_t f, int mid_y);
int  uif_draw_mid(int x, int mid_y, uif_t f, const char* s, uint32_t color);
const uif_face_t* uif_face(uif_t f);
/* Horizontally centred in [x, x+w), vertically centred in [y, y+h). */
void uif_draw_center(int x, int y, int w, int h, uif_t f, const char* s, uint32_t color);
/* Word-wrapped paragraph; '\n' breaks lines. Returns the y after the text. */
int  uif_draw_wrap(int x, int y, int max_w, int line_h, uif_t f, const char* s, uint32_t color);

/* Render into a plain XRGB buffer (user windows). font may be UIF_MONO for
   the 8x16 terminal face. Returns the pen x after the text. */
#define UIF_MONO 16
int  uif_draw_mem(uint32_t* buf, int bw, int bh, int x, int y, int font,
                  const char* s, uint32_t color);
int  uif_width_any(int font, const char* s);
int  uif_height_any(int font);

/* Terminal cell (8x16) from the anti-aliased mono font; falls back to the
   VGA bitmap for box drawing and control glyphs. */
void uif_mono_cell(int x, int y, uint8_t c, uint32_t fg, uint32_t bg);

#endif
