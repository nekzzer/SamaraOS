#ifndef SAMARA_THEME_H
#define SAMARA_THEME_H
/* Desktop palette shared by the window manager and apps. */
#include "gfx/gfx.h"

// graphite (pure black looked dead) + greys, one amber accent for focus/selection/caret. red only on
// close, green only on the net dot. old warm-paper palette is in git
// (and in build/backup-theme-*)

enum {
    T_DESK_TOP,
    T_DESK_BOT,
    T_WORDMARK,
    T_SURFACE,
    T_INK,
    T_INK_DIM,
    T_GLYPH,
    T_GLYPH_DIM,
    T_RULE,
    T_OUTLINE,
    T_BTN_HOVER,
    T_ACCENT,
    T_DANGER,
    T_ONLINE,
    T_WHITE,
    T_BLACK,
    T_BAR,
    T_BAR_RULE,
    T_BAR_HOVER,
    T_BAR_ACTIVE,
    T_BAR_PILL_HV,
    T_BAR_TEXT,
    T_BAR_DIM,
    T_BAR_FAINT,
    T_MENU,
    T_MENU_EDGE,
    T_MENU_HOVER,
    T_WELL,
    T_PANEL,
    T_PRESSED,
    T_N
};
extern uint32_t th_pal[T_N];
void theme_set(int light);

#define C_DESK_TOP (th_pal[T_DESK_TOP])
#define C_DESK_BOT (th_pal[T_DESK_BOT])
#define C_WORDMARK (th_pal[T_WORDMARK])
#define C_SURFACE (th_pal[T_SURFACE])
#define C_INK (th_pal[T_INK])
#define C_INK_DIM (th_pal[T_INK_DIM])
#define C_GLYPH (th_pal[T_GLYPH])
#define C_GLYPH_DIM (th_pal[T_GLYPH_DIM])
#define C_RULE (th_pal[T_RULE])
#define C_OUTLINE (th_pal[T_OUTLINE])
#define C_BTN_HOVER (th_pal[T_BTN_HOVER])
#define C_ACCENT (th_pal[T_ACCENT])
#define C_DANGER (th_pal[T_DANGER])
#define C_ONLINE (th_pal[T_ONLINE])
#define C_WHITE (th_pal[T_WHITE])
#define C_BLACK (th_pal[T_BLACK])
#define C_BAR (th_pal[T_BAR])
#define C_BAR_RULE (th_pal[T_BAR_RULE])
#define C_BAR_HOVER (th_pal[T_BAR_HOVER])
#define C_BAR_ACTIVE (th_pal[T_BAR_ACTIVE])
#define C_BAR_PILL_HV (th_pal[T_BAR_PILL_HV])
#define C_BAR_TEXT (th_pal[T_BAR_TEXT])
#define C_BAR_DIM (th_pal[T_BAR_DIM])
#define C_BAR_FAINT (th_pal[T_BAR_FAINT])
#define C_MENU (th_pal[T_MENU])
#define C_MENU_EDGE (th_pal[T_MENU_EDGE])
#define C_MENU_HOVER (th_pal[T_MENU_HOVER])
#define C_WELL (th_pal[T_WELL])
#define C_PANEL (th_pal[T_PANEL])
#define C_PRESSED (th_pal[T_PRESSED])

#endif
