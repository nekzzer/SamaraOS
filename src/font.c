#include "font.h"
#include "io.h"
#include "string.h"
#include "font_cp866.h"

static uint8_t font_data[256 * 16];

/* Overlay the Cyrillic CP866 glyphs (authored as ASCII art) onto the byte
   table. Called after font_init() captures the ROM font from VGA plane 2. */
static void install_cp866(void) {
    for (int g = 0; g < CP866_GLYPH_N; g++) {
        const cp866_glyph_t* gl = &cp866_glyphs[g];
        uint8_t* dst = &font_data[(uint32_t)gl->code * 16];
        for (int r = 0; r < 16; r++) {
            uint8_t row = 0;
            for (int c = 0; c < 8; c++) {
                char p = gl->art[r * 8 + c];
                if (p == '#') row |= (uint8_t)(0x80 >> c);
            }
            dst[r] = row;
        }
    }
}

static void     wseq(uint8_t i, uint8_t v) { outb(0x3C4, i); outb(0x3C5, v); }
static uint8_t  rseq(uint8_t i)            { outb(0x3C4, i); return inb(0x3C5); }
static void     wgc (uint8_t i, uint8_t v) { outb(0x3CE, i); outb(0x3CF, v); }
static uint8_t  rgc (uint8_t i)            { outb(0x3CE, i); return inb(0x3CF); }

void font_init(void) {
    /* Save state */
    uint8_t s2 = rseq(2);
    uint8_t s4 = rseq(4);
    uint8_t g4 = rgc(4);
    uint8_t g5 = rgc(5);
    uint8_t g6 = rgc(6);

    /* Switch to a config that exposes plane 2 (where the font is) at 0xA0000 */
    wseq(2, 0x04);          /* map mask: plane 2 */
    wseq(4, 0x07);          /* extended mem, no chain4, no oe */
    wgc (4, 0x02);          /* read map: plane 2 */
    wgc (5, 0x00);          /* read mode 0 */
    wgc (6, 0x04);          /* graphics mode, A0000-AFFFF, no oe */

    volatile const uint8_t* vram = (const uint8_t*)0xA0000;
    for (int ch = 0; ch < 256; ch++)
        for (int r = 0; r < 16; r++)
            font_data[ch * 16 + r] = vram[ch * 32 + r];

    /* Restore */
    wseq(2, s2);
    wseq(4, s4);
    wgc (4, g4);
    wgc (5, g5);
    wgc (6, g6);

    /* Now overlay our Cyrillic glyphs into the CP866 slots. The ROM font's
       CP437 characters in those ranges (mostly box-drawing) get replaced. */
    install_cp866();
}

const uint8_t* font_glyph(uint8_t c) {
    return &font_data[(uint32_t)c * 16];
}

void font_restore(void) {
    uint8_t s2 = rseq(2);
    uint8_t s4 = rseq(4);
    uint8_t g4 = rgc(4);
    uint8_t g5 = rgc(5);
    uint8_t g6 = rgc(6);

    wseq(2, 0x04);
    wseq(4, 0x07);
    wgc (4, 0x02);
    wgc (5, 0x00);
    wgc (6, 0x04);

    volatile uint8_t* vram = (uint8_t*)0xA0000;
    for (int ch = 0; ch < 256; ch++)
        for (int r = 0; r < 16; r++)
            vram[ch * 32 + r] = font_data[ch * 16 + r];

    wseq(2, s2);
    wseq(4, s4);
    wgc (4, g4);
    wgc (5, g5);
    wgc (6, g6);
}
