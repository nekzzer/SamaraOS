#ifndef SAMARA_FONT_H
#define SAMARA_FONT_H
#include "types.h"

#define FONT_W 8
#define FONT_H 16

/* Must be called BEFORE switching out of text mode (font lives in VGA plane 2). */
void font_init(void);

/* Write the captured font back into VGA plane 2 — call after returning to text mode. */
void font_restore(void);

/* Returns 16 bytes: one per row, MSB = leftmost pixel. */
const uint8_t* font_glyph(uint8_t c);

#endif
