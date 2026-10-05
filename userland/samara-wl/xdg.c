#include "swl.h"
#include "xdg-shell-server-protocol.h"
#include "xdg-decoration-server-protocol.h"

struct pos { int w, h, rx, ry, rw, rh, anchor, grav, ox, oy; };

/* ---- toplevel ---- */

void xdg_top_configure(struct tl *t, int w, int h) {
    struct wl_array st;
    wl_array_init(&st);
    if (t->focus) *(uint32_t *)wl_array_add(&st, 4) = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (t->xt) xdg_toplevel_send_configure(t->xt, w / scale, h / scale, &st);
    wl_array_release(&st);
    if (t->deco) zxdg_toplevel_decoration_v1_send_configure(t->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    t->cfg = next_serial();
    if (t->s->xs) xdg_surface_send_configure(t->s->xs, t->cfg);
}

void xdg_close(struct tl *t) { if (t->xt) xdg_toplevel_send_close(t->xt); }

void xdg_commit(struct surf *s) {
    if (s->role == R_TOP && s->root && !s->root->configured) {
        s->root->configured = true;
        xdg_top_configure(s->root, 0, 0);
    }
}

static void tp_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void tp_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) {}
static void tp_title(struct wl_client *c, struct wl_resource *r, const char *title) {
    struct tl *t = wl_resource_get_user_data(r);
    if (!t) return;
    strncpy(t->title, title, sizeof(t->title) - 1);
    if (t->h >= 0) sm(OP_TITLE, t->h, (long)t->title, 0);
}
static void tp_appid(struct wl_client *c, struct wl_resource *r, const char *id) {}
static void tp_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t sr, int32_t x, int32_t y) {}
static void tp_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t sr) {}
static void tp_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t sr, uint32_t e) {}
static void tp_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {}
static void tp_none(struct wl_client *c, struct wl_resource *r) {}
static void tp_fs(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) {}
static const struct xdg_toplevel_interface tp_impl = {
    tp_destroy, tp_parent, tp_title, tp_appid, tp_menu, tp_move, tp_resize, tp_size, tp_size,
    tp_none, tp_none, tp_fs, tp_none, tp_none
};

static void tp_free(struct wl_resource *r) {
    struct tl *t = wl_resource_get_user_data(r);
    if (!t) return;
    t->xt = NULL;
    surf_unmap(t->s);
}

static void deco_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void deco_mode(struct wl_client *c, struct wl_resource *r, uint32_t m) {
    struct tl *t = wl_resource_get_user_data(r);
    if (t) xdg_top_configure(t, t->h >= 0 ? t->cw : 0, t->h >= 0 ? t->ch : 0);
}
static void deco_unset(struct wl_client *c, struct wl_resource *r) { deco_mode(c, r, 0); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = { deco_destroy, deco_mode, deco_unset };

static void deco_free(struct wl_resource *r) {
    struct tl *t = wl_resource_get_user_data(r);
    if (t) t->deco = NULL;
}

static void dm_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void dm_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *top) {
    struct tl *t = wl_resource_get_user_data(top);
    struct wl_resource *d = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(d, &deco_impl, t, deco_free);
    if (!t) return;
    t->deco = d;
    zxdg_toplevel_decoration_v1_send_configure(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_decoration_manager_v1_interface dm_impl = { dm_destroy, dm_get };

static void dm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface, 1, id);
    wl_resource_set_implementation(r, &dm_impl, NULL, NULL);
}

/* ---- popups ---- */

static void pu_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void pu_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t sr) {
    struct surf *s = wl_resource_get_user_data(r);
    if (s) s->grab = true;
}
static void pu_repos(struct wl_client *c, struct wl_resource *r, struct wl_resource *pos, uint32_t token) {
    xdg_popup_send_repositioned(r, token);
}
static const struct xdg_popup_interface pu_impl = { pu_destroy, pu_grab, pu_repos };

static void pu_free(struct wl_resource *r) {
    struct surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->xp = NULL;
    s->role = R_NONE;
    surf_unmap(s);
}

void popup_dismiss(struct tl *t) {
    struct surf *p;
    wl_list_for_each_reverse(p, &t->s->popups, link)
        if (p->grab && p->xp) {
            p->grab = false;
            xdg_popup_send_popup_done(p->xp);
        }
}

