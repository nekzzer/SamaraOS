#ifndef SAMARA_TERMFONT_H
#define SAMARA_TERMFONT_H
#include "core/types.h"

/* The terminal font: DejaVu Sans Mono (regular + bold) baked at several cell
   sizes by tools/mktermfont.py into termfont.bin, indexed by Unicode code
   point. Box drawing, blocks, braille and powerline symbols are drawn in
   code so they join up exactly at any size (like kitty does). */

#define TF_BOLD      0x01
#define TF_UNDERLINE 0x02
#define TF_STRIKE    0x04
#define TF_DIM       0x08
#define TF_ITALIC    0x10       /* accepted, drawn upright */
#define TF_CURSOR    0x80       /* draw the text cursor in this cell */

void     termfont_init(void);
int      termfont_sizes(void);            /* number of cell sizes */
void     termfont_set(int idx);           /* 0 = 8x16 ... */
int      termfont_get(void);
int      termfont_cw(void);               /* current cell size */
int      termfont_ch(void);
int      termfont_cw_of(int idx);
int      termfont_ch_of(int idx);
/* One cell at pixel x,y: background, glyph, decorations. */
void     termfont_cell(int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, uint8_t attr);

/* CP866 (the console's 8-bit charset) <-> Unicode */
uint32_t cp866_to_uni(uint8_t c);
int      uni_to_cp866(uint32_t cp);       /* -1 if not representable */

#endif
