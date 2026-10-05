#include "swl.h"
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <fcntl.h>

static xcb_connection_t *xc;
static xcb_window_t root, wmwin;
static struct wl_client *xcl;
static int wmfd, evfd;
static xcb_atom_t a_proto, a_delete, a_focus, a_netname, a_utf8, a_surf, a_sel, a_check, a_wmname;

struct xw {
    xcb_window_t id;
    bool or, mapped;
    int x, y, w, h;
    struct surf *s;
    struct tl *t;
    struct wl_list link;
};
static struct wl_list xws;
static struct wl_event_source *csrc;

static xcb_atom_t atom(const char *n) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(xc, xcb_intern_atom(xc, 0, strlen(n), n), NULL);
    xcb_atom_t a = r ? r->atom : 0;
    free(r);
    return a;
}

static struct xw *xw_find(xcb_window_t id) {
    struct xw *w;
    wl_list_for_each(w, &xws, link) if (w->id == id) return w;
    return NULL;
}

static struct xw *xw_new(xcb_window_t id, bool or, int x, int y, int w, int h) {
    struct xw *n = xw_find(id);
    if (!n) {
        n = calloc(1, sizeof(*n));
        n->id = id;
        wl_list_insert(&xws, &n->link);
    }
    n->or = or; n->x = x; n->y = y; n->w = w; n->h = h;
    return n;
}

static void get_title(struct xw *w, char *out, int cap) {
    xcb_get_property_reply_t *r = xcb_get_property_reply(xc,
        xcb_get_property(xc, 0, w->id, a_netname, a_utf8, 0, 64), NULL);
    if (!r || !xcb_get_property_value_length(r)) {
        free(r);
        r = xcb_get_property_reply(xc, xcb_get_property(xc, 0, w->id, a_wmname, XCB_ATOM_STRING, 0, 64), NULL);
    }
    out[0] = 0;
    if (r) {
        int n = xcb_get_property_value_length(r);
        if (n > cap - 1) n = cap - 1;
        memcpy(out, xcb_get_property_value(r), n);
        out[n] = 0;
        free(r);
    }
}

static void get_proto(struct tl *t) {
    xcb_get_property_reply_t *r = xcb_get_property_reply(xc,
        xcb_get_property(xc, 0, t->xwin, a_proto, XCB_ATOM_ATOM, 0, 32), NULL);
    t->del_ok = t->take_focus = false;
    if (!r) return;
    xcb_atom_t *a = xcb_get_property_value(r);
    int n = xcb_get_property_value_length(r) / 4;
    for (int i = 0; i < n; i++) {
        if (a[i] == a_delete) t->del_ok = true;
        if (a[i] == a_focus) t->take_focus = true;
    }
    free(r);
}

static void x_try_map(struct xw *w) {
    struct surf *s = w->s;
    if (!s || !s->has_buf) return;
    if (w->or) {
        if (s->parent || !kfocus) return;
        s->role = R_XOR;
        s->parent = kfocus->s;
        wl_list_remove(&s->link);
        wl_list_insert(kfocus->s->popups.prev, &s->link);
        s->px = w->x; s->py = w->y;
        compose(kfocus, s->px, s->py, s->px + s->w, s->py + s->h);
        present(kfocus);
        return;
    }
    if (s->root) return;
    struct tl *t = calloc(1, sizeof(*t));
    t->s = s; t->h = -1; t->xwin = w->id; t->configured = true;
    get_title(w, t->title, sizeof(t->title));
    if (!t->title[0]) strcpy(t->title, "x11");
    get_proto(t);
    s->root = t;
    s->role = R_X;
    w->t = t;
    wl_list_insert(&tls, &t->link);
    tl_open(t);
}

void xwm_surf_commit(struct surf *s) {
    if (!s->xwin || !xc) return;
    struct xw *w = xw_find(s->xwin);
    if (w) x_try_map(w);
}

