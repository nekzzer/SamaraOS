#include "swl.h"
#include <sys/mman.h>
#include <errno.h>

static struct wl_list surfs;
static int sub_cnt;

/* ---- shm ---- */

static void pool_unref(struct pool *p) {
    if (--p->refs > 0) return;
    munmap(p->data, p->size);
    close(p->fd);
    free(p);
}

static void buf_destroy_req(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface buf_impl = { buf_destroy_req };

static void buf_free(struct wl_resource *r) {
    struct buf *b = wl_resource_get_user_data(r);
    pool_unref(b->pool);
    free(b);
}

static void pool_create_buffer(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t off,
                               int32_t w, int32_t h, int32_t stride, uint32_t fmt) {
    struct pool *p = wl_resource_get_user_data(r);
    if (off < 0 || w <= 0 || h <= 0 || stride < w * 4 || off + (long)stride * h > p->size) {
        wl_resource_post_error(r, 0, "bad buffer");
        return;
    }
    struct buf *b = calloc(1, sizeof(*b));
    b->pool = p; p->refs++;
    b->off = off; b->w = w; b->h = h; b->stride = stride; b->fmt = fmt;
    b->res = wl_resource_create(c, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(b->res, &buf_impl, b, buf_free);
}

static void pool_destroy_req(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void pool_resize(struct wl_client *c, struct wl_resource *r, int32_t size) {
    struct pool *p = wl_resource_get_user_data(r);
    if (size <= p->size) return;
    munmap(p->data, p->size);
    p->data = mmap(NULL, size, PROT_READ, MAP_SHARED, p->fd, 0);
    p->size = size;
}
static const struct wl_shm_pool_interface pool_impl = { pool_create_buffer, pool_destroy_req, pool_resize };

static void pool_res_free(struct wl_resource *r) { pool_unref(wl_resource_get_user_data(r)); }

static void shm_create_pool(struct wl_client *c, struct wl_resource *r, uint32_t id, int fd, int32_t size) {
    struct pool *p = calloc(1, sizeof(*p));
    p->data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p->data == MAP_FAILED) {
        wl_resource_post_error(r, 1, "mmap failed");
        close(fd); free(p);
        return;
    }
    p->fd = fd; p->size = size; p->refs = 1;
    struct wl_resource *pr = wl_resource_create(c, &wl_shm_pool_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(pr, &pool_impl, p, pool_res_free);
}
static const struct wl_shm_interface shm_impl = { shm_create_pool };

static void shm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_shm_interface, ver > 1 ? 1 : ver, id);
    wl_resource_set_implementation(r, &shm_impl, NULL, NULL);
    wl_shm_send_format(r, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(r, WL_SHM_FORMAT_XRGB8888);
}

/* ---- output ---- */

static void out_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_output_interface out_impl = { out_release };

static void out_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, ver > 3 ? 3 : ver, id);
    wl_resource_set_implementation(r, &out_impl, NULL, NULL);
    wl_output_send_geometry(r, 0, 0, scr_w * 25 / 100, scr_h * 25 / 100, 0, "samara", "de", 0);
    wl_output_send_mode(r, 3, scr_w, scr_h, 60000);
    if (ver >= 2) { wl_output_send_scale(r, scale); wl_output_send_done(r); }
}

/* ---- regions (we never look at them) ---- */

static void reg_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void reg_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static const struct wl_region_interface reg_impl = { reg_destroy, reg_rect, reg_rect };

/* ---- surfaces ---- */

static struct tl *rootof(struct surf *s) {
    while (s && !s->root) s = s->parent;
    return s ? s->root : NULL;
}

void surf_rect(struct surf *s, int *x, int *y) {
    *x = *y = 0;
    for (; s && !s->root; s = s->parent) { *x += s->px; *y += s->py; }
}

static void blit(struct tl *t, struct surf *s, int ax, int ay, int x0, int y0, int x1, int y1) {
    if (ax > x0) x0 = ax;
    if (ay > y0) y0 = ay;
    if (ax + s->w < x1) x1 = ax + s->w;
    if (ay + s->h < y1) y1 = ay + s->h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *d = t->pix + y * t->cw + x0, *p = s->pix + (y - ay) * s->w + (x0 - ax);
        if (!s->argb) { memcpy(d, p, (x1 - x0) * 4); continue; }
        for (int x = x0; x < x1; x++, d++, p++) {
            uint32_t v = *p, a = v >> 24;
            if (a == 255) *d = v;
            else if (a) {
                uint32_t o = *d, ia = 255 - a;
                uint32_t r = (v >> 16 & 255) + ((o >> 16 & 255) * ia) / 255;
                uint32_t g = (v >> 8 & 255) + ((o >> 8 & 255) * ia) / 255;
                uint32_t b = (v & 255) + ((o & 255) * ia) / 255;
                *d = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
        }
    }
}

