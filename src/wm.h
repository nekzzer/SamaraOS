#ifndef SAMARA_WM_H
#define SAMARA_WM_H
#include "types.h"

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
    void (*on_tick)(window_t*, uint32_t now_ms);              /* per-frame logic; NULL ok */
    void (*on_close)(window_t*);                              /* fires from wm_close; NULL ok */
    bool needs_repaint;                                       /* set by app to force repaint */
    bool animate;                                             /* if true, app repainted every frame */
};

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

/* For app windows: get the client rectangle (inside title+border). */
void      wm_client_rect(window_t* w, int* x, int* y, int* cw, int* ch);

#endif