void xwm_resize(struct tl *t, int w, int h) {
    uint32_t v[2] = { w, h };
    xcb_configure_window(xc, t->xwin, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    xcb_flush(xc);
}

static void send_proto(xcb_window_t win, xcb_atom_t what) {
    xcb_client_message_event_t ev = { 0 };
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = a_proto;
    ev.data.data32[0] = what;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(xc, 0, win, XCB_EVENT_MASK_NO_EVENT, (char *)&ev);
    xcb_flush(xc);
}

void xwm_close(struct tl *t) {
    if (t->del_ok) send_proto(t->xwin, a_delete);
    else { xcb_kill_client(xc, t->xwin); xcb_flush(xc); }
}

void xwm_focus(struct tl *t) {
    if (!xc) return;
    xcb_set_input_focus(xc, XCB_INPUT_FOCUS_POINTER_ROOT, t->xwin, XCB_CURRENT_TIME);
    if (t->take_focus) send_proto(t->xwin, a_focus);
    xcb_flush(xc);
}

static void x_gone(struct xw *w) {
    if (w->t) { w->t->s->root = NULL; tl_destroy(w->t); w->t = NULL; }
    else if (w->s && w->s->parent) surf_unmap(w->s);
    if (w->s) w->s->xwin = 0;
    wl_list_remove(&w->link);
    free(w);
}

static void ev_configure_request(xcb_configure_request_event_t *e) {
    uint32_t v[7];
    int n = 0;
    struct xw *w = xw_find(e->window);
    uint16_t m = e->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                  XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH);
    if (w && !w->or) m |= XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    if (m & XCB_CONFIG_WINDOW_X) v[n++] = (w && !w->or) ? 0 : e->x;
    if (m & XCB_CONFIG_WINDOW_Y) v[n++] = (w && !w->or) ? 0 : e->y;
    if (m & XCB_CONFIG_WINDOW_WIDTH) v[n++] = e->width;
    if (m & XCB_CONFIG_WINDOW_HEIGHT) v[n++] = e->height;
    if (m & XCB_CONFIG_WINDOW_BORDER_WIDTH) v[n++] = e->border_width;
    xcb_configure_window(xc, e->window, m, v);
    if (w) {
        if (m & XCB_CONFIG_WINDOW_WIDTH) w->w = e->width;
        if (m & XCB_CONFIG_WINDOW_HEIGHT) w->h = e->height;
    }
}

static void ev_client_message(xcb_client_message_event_t *e) {
    if (e->type != a_surf) return;
    struct wl_resource *r = wl_client_get_object(xcl, e->data.data32[0]);
    if (!r || strcmp(wl_resource_get_class(r), "wl_surface")) return;
    struct surf *s = wl_resource_get_user_data(r);
    struct xw *w = xw_find(e->window);
    if (!w) return;
    s->xwin = w->id;
    w->s = s;
    if (s->role == R_NONE) s->role = w->or ? R_XOR : R_X;
    x_try_map(w);
}

static void ev_property(xcb_property_notify_event_t *e) {
    struct xw *w = xw_find(e->window);
    if (!w || !w->t) return;
    if (e->atom == a_netname || e->atom == a_wmname) {
        get_title(w, w->t->title, sizeof(w->t->title));
        if (w->t->h >= 0) sm(OP_TITLE, w->t->h, (long)w->t->title, 0);
    } else if (e->atom == a_proto) get_proto(w->t);
}

static int xwm_readable(int fd, uint32_t mask, void *d) {
    xcb_generic_event_t *e;
    while ((e = xcb_poll_for_event(xc))) {
        switch (e->response_type & 0x7f) {
        case XCB_CREATE_NOTIFY: {
            xcb_create_notify_event_t *c = (void *)e;
            if (c->parent == root) {
                uint32_t m = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE;
                xw_new(c->window, c->override_redirect, c->x, c->y, c->width, c->height);
                xcb_change_window_attributes(xc, c->window, XCB_CW_EVENT_MASK, &m);
                xcb_flush(xc);
            }
            break;
        }
        case XCB_DESTROY_NOTIFY: {
            struct xw *w = xw_find(((xcb_destroy_notify_event_t *)e)->window);
            if (w) x_gone(w);
            break;
        }
        case XCB_MAP_REQUEST: {
            xcb_map_request_event_t *m = (void *)e;
            struct xw *w = xw_find(m->window);
            if (w) w->mapped = true;
            xcb_composite_redirect_window(xc, m->window, XCB_COMPOSITE_REDIRECT_MANUAL);
            xcb_map_window(xc, m->window);
            xcb_flush(xc);
            break;
        }
        case XCB_MAP_NOTIFY: {
            xcb_map_notify_event_t *m = (void *)e;
            struct xw *w = xw_find(m->window);
            if (w) { w->mapped = true; x_try_map(w); }
            break;
        }
        case XCB_UNMAP_NOTIFY: {
            struct xw *w = xw_find(((xcb_unmap_notify_event_t *)e)->window);
            if (w && w->t) { w->t->s->root = NULL; tl_destroy(w->t); w->t = NULL; }
            else if (w && w->s && w->s->parent) { w->s->parent = NULL; }
            if (w) w->mapped = false;
            break;
        }
        case XCB_CONFIGURE_REQUEST: ev_configure_request((void *)e); break;
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *c = (void *)e;
            struct xw *w = xw_find(c->window);
            if (w) { w->x = c->x; w->y = c->y; w->w = c->width; w->h = c->height; }
            break;
        }
        case XCB_CLIENT_MESSAGE: ev_client_message((void *)e); break;
        case XCB_PROPERTY_NOTIFY: ev_property((void *)e); break;
        }
        free(e);
    }
    return 0;
}