static void draw(struct tl *t, struct surf *s, int ax, int ay, int x0, int y0, int x1, int y1) {
    struct surf *c;
    if (s->has_buf) blit(t, s, ax, ay, x0, y0, x1, y1);
    wl_list_for_each(c, &s->subs, link) draw(t, c, ax + c->px, ay + c->py, x0, y0, x1, y1);
    wl_list_for_each(c, &s->popups, link) draw(t, c, ax + c->px, ay + c->py, x0, y0, x1, y1);
}

void compose(struct tl *t, int x0, int y0, int x1, int y1) {
    if (!t->pix) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->cw) x1 = t->cw;
    if (y1 > t->ch) y1 = t->ch;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) memset(t->pix + y * t->cw + x0, 0, (x1 - x0) * 4);
    draw(t, t->s, 0, 0, x0, y0, x1, y1);
}

void present(struct tl *t) {
    if (t->h < 0 || !t->pix) return;
    sm(OP_PRESENT, t->h, (long)t->pix, (long)t->cw << 16 | t->ch);
}

struct surf *pick(struct tl *t, int x, int y, int *lx, int *ly);

static struct surf *pick_in(struct surf *s, int ax, int ay, int x, int y, int *lx, int *ly) {
    struct surf *c, *r;
    wl_list_for_each_reverse(c, &s->popups, link)
        if ((r = pick_in(c, ax + c->px, ay + c->py, x, y, lx, ly))) return r;
    wl_list_for_each_reverse(c, &s->subs, link)
        if ((r = pick_in(c, ax + c->px, ay + c->py, x, y, lx, ly))) return r;
    if (s->has_buf && x >= ax && y >= ay && x < ax + s->w && y < ay + s->h) {
        *lx = x - ax; *ly = y - ay;
        return s;
    }
    return NULL;
}

struct surf *pick(struct tl *t, int x, int y, int *lx, int *ly) {
    struct surf *s = pick_in(t->s, 0, 0, x, y, lx, ly);
    if (!s) { s = t->s; *lx = x; *ly = y; }
    return s;
}

static void surf_damage(struct surf *s, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    x *= s->k; y *= s->k; w *= s->k; h *= s->k;
    if (s->dx1 <= s->dx0) { s->dx0 = x; s->dy0 = y; s->dx1 = x + w; s->dy1 = y + h; return; }
    if (x < s->dx0) s->dx0 = x;
    if (y < s->dy0) s->dy0 = y;
    if (x + w > s->dx1) s->dx1 = x + w;
    if (y + h > s->dy1) s->dy1 = y + h;
}

static void s_destroy_req(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void pend_gone(struct wl_listener *l, void *d) {
    struct surf *s = wl_container_of(l, s, bl);
    s->pend = NULL;
    wl_list_init(&l->link);
}

static void s_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *b, int32_t x, int32_t y) {
    struct surf *s = wl_resource_get_user_data(r);
    wl_list_remove(&s->bl.link);
    wl_list_init(&s->bl.link);
    s->attached = true;
    s->pend = b ? wl_resource_get_user_data(b) : NULL;
    if (b) wl_resource_add_destroy_listener(b, &s->bl);
}

static void s_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    struct surf *s = wl_resource_get_user_data(r);
    int o = s->k;
    s->k = scale;     // surface coords are logical
    surf_damage(s, x, y, w, h);
    s->k = o;
}

static void s_dmgbuf(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    surf_damage(wl_resource_get_user_data(r), x, y, w, h);
}

static void s_bscale(struct wl_client *c, struct wl_resource *r, int32_t v) {
    struct surf *s = wl_resource_get_user_data(r);
    s->bs = v < 1 ? 1 : v;
    s->k = scale > s->bs ? scale / s->bs : 1;
}

static void frame_free(struct wl_resource *r) {
    struct frame *f = wl_resource_get_user_data(r);
    wl_list_remove(&f->link);
    free(f);
}

static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surf *s = wl_resource_get_user_data(r);
    struct frame *f = calloc(1, sizeof(*f));
    f->res = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(f->res, NULL, f, frame_free);
    wl_list_insert(s->pframes.prev, &f->link);
}

