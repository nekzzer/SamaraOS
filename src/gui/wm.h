#ifndef SAMARA_WM_H
#define SAMARA_WM_H
#include "core/types.h"

#define WM_MAX_WINDOWS 12
#define WM_TITLE_H 22

typedef enum {
    WIN_INFO     = 0,    /* static text content */
    WIN_TERMINAL = 1,    /* shell terminal (only one allowed) */
    WIN_APP      = 2,    /* generic app with paint/key callbacks */
} window_type_t;

struct window;
typedef struct window window_t;

struct window {
    int  x, y, w, h;
    char title[64];
    bool open;
    int  z;
    window_type_t type;
    bool dragging;
    int  drag_off_x, drag_off_y;

    /* WIN_INFO */
    const char* content;

    /* WIN_TERMINAL */
    int term_x, term_y;          /* gfx_term grid origin = client area */

    /* WIN_APP */
    void* user;
    void (*on_paint)(window_t*);                              /* called during full redraw */
    void (*on_key)(window_t*, char);
    void (*on_click)(window_t*, int rel_x, int rel_y);        /* click inside client area; NULL ok */
    void (*on_release)(window_t*);                            /* mouse-up after a press on this window; NULL ok */
    void (*on_tick)(window_t*, uint32_t now_ms);              /* per-frame logic; NULL ok */
    void (*on_close)(window_t*);                              /* fires from wm_close; NULL ok */
    bool needs_repaint;                                       /* set by app to force repaint */
    bool animate;                                             /* if true, app repainted every frame */
    bool minimized;                                           /* hidden, restorable from taskbar */

    /* Geometry management (all windows). Apps must lay out from
       wm_client_rect() on every paint; on_resize is just a heads-up. */
    bool resizable;                  /* default true; false = fixed size, no maximize */
    bool opaque;                     /* on_paint covers its whole client every time:
                                        the compositor then skips what is beneath */
    int  min_w, min_h;               /* resize limits (outer size) */
    bool maximized;                  /* fills the desktop above the taskbar */
    bool fullscreen;                 /* whole screen, no decorations (Alt+Enter) */
    int  resizing;                   /* WM_EDGE_* mask while a border is dragged */
    int  rs_x, rs_y, rs_w, rs_h, rs_mx, rs_my;               /* resize start */
    int  norm_x, norm_y, norm_w, norm_h;                     /* before maximize */
    int  fs_x, fs_y, fs_w, fs_h;                             /* before fullscreen */
    void (*on_resize)(window_t*);                             /* client size changed; NULL ok */
    void (*on_scroll)(window_t*, int dz);                     /* mouse wheel, dz > 0 = down; NULL ok */
};

#define WM_EDGE_L 1
#define WM_EDGE_R 2
#define WM_EDGE_T 4
#define WM_EDGE_B 8

/* Public API */
void      wm_init(void);
void      wm_run(void);
void      wm_request_redraw(void);
void      wm_request_exit(void);
window_t* wm_open_info(int x, int y, int w, int h, const char* title, const char* content);
window_t* wm_open_terminal(int x, int y);   /* singleton terminal */
window_t* wm_open_app(int x, int y, int w, int h, const char* title,
                       void (*on_paint)(window_t*),
                       void (*on_key)(window_t*, char),
                       void* user);
/* Extended variant: also handles per-frame tick + clicks inside client area.
   Pass NULL for callbacks you don't need. animate=true → repainted every frame. */
window_t* wm_open_app_ex(int x, int y, int w, int h, const char* title,
                          void (*on_paint)(window_t*),
                          void (*on_key)(window_t*, char),
                          void (*on_click)(window_t*, int rel_x, int rel_y),
                          void (*on_tick)(window_t*, uint32_t now_ms),
                          bool animate,
                          void* user);
void      wm_close(window_t* w);
window_t* wm_focused(void);

/* Desktop wallpaper (/home/user, /mnt or / wallpaper.bmp) changed: reload
   it on the next frame (or at the next desktop start). */
void      wm_invalidate_wallpaper(void);
/* Type + run a shell command line in the desktop terminal (opens it). */
bool      wm_terminal_feed(const char* line);

/* Toggle maximized / fullscreen state (no-op for non-resizable windows). */
void      wm_toggle_maximize(window_t* w);
void      wm_toggle_fullscreen(window_t* w);

/* For app windows: get the client rectangle (inside title+border). */
void      wm_client_rect(window_t* w, int* x, int* y, int* cw, int* ch);

#endif
