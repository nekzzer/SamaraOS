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
    SM_OP_PRESENT,       /* ecx = handle, edx = pixels, esi = w<<16|h or 0 -> 0 / -EPIPE */
    SM_OP_EVENT,         /* ecx = handle, edx = ev*, esi = timeout ms (-1 = wait) -> 1 / 0 */
    SM_OP_CLOSE,         /* ecx = handle                                        */
    SM_OP_TEXT,          /* ecx = sm_text_t*                -> pen x / width    */
    SM_OP_KEYS,          /* ecx = handle, edx = uint8_t[32]  (0s unless focused) */
    SM_OP_MOUSE,         /* ecx = handle, edx = int[3] x,y,buttons -> 1 if inside */
    SM_OP_TITLE,         /* ecx = handle, edx = title                           */
    SM_OP_FONT_H,        /* ecx = font                       -> line height      */
    SM_OP_SCREEN,        /*                                  -> screen w<<16|h   */
    SM_OP_CLIP_GET,      /* ecx = buf, edx = cap             -> bytes (utf-8, not NUL-terminated) */
    SM_OP_CLIP_SET,      /* ecx = buf, edx = len                                  */
    SM_OP_NOTIFY,        /* ecx = "title\nbody"              popup in the corner  */
    SM_OP_CTL,           /* ecx = SM_CTL_*, edx = arg                             */
};

enum { SM_EV_NONE, SM_EV_KEY, SM_EV_MOUSE_DOWN, SM_EV_MOUSE_UP, SM_EV_MOUSE_MOVE, SM_EV_CLOSE,
       SM_EV_WHEEL, SM_EV_RDOWN, SM_EV_RESIZE,
       SM_EV_RAWKEY, SM_EV_PENTER, SM_EV_PLEAVE, SM_EV_PMOVE, SM_EV_PBTN, SM_EV_FOCUS };
/* SM_EV_KEY: b = modifiers (1 shift, 2 ctrl, 4 alt). SM_EV_RDOWN: a, b = x, y of a right click.
   SM_EV_RESIZE: a, b = new client size (SM_F_RESIZE windows) */
/* SM_F_WL windows (samara-wl): resizable, scale 1, and raw input instead of the old events:
   SM_EV_RAWKEY a = linux key code, b = 1 down / 0 up, c = 1 if the RU layout is on (keyboard focus only),
   SM_EV_FOCUS a = 1/0 keyboard focus, SM_EV_PENTER / PMOVE a, b = x, y in the client (PMOVE goes on while a
   button is held, even outside), SM_EV_PLEAVE, SM_EV_PBTN a = BTN_LEFT/RIGHT/MIDDLE (0x110..), b = 1/0,
   SM_EV_WHEEL a = dz (> 0 down). The open size is clamped to the screen: a RESIZE event follows if it was. */
enum { SM_CTL_RELOAD = 1, SM_CTL_SHOT, SM_CTL_LOCK };   /* conf changed / PrintScreen / Super+L */

#define SM_F_WL     4
#define SM_F_RAW    1     /* sm_text rasterizes into the buffer, no crisp overlay (lots of text) */
#define SM_F_RESIZE 2     /* window follows its frame: scale 1, SM_EV_RESIZE; present with size in esi */

typedef struct { int32_t w, h, scale, flags; uint64_t title; } sm_open_t;
typedef struct { int32_t type, a, b, c; } sm_event_t;
typedef struct {
    uint64_t buf; int32_t bw, bh, x, y, font; uint32_t color; uint64_t str;
} sm_text_t;              /* buf == 0: measure only */

/* Kernel side */
void    clip_set(const char* s, int n);   /* the shared clipboard, utf-8 */
int     clip_get(char* out, int cap);
int32_t uwin_syscall(uint32_t op, uint64_t a, uint64_t b, uint64_t c);
void    uwin_proc_exit(int pid);       /* from process teardown */
void    uwin_wm_frame(void);           /* once per WM frame: create/close windows */
void    uwin_wm_exit(void);            /* WM shutting down: all windows closed */
bool    uwin_wm_running(void);
bool    uwin_pid_has_window(int pid);  /* process has an open desktop window */

#endif
