#ifndef SAMARA_UWIN_H
#define SAMARA_UWIN_H
/* Desktop windows for ring-3 programs: syscall 500 ("samara"), op in ebx.
   The userland side is userland/samara/samara.h (C / TCC) and the MicroPython
   `samara` module. Programs draw into their own XRGB buffer and present it;
   the WM shows it (optionally pixel-scaled) and feeds back input events. */
#include "core/types.h"

#define SYS_SAMARA 500

enum {
    SM_OP_OPEN = 1,      /* ecx = sm_open_t*                -> handle          */
    SM_OP_PRESENT,       /* ecx = handle, edx = pixels       -> 0 / -EPIPE      */
    SM_OP_EVENT,         /* ecx = handle, edx = ev*, esi = timeout ms (-1 = wait) -> 1 / 0 */
    SM_OP_CLOSE,         /* ecx = handle                                        */
    SM_OP_TEXT,          /* ecx = sm_text_t*                -> pen x / width    */
    SM_OP_KEYS,          /* ecx = handle, edx = uint8_t[32]  (0s unless focused) */
    SM_OP_MOUSE,         /* ecx = handle, edx = int[3] x,y,buttons -> 1 if inside */
    SM_OP_TITLE,         /* ecx = handle, edx = title                           */
    SM_OP_FONT_H,        /* ecx = font                       -> line height      */
};

enum { SM_EV_NONE, SM_EV_KEY, SM_EV_MOUSE_DOWN, SM_EV_MOUSE_UP, SM_EV_MOUSE_MOVE, SM_EV_CLOSE };

typedef struct { int32_t w, h, scale, flags; uint32_t title; } sm_open_t;
typedef struct { int32_t type, a, b, c; } sm_event_t;
typedef struct {
    uint32_t buf; int32_t bw, bh, x, y, font; uint32_t color, str;
} sm_text_t;              /* buf == 0: measure only */

/* Kernel side */
int32_t uwin_syscall(uint32_t op, uint32_t a, uint32_t b, uint32_t c);
void    uwin_proc_exit(int pid);       /* from process teardown */
void    uwin_wm_frame(void);           /* once per WM frame: create/close windows */
void    uwin_wm_exit(void);            /* WM shutting down: all windows closed */
bool    uwin_wm_running(void);

#endif