static void s_noop_reg(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {}
static void s_noop_i(struct wl_client *c, struct wl_resource *r, int32_t v) {}
static void s_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {}

void surf_unmap(struct surf *s) {
    struct tl *t = s->root;
    if (s->parent) {
        struct tl *rt = rootof(s);
        wl_list_remove(&s->link);
        wl_list_init(&s->link);
        if (rt && rt->pix) {
            int ax, ay;
            surf_rect(s, &ax, &ay);
            compose(rt, ax, ay, ax + s->w, ay + s->h);
            present(rt);
        }
        s->parent = NULL;
    }
    ptr_gone(s);
    if (t) { s->root = NULL; tl_destroy(t); }
}

static void s_commit(struct wl_client *c, struct wl_resource *r) {
    struct surf *s = wl_resource_get_user_data(r);
    if (s->attached) {
        struct buf *b = s->pend;
        if (b) {
            int k = s->k;
            bool resized = !s->pix || s->w != b->w * k || s->h != b->h * k;
            if (resized) {
                free(s->pix);
                s->pix = malloc((size_t)b->w * b->h * k * k * 4);
                s->w = b->w * k; s->h = b->h * k;
                s->dx0 = s->dy0 = 0; s->dx1 = s->w; s->dy1 = s->h;
            }
            if (s->dx1 <= s->dx0) { s->dx0 = s->dy0 = 0; s->dx1 = s->w; s->dy1 = s->h; }
            if (s->dx0 < 0) s->dx0 = 0;
            if (s->dy0 < 0) s->dy0 = 0;
            if (s->dx1 > s->w) s->dx1 = s->w;
            if (s->dy1 > s->h) s->dy1 = s->h;
            for (int y = s->dy0; y < s->dy1; y++) {
                uint32_t *src = (uint32_t *)((char *)b->pool->data + b->off + y / k * b->stride);
                if (k == 1) memcpy(s->pix + y * s->w + s->dx0, src + s->dx0, (s->dx1 - s->dx0) * 4);
                else for (int x = s->dx0; x < s->dx1; x++) s->pix[y * s->w + x] = src[x / k];
            }
            s->argb = b->fmt == WL_SHM_FORMAT_ARGB8888;
            s->has_buf = true;
            wl_buffer_send_release(b->res);
        } else if (s->has_buf) {
            struct tl *rt = rootof(s);
            s->has_buf = false;
            if (s->role == R_TOP || s->role == R_X) {
                if (s->root) { tl_destroy(s->root); s->root = NULL; }
            } else if (rt && rt->pix) {
                int ax, ay;
                surf_rect(s, &ax, &ay);
                compose(rt, ax, ay, ax + s->w, ay + s->h);
                present(rt);
            }
        }
        wl_list_remove(&s->bl.link);
        wl_list_init(&s->bl.link);
        s->attached = false;
        s->pend = NULL;
    }
    wl_list_insert_list(s->frames.prev, &s->pframes);
    wl_list_init(&s->pframes);
    xdg_commit(s);
    xwm_surf_commit(s);
    commit_done(s);
    s->dx0 = s->dy0 = s->dx1 = s->dy1 = 0;
}

/* redraw what the commit touched and push it to the window */
void commit_done(struct surf *s) {
    struct tl *t = rootof(s);
    if (!t || !s->has_buf || t->dead) return;
    if (t->h < 0) {
        if (s->root) tl_open(t);
        return;
    }
    int ax, ay, x0 = s->dx0, y0 = s->dy0, x1 = s->dx1, y1 = s->dy1;
    surf_rect(s, &ax, &ay);
    if (x1 <= x0) { x0 = 0; y0 = 0; x1 = s->w; y1 = s->h; }
    compose(t, ax + x0, ay + y0, ax + x1, ay + y1);
    present(t);
}

static const struct wl_surface_interface surf_impl = {
    s_destroy_req, s_attach, s_damage, s_frame, s_noop_reg, s_noop_reg, s_commit, s_noop_i, s_bscale, s_dmgbuf, s_offset
};

static void surf_free(struct wl_resource *r) {
    struct surf *s = wl_resource_get_user_data(r);
    struct surf *c, *n;
    wl_list_remove(&s->bl.link);
    if (s->xs) wl_resource_set_user_data(s->xs, NULL);
    wl_list_for_each_safe(c, n, &s->subs, link) { c->parent = NULL; wl_list_remove(&c->link); wl_list_init(&c->link); }
    wl_list_for_each_safe(c, n, &s->popups, link) { c->parent = NULL; wl_list_remove(&c->link); wl_list_init(&c->link); }
    surf_unmap(s);
    struct frame *f, *fn;
    wl_list_for_each_safe(f, fn, &s->frames, link) wl_resource_destroy(f->res);
    wl_list_for_each_safe(f, fn, &s->pframes, link) wl_resource_destroy(f->res);
    wl_list_remove(&s->glink);
    xwm_surf_gone(s);
    free(s->pix);
    free(s);
}

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surf *s = calloc(1, sizeof(*s));
    s->bs = s->k = 1;
    s->cl = c;
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &surf_impl, s, surf_free);
    wl_list_init(&s->pframes); wl_list_init(&s->frames);
    wl_list_init(&s->subs); wl_list_init(&s->popups); wl_list_init(&s->link);
    wl_list_init(&s->bl.link);
    s->bl.notify = pend_gone;
    wl_list_insert(&surfs, &s->glink);
}

