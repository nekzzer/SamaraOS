#ifndef SAMARA_THEME_H
#define SAMARA_THEME_H
/* Desktop palette shared by the window manager and apps. */
#include "gfx/gfx.h"

// graphite (pure black looked dead) + greys, one amber accent for focus/selection/caret. red only on
// close, green only on the net dot. old warm-paper palette is in git
// (and in build/backup-theme-*)

#define C_DESK_TOP    RGB(0x10, 0x11, 0x14)
#define C_DESK_BOT    RGB(0x1E, 0x1F, 0x23)
#define C_WORDMARK    RGB(0x27, 0x28, 0x2D)

#define C_SURFACE     RGB(0x17, 0x18, 0x1B)
#define C_INK         RGB(0xE6, 0xE6, 0xE6)
#define C_INK_DIM     RGB(0x85, 0x86, 0x8C)
#define C_GLYPH       RGB(0xA9, 0xAA, 0xAF)
#define C_GLYPH_DIM   RGB(0x55, 0x56, 0x5C)
#define C_RULE        RGB(0x26, 0x27, 0x2B)
#define C_OUTLINE     RGB(0x2C, 0x2D, 0x32)
#define C_BTN_HOVER   RGB(0x25, 0x26, 0x2A)
#define C_ACCENT      RGB(0xE3, 0x9B, 0x32)
#define C_DANGER      RGB(0xC4, 0x3B, 0x30)
#define C_ONLINE      RGB(0x5F, 0xB0, 0x6A)
#define C_WHITE       RGB(0xFF, 0xFF, 0xFF)
#define C_BLACK       RGB(0x00, 0x00, 0x00)

#define C_BAR         RGB(0x0E, 0x0F, 0x11)
#define C_BAR_RULE    RGB(0x22, 0x23, 0x27)
#define C_BAR_HOVER   RGB(0x1A, 0x1B, 0x1E)
#define C_BAR_ACTIVE  RGB(0x22, 0x23, 0x27)
#define C_BAR_PILL_HV RGB(0x2C, 0x2D, 0x32)
#define C_BAR_TEXT    RGB(0xE6, 0xE6, 0xE6)
#define C_BAR_DIM     RGB(0x8A, 0x8B, 0x90)
#define C_BAR_FAINT   RGB(0x4E, 0x4F, 0x55)

#define C_MENU        RGB(0x14, 0x15, 0x18)
#define C_MENU_EDGE   RGB(0x2C, 0x2D, 0x32)
#define C_MENU_HOVER  RGB(0x21, 0x22, 0x26)

/* App-side extras in the same family */
#define C_WELL        RGB(0x10, 0x11, 0x14)   /* recessed area behind content */
#define C_PANEL       RGB(0x1E, 0x1F, 0x23)   /* boxes inside a window */
#define C_PRESSED     RGB(0x2C, 0x2D, 0x32)   /* active/toggled button */

#endif
