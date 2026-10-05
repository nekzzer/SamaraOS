#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

/* uwin (syscall 500), see src/gui/uwin.h */
enum { OP_OPEN = 1, OP_PRESENT, OP_EVENT, OP_CLOSE, OP_TITLE = 8, OP_SCREEN = 10 };
enum { EV_CLOSE = 5, EV_WHEEL = 6, EV_RESIZE = 8, EV_RAWKEY, EV_PENTER, EV_PLEAVE, EV_PMOVE, EV_PBTN, EV_FOCUS };
#define F_WL 4
typedef struct { int32_t w, h, scale, flags; uint64_t title; } uopen_t;
typedef struct { int32_t type, a, b, c; } uev_t;

struct pool { int refs, fd; void *data; int size; };
struct buf { struct wl_resource *res; struct pool *pool; int off, w, h, stride; uint32_t fmt; };

struct frame { struct wl_resource *res; struct wl_list link; };

struct tl;

struct surf {
    struct wl_resource *res;
    struct wl_client *cl;
    struct buf *pend;
    bool attached;
    int dx0, dy0, dx1, dy1;           /* pending damage bbox, x1 <= x0 = none */
    struct wl_list pframes, frames;
    uint32_t *pix;
    int w, h;
    bool argb, has_buf;
    int role;                          /* R_* */
    struct tl *root;                   /* toplevel this draws into */
    struct surf *parent;
    int px, py;                        /* origin relative to the parent's */
    struct wl_list subs, popups, link, glink; /* children, link in parent's list, all surfaces */
    struct wl_resource *xs;            /* xdg_surface */
    struct wl_resource *xp;            /* xdg_popup */
    int bs, k;                         /* buffer_scale, pixel multiplier we apply */
    int xwin;                          /* x11 window id, 0 if none */
    bool grab, sub_sync;
    struct wl_listener bl;             /* pending buffer destroyed */
};
enum { R_NONE, R_TOP, R_POPUP, R_SUB, R_X, R_XOR };

struct tl {
    struct surf *s;
    struct wl_resource *xt, *deco;
    int h;                             /* uwin handle, -1 = not open yet */
    int cw, ch;                        /* window client size */
    uint32_t *pix;
    char title[160];
    uint32_t cfg;                      /* last configure serial */
    bool configured, focus, dead;
    uint32_t dead_t;
    int xwin;
    bool del_ok, take_focus;           /* x11: WM_DELETE_WINDOW, WM_TAKE_FOCUS */
    struct wl_list link;
};

extern struct wl_display *dpy;
extern struct wl_list tls;
extern int scr_w, scr_h;
extern struct tl *kfocus;

uint32_t now_ms(void);
uint32_t next_serial(void);
long sm(long op, long a, long b, long c);

/* comp.c */
void comp_init(void);
void surf_rect(struct surf *s, int *x, int *y);
void xdg_commit(struct surf *s);
void commit_done(struct surf *s);
void compose(struct tl *t, int x0, int y0, int x1, int y1);
void present(struct tl *t);
void frames_tick(void);
void tl_resized(struct tl *t, int cw, int ch);
void tl_open(struct tl *t);
void tl_destroy(struct tl *t);
void surf_unmap(struct surf *s);
struct surf *pick(struct tl *t, int x, int y, int *lx, int *ly);

/* seat.c */
void seat_init(void);
void kbd_event(int code, int down, int ru);
void kbd_enter(struct tl *t);
void kbd_leave(void);
void ptr_event(struct tl *t, uev_t *e);
void ptr_gone(struct surf *s);
void data_init(void);
void data_set_x(void);
const char *data_wl_mime(void);
void data_wl_send(const char *m, int fd);

/* xdg.c */
void xdg_init(void);
void xdg_top_configure(struct tl *t, int w, int h);
void xdg_close(struct tl *t);
void popup_dismiss(struct tl *t);

/* xwm.c */
extern int scale;
void xwm_start(void);
void xwm_surface(int win, struct wl_resource *sres);
void xwm_close(struct tl *t);
void xwm_resize(struct tl *t, int w, int h);
void xwm_focus(struct tl *t);
void xwm_sel_own(bool on);
void xwm_sel_get(int fd);
void xwm_surf_commit(struct surf *s);
void xwm_surf_gone(struct surf *s);