static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *rr = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(rr, &reg_impl, NULL, NULL);
}
static const struct wl_compositor_interface comp_impl = { comp_create_surface, comp_create_region };

static void comp_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver > 5 ? 5 : ver, id);
    wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

/* ---- subsurfaces ---- */

static void sub_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void sub_pos(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {
    struct surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->px = x * scale; s->py = y * scale;
}

static void sub_order(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib) {}
static void sub_mode(struct wl_client *c, struct wl_resource *r) {}
static const struct wl_subsurface_interface sub_impl = { sub_destroy, sub_pos, sub_order, sub_order, sub_mode, sub_mode };

static void sub_free(struct wl_resource *r) {
    struct surf *s = wl_resource_get_user_data(r);
    if (!s) return;
    s->role = R_NONE;
    surf_unmap(s);
}

static void subc_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static void subc_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *sr, struct wl_resource *pr) {
    struct surf *s = wl_resource_get_user_data(sr), *p = wl_resource_get_user_data(pr);
    struct wl_resource *sub = wl_resource_create(c, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(sub, &sub_impl, s, sub_free);
    s->role = R_SUB;
    s->parent = p;
    wl_list_remove(&s->link);
    wl_list_insert(p->subs.prev, &s->link);
    sub_cnt++;
}
static const struct wl_subcompositor_interface subc_impl = { subc_destroy, subc_get };

static void subc_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, 1, id);
    wl_resource_set_implementation(r, &subc_impl, NULL, NULL);
}

/* ---- toplevel windows ---- */

void tl_open(struct tl *t) {
    // wl-copy has a 1x1 window, kernel wants 16 at least
    uopen_t o = { t->s->w < 16 ? 16 : t->s->w, t->s->h < 16 ? 16 : t->s->h, 1, F_WL, (uint64_t)(uintptr_t)t->title };
    int ww = o.w, hh = o.h;
    if (o.w > scr_w - 8) o.w = scr_w - 8;
    if (o.h > scr_h - 80) o.h = scr_h - 80;
    long h = sm(OP_OPEN, (long)&o, 0, 0);
    if (h < 0) { fprintf(stderr, "samara-wl: window open failed %ld\n", h); return; }
    t->h = h;
    t->cw = o.w; t->ch = o.h;
    t->pix = calloc((size_t)t->cw * t->ch, 4);
    compose(t, 0, 0, t->cw, t->ch);
    present(t);
    if (!t->xwin && (o.w != ww || o.h != hh)) xdg_top_configure(t, o.w, o.h);
}

void tl_resized(struct tl *t, int cw, int ch) {
    if (cw == t->cw && ch == t->ch) return;
    free(t->pix);
    t->cw = cw; t->ch = ch;
    t->pix = calloc((size_t)cw * ch, 4);
    if (t->xwin) xwm_resize(t, cw, ch);
    else xdg_top_configure(t, cw, ch);
    compose(t, 0, 0, cw, ch);
    present(t);
}

void tl_destroy(struct tl *t) {
    if (t->h >= 0) sm(OP_CLOSE, t->h, 0, 0);
    if (kfocus == t) kbd_leave();
    if (t->xt) wl_resource_set_user_data(t->xt, NULL);
    if (t->deco) wl_resource_set_user_data(t->deco, NULL);
    ptr_gone(t->s);
    wl_list_remove(&t->link);
    if (t->s) t->s->root = NULL;
    free(t->pix);
    free(t);
}

void frames_tick(void) {
    struct surf *s;
    struct frame *f, *n;
    uint32_t ms = now_ms();
    wl_list_for_each(s, &surfs, glink) {
        if (wl_list_empty(&s->frames)) continue;
        wl_list_for_each_safe(f, n, &s->frames, link) {
            wl_callback_send_done(f->res, ms);
            wl_resource_destroy(f->res);
        }
    }
}

void comp_init(void) {
    wl_list_init(&surfs);
    wl_global_create(dpy, &wl_compositor_interface, 5, NULL, comp_bind);
    wl_global_create(dpy, &wl_subcompositor_interface, 1, NULL, subc_bind);
    wl_global_create(dpy, &wl_shm_interface, 1, NULL, shm_bind);
    wl_global_create(dpy, &wl_output_interface, 3, NULL, out_bind);
}