/* xcb_connect_to_fd waits for Xwayland, and Xwayland waits for us on the
   wayland side, so the handshake lives in its own thread */
static void *conn_thread(void *a) {
    xc = xcb_connect_to_fd(wmfd, NULL);
    uint64_t one = 1;
    write(evfd, &one, 8);
    return NULL;
}

static int conn_done(int fd, uint32_t mask, void *d) {
    uint64_t v;
    read(fd, &v, 8);
    if (!xc || xcb_connection_has_error(xc)) { fprintf(stderr, "samara-wl: xwm connect failed\n"); return 0; }
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    wl_event_source_remove(csrc);
    xcb_screen_t *sc = xcb_setup_roots_iterator(xcb_get_setup(xc)).data;
    root = sc->root;
    a_proto = atom("WM_PROTOCOLS");
    a_delete = atom("WM_DELETE_WINDOW");
    a_focus = atom("WM_TAKE_FOCUS");
    a_netname = atom("_NET_WM_NAME");
    a_wmname = XCB_ATOM_WM_NAME;
    a_utf8 = atom("UTF8_STRING");
    a_surf = atom("WL_SURFACE_ID");
    a_sel = atom("WM_S0");
    a_check = atom("_NET_SUPPORTING_WM_CHECK");
    uint32_t em = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(xc, root, XCB_CW_EVENT_MASK, &em);
    xcb_generic_error_t *er = xcb_request_check(xc, ck);
    if (er) { fprintf(stderr, "samara-wl: another wm on the x server?\n"); free(er); return 0; }
    xcb_composite_redirect_subwindows(xc, root, XCB_COMPOSITE_REDIRECT_MANUAL);   /* without it rootless xwayland never makes surfaces */
    wmwin = xcb_generate_id(xc);
    xcb_create_window(xc, 0, wmwin, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root, a_check, XCB_ATOM_WINDOW, 32, 1, &wmwin);
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, wmwin, a_check, XCB_ATOM_WINDOW, 32, 1, &wmwin);
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, wmwin, a_netname, a_utf8, 8, 10, "samara-wl");
    xcb_set_selection_owner(xc, wmwin, a_sel, XCB_CURRENT_TIME);
    xcb_flush(xc);
    wl_event_loop_add_fd(loop, xcb_get_file_descriptor(xc), WL_EVENT_READABLE, xwm_readable, NULL);
    /* windows that were mapped before we got here */
    xwm_readable(0, 0, NULL);
    return 0;
}

void xwm_start(void) {
    int wl[2], wm[2];
    wl_list_init(&xws);
    socketpair(AF_UNIX, SOCK_STREAM, 0, wl);
    socketpair(AF_UNIX, SOCK_STREAM, 0, wm);
    xcl = wl_client_create(dpy, wl[0]);
    wmfd = wm[0];
    pid_t pid = fork();
    if (pid == 0) {
        char a[16], b[16];
        close(wl[0]); close(wm[0]);
        snprintf(a, sizeof a, "%d", wl[1]);
        snprintf(b, sizeof b, "%d", wm[1]);
        setenv("WAYLAND_SOCKET", a, 1);
        execlp("Xwayland", "Xwayland", ":0", "-rootless", "-wm", b, "-shm", "-noreset", (char *)NULL);
        _exit(1);
    }
    close(wl[1]); close(wm[1]);
    evfd = eventfd(0, 0);
    struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
    csrc = wl_event_loop_add_fd(loop, evfd, WL_EVENT_READABLE, conn_done, NULL);
    pthread_t th;
    pthread_create(&th, NULL, conn_thread, NULL);
    pthread_detach(th);
}
