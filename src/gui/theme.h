#ifndef SAMARA_THEME_H
#define SAMARA_THEME_H
/* Desktop palette shared by the window manager and apps. */
#include "gfx/gfx.h"

/* ========================================================================
   Palette: graphite desk, warm paper windows, one amber accent.
   ======================================================================== */

#define C_DESK_TOP    RGB(0x17, 0x19, 0x1D)
#define C_DESK_BOT    RGB(0x23, 0x26, 0x2C)
#define C_WORDMARK    RGB(0x2B, 0x2F, 0x36)

#define C_SURFACE     RGB(0xEE, 0xEB, 0xE5)
#define C_INK         RGB(0x1D, 0x1E, 0x21)
#define C_INK_DIM     RGB(0x8C, 0x88, 0x80)
#define C_GLYPH       RGB(0x6E, 0x6A, 0x62)
#define C_GLYPH_DIM   RGB(0xB4, 0xAF, 0xA5)
#define C_RULE        RGB(0xD4, 0xCF, 0xC6)
#define C_OUTLINE     RGB(0x0B, 0x0C, 0x0E)
#define C_BTN_HOVER   RGB(0xDC, 0xD7, 0xCE)
#define C_ACCENT      RGB(0xE3, 0x9B, 0x32)
#define C_DANGER      RGB(0xCF, 0x4A, 0x3E)
#define C_ONLINE      RGB(0x74, 0xB0, 0x5E)
#define C_WHITE       RGB(0xFF, 0xFF, 0xFF)
#define C_BLACK       RGB(0x00, 0x00, 0x00)

#define C_BAR         RGB(0x0F, 0x10, 0x13)
#define C_BAR_RULE    RGB(0x26, 0x29, 0x2F)
#define C_BAR_HOVER   RGB(0x1C, 0x1F, 0x24)
#define C_BAR_ACTIVE  RGB(0x25, 0x29, 0x30)
#define C_BAR_PILL_HV RGB(0x31, 0x35, 0x3D)
#define C_BAR_TEXT    RGB(0xE9, 0xE6, 0xE0)
#define C_BAR_DIM     RGB(0x80, 0x85, 0x8E)
#define C_BAR_FAINT   RGB(0x4E, 0x53, 0x5B)

#define C_MENU        RGB(0x19, 0x1B, 0x20)
#define C_MENU_EDGE   RGB(0x30, 0x34, 0x3B)
#define C_MENU_HOVER  RGB(0x28, 0x2B, 0x32)

/* App-side extras in the same family */
#define C_WELL        RGB(0xDD, 0xD9, 0xD1)   /* recessed area behind content */
#define C_PRESSED     RGB(0xD3, 0xCE, 0xC4)   /* active/toggled button */

#endif