static void ps_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void ps_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {
    struct pos *p = wl_resource_get_user_data(r);
    p->w = w; p->h = h;
}
static void ps_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    struct pos *p = wl_resource_get_user_data(r);
    p->rx = x; p->ry = y; p->rw = w; p->rh = h;
}
static void ps_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) {
    struct pos *p = wl_resource_get_user_data(r);
    p->anchor = a;
}
static void ps_grav(struct wl_client *c, struct wl_resource *r, uint32_t g) {
    struct pos *p = wl_resource_get_user_data(r);
    p->grav = g;
}
static void ps_adj(struct wl_client *c, struct wl_resource *r, uint32_t a) {}
static void ps_off(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    struct pos *p = wl_resource_get_user_data(r);
    p->ox = x; p->oy = y;
}
static void ps_none(struct wl_client *c, struct wl_resource *r) {}
static void ps_psize(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {}
static void ps_pcfg(struct wl_client *c, struct wl_resource *r, uint32_t s) {}
static const struct xdg_positioner_interface ps_impl = {
    ps_destroy, ps_size, ps_rect, ps_anchor, ps_grav, ps_adj, ps_off, ps_none, ps_psize, ps_pcfg
};

static void ps_free(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }

/* ---- xdg_surface ---- */

static void xs_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void xs_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surf *s = wl_resource_get_user_data(r);
    struct tl *t = calloc(1, sizeof(*t));
    t->s = s; t->h = -1;
    strcpy(t->title, "wayland");
    s->root = t;
    s->role = R_TOP;
    wl_list_insert(&tls, &t->link);
    t->xt = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(t->xt, &tp_impl, t, tp_free);
}

static void xs_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *par, struct wl_resource *pos) {
    struct surf *s = wl_resource_get_user_data(r);
    struct pos *p = wl_resource_get_user_data(pos);
    struct surf *ps = par ? wl_resource_get_user_data(par) : NULL;
    s->xp = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->xp, &pu_impl, s, pu_free);
    if (!ps) return;
    s->role = R_POPUP;
    s->parent = ps;
    wl_list_remove(&s->link);
    wl_list_insert(ps->popups.prev, &s->link);
    int x = p->rx, y = p->ry;
    switch (p->anchor) {
    case XDG_POSITIONER_ANCHOR_TOP: x += p->rw / 2; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM: x += p->rw / 2; y += p->rh; break;
    case XDG_POSITIONER_ANCHOR_LEFT: y += p->rh / 2; break;
    case XDG_POSITIONER_ANCHOR_RIGHT: x += p->rw; y += p->rh / 2; break;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT: break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: y += p->rh; break;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT: x += p->rw; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: x += p->rw; y += p->rh; break;
    default: x += p->rw / 2; y += p->rh / 2;
    }
    int g = p->grav;
    bool gl = g == XDG_POSITIONER_GRAVITY_LEFT || g == XDG_POSITIONER_GRAVITY_TOP_LEFT || g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
    bool gr = g == XDG_POSITIONER_GRAVITY_RIGHT || g == XDG_POSITIONER_GRAVITY_TOP_RIGHT || g == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    bool gt = g == XDG_POSITIONER_GRAVITY_TOP || g == XDG_POSITIONER_GRAVITY_TOP_LEFT || g == XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    bool gb = g == XDG_POSITIONER_GRAVITY_BOTTOM || g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT || g == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    x += gl ? -p->w : gr ? 0 : -p->w / 2;
    y += gt ? -p->h : gb ? 0 : -p->h / 2;
    x += p->ox; y += p->oy;
    /* slide it back into the window */
    struct surf *q = ps;
    int bx, by;
    surf_rect(ps, &bx, &by);
    bx /= scale; by /= scale;
    while (q->parent) q = q->parent;
    if (q->root && q->root->h >= 0) {
        if (bx + x + p->w > q->root->cw / scale) x = q->root->cw / scale - p->w - bx;
        if (by + y + p->h > q->root->ch / scale) y = q->root->ch / scale - p->h - by;
        if (bx + x < 0) x = -bx;
        if (by + y < 0) y = -by;
    }
    s->px = x * scale; s->py = y * scale;
    xdg_popup_send_configure(s->xp, x, y, p->w, p->h);
    xdg_surface_send_configure(r, next_serial());
}

static void xs_geom(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t sr) {}
static const struct xdg_surface_interface xs_impl = { xs_destroy, xs_toplevel, xs_popup, xs_geom, xs_ack };

static void xs_free(struct wl_resource *r) {
    struct surf *s = wl_resource_get_user_data(r);
    if (s) s->xs = NULL;
}

/* ---- wm_base ---- */

static void wb_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void wb_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &ps_impl, calloc(1, sizeof(struct pos)), ps_free);
}
static void wb_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *sres) {
    struct surf *s = wl_resource_get_user_data(sres);
    s->xs = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->xs, &xs_impl, s, xs_free);
}
static void wb_pong(struct wl_client *c, struct wl_resource *r, uint32_t sr) {}
static const struct xdg_wm_base_interface wb_impl = { wb_destroy, wb_positioner, wb_get, wb_pong };

static void wb_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, ver > 5 ? 5 : ver, id);
    wl_resource_set_implementation(r, &wb_impl, NULL, NULL);
}

void xdg_init(void) {
    wl_global_create(dpy, &xdg_wm_base_interface, 5, NULL, wb_bind);
    wl_global_create(dpy, &zxdg_decoration_manager_v1_interface, 1, NULL, dm_bind);
}
