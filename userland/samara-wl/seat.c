#include "swl.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/syscall.h>

struct kr { struct wl_resource *r; struct wl_list link; };
static struct wl_list kbds, ptrs;
static struct xkb_context *xc;
static struct xkb_keymap *km;
static struct xkb_state *xst;
static int km_fd, km_size;
struct tl *kfocus;
static struct surf *pfocus;
static uint8_t held[256];
static int cur_grp;
static uint32_t d_mods, l_mods, k_mods;

static void kr_free(struct wl_resource *r) {
    struct kr *k = wl_resource_get_user_data(r);
    wl_list_remove(&k->link);
    free(k);
}

static void send_mods(void) {
    struct kr *k;
    uint32_t sr = next_serial();
    if (!kfocus) return;
    wl_list_for_each(k, &kbds, link)
        if (wl_resource_get_client(k->r) == kfocus->s->cl)
            wl_keyboard_send_modifiers(k->r, sr, d_mods, l_mods, k_mods, cur_grp);
}

static void grab_mods(void) {
    d_mods = xkb_state_serialize_mods(xst, XKB_STATE_MODS_DEPRESSED);
    l_mods = xkb_state_serialize_mods(xst, XKB_STATE_MODS_LATCHED);
    k_mods = xkb_state_serialize_mods(xst, XKB_STATE_MODS_LOCKED);
}

void kbd_enter(struct tl *t) {
    struct kr *k;
    struct wl_array keys;
    if (kfocus == t) return;
    if (kfocus) kbd_leave();
    kfocus = t;
    wl_array_init(&keys);
    uint32_t sr = next_serial();
    wl_list_for_each(k, &kbds, link)
        if (wl_resource_get_client(k->r) == t->s->cl) {
            wl_keyboard_send_enter(k->r, sr, t->s->res, &keys);
            wl_keyboard_send_modifiers(k->r, sr, d_mods, l_mods, k_mods, cur_grp);
        }
    wl_array_release(&keys);
}

void kbd_leave(void) {
    struct kr *k;
    if (!kfocus) return;
    uint32_t sr = next_serial();
    wl_list_for_each(k, &kbds, link)
        if (wl_resource_get_client(k->r) == kfocus->s->cl)
            wl_keyboard_send_leave(k->r, sr, kfocus->s->res);
    kfocus = NULL;
    memset(held, 0, sizeof(held));
}

void kbd_event(int code, int down, int ru) {
    struct kr *k;
    if (!kfocus || code <= 0 || code > 255) return;
    if (down == held[code]) return;               /* typematic repeats: the client does its own */
    held[code] = down;
    if (ru != cur_grp) {
        cur_grp = ru;
        xkb_state_update_mask(xst, d_mods, l_mods, k_mods, 0, 0, ru);
        send_mods();
    }
    xkb_state_update_key(xst, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    uint32_t sr = next_serial(), ms = now_ms();
    wl_list_for_each(k, &kbds, link)
        if (wl_resource_get_client(k->r) == kfocus->s->cl)
            wl_keyboard_send_key(k->r, sr, ms, code, down ? 1 : 0);
    uint32_t od = d_mods, ol = l_mods, ok = k_mods;
    grab_mods();
    if (od != d_mods || ol != l_mods || ok != k_mods) send_mods();
}

static void kb_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_keyboard_interface kb_impl = { kb_release };

/* ---- pointer ---- */

static void pt_cursor(struct wl_client *c, struct wl_resource *r, uint32_t sr, struct wl_resource *s, int32_t x, int32_t y) {}
static void pt_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pt_impl = { pt_cursor, pt_release };

static void pframe(struct wl_client *c) {
    struct kr *k;
    wl_list_for_each(k, &ptrs, link)
        if (wl_resource_get_client(k->r) == c && wl_resource_get_version(k->r) >= 5) wl_pointer_send_frame(k->r);
}

static void pfocus_set(struct surf *s, int x, int y) {
    struct kr *k;
    if (s == pfocus) return;
    if (pfocus) {
        uint32_t sr = next_serial();
        wl_list_for_each(k, &ptrs, link)
            if (wl_resource_get_client(k->r) == pfocus->cl) wl_pointer_send_leave(k->r, sr, pfocus->res);
        pframe(pfocus->cl);
    }
    pfocus = s;
    if (!s) return;
    uint32_t sr = next_serial();
    wl_list_for_each(k, &ptrs, link)
        if (wl_resource_get_client(k->r) == s->cl)
            wl_pointer_send_enter(k->r, sr, s->res, wl_fixed_from_int(x), wl_fixed_from_int(y));
    pframe(s->cl);
}

void ptr_gone(struct surf *s) { if (pfocus == s) pfocus = NULL; }

void ptr_event(struct tl *t, uev_t *e) {
    struct kr *k;
    int lx = 0, ly = 0;
    uint32_t ms = now_ms();
    if (e->type == EV_PLEAVE) { pfocus_set(NULL, 0, 0); return; }
    if (e->type == EV_PENTER || e->type == EV_PMOVE) {
        struct surf *s = pick(t, e->a, e->b, &lx, &ly);
        pfocus_set(s, lx, ly);
        if (e->type == EV_PMOVE && pfocus) {
            wl_list_for_each(k, &ptrs, link)
                if (wl_resource_get_client(k->r) == pfocus->cl)
                    wl_pointer_send_motion(k->r, ms, wl_fixed_from_int(lx), wl_fixed_from_int(ly));
            pframe(pfocus->cl);
        }
        return;
    }
    if (e->type == EV_PBTN) {
        if (!pfocus) return;
        if (e->b && pfocus->role != R_POPUP && pfocus->role != R_XOR) popup_dismiss(t);
        uint32_t sr = next_serial();
        wl_list_for_each(k, &ptrs, link)
            if (wl_resource_get_client(k->r) == pfocus->cl)
                wl_pointer_send_button(k->r, sr, ms, e->a, e->b ? 1 : 0);
        pframe(pfocus->cl);
        return;
    }
    if (e->type == EV_WHEEL && pfocus) {
        wl_list_for_each(k, &ptrs, link)
            if (wl_resource_get_client(k->r) == pfocus->cl) {
                int v = wl_resource_get_version(k->r);
                if (v >= 5) wl_pointer_send_axis_discrete(k->r, 0, e->a);
                wl_pointer_send_axis(k->r, ms, 0, wl_fixed_from_int(e->a * 10));
            }
        pframe(pfocus->cl);
    }
}

/* ---- seat ---- */

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct kr *k = calloc(1, sizeof(*k));
    k->r = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(k->r, &pt_impl, k, kr_free);
    wl_list_insert(&ptrs, &k->link);
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct kr *k = calloc(1, sizeof(*k));
    int v = wl_resource_get_version(r);
    k->r = wl_resource_create(c, &wl_keyboard_interface, v, id);
    wl_resource_set_implementation(k->r, &kb_impl, k, kr_free);
    wl_list_insert(&kbds, &k->link);
    wl_keyboard_send_keymap(k->r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, km_fd, km_size);
    if (v >= 4) wl_keyboard_send_repeat_info(k->r, 30, 400);
    if (kfocus && kfocus->s->cl == c) {
        struct wl_array keys;
        wl_array_init(&keys);
        uint32_t sr = next_serial();
        wl_keyboard_send_enter(k->r, sr, kfocus->s->res, &keys);
        wl_keyboard_send_modifiers(k->r, sr, d_mods, l_mods, k_mods, cur_grp);
        wl_array_release(&keys);
    }
}

static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    wl_resource_post_error(r, 0, "no touch");
}
static void seat_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = { seat_get_pointer, seat_get_keyboard, seat_get_touch, seat_release };

static void seat_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, ver > 5 ? 5 : ver, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (ver >= 2) wl_seat_send_name(r, "seat0");
}

void seat_init(void) {
    wl_list_init(&kbds);
    wl_list_init(&ptrs);
    xc = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names rn = { "evdev", "pc105", "us,ru", "", "" };
    km = xkb_keymap_new_from_names(xc, &rn, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) { fprintf(stderr, "samara-wl: xkb keymap failed\n"); exit(1); }
    xst = xkb_state_new(km);
    char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    km_size = strlen(str) + 1;
    km_fd = syscall(SYS_memfd_create, "keymap", 0);
    ftruncate(km_fd, km_size);
    char *m = mmap(NULL, km_size, PROT_READ | PROT_WRITE, MAP_SHARED, km_fd, 0);
    memcpy(m, str, km_size);
    munmap(m, km_size);
    free(str);
    wl_global_create(dpy, &wl_seat_interface, 5, NULL, seat_bind);
    data_init();
}

/* ---- clipboard: wl_data_device_manager, selection only ---- */

struct dsrc { struct wl_resource *res; char mime[8][64]; int n; };
static struct dsrc *sel;
static struct wl_list ddevs;
struct dd { struct wl_resource *r; struct wl_list link; };

static void off_accept(struct wl_client *c, struct wl_resource *r, uint32_t s, const char *m) {}
static void off_receive(struct wl_client *c, struct wl_resource *r, const char *m, int fd) {
    if (sel) wl_data_source_send_send(sel->res, m, fd);
    close(fd);
}
static void off_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void off_finish(struct wl_client *c, struct wl_resource *r) {}
static void off_actions(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t p) {}
static const struct wl_data_offer_interface off_impl = { off_accept, off_receive, off_destroy, off_finish, off_actions };

static void send_sel(struct dd *d) {
    if (!sel) { wl_data_device_send_selection(d->r, NULL); return; }
    struct wl_client *c = wl_resource_get_client(d->r);
    struct wl_resource *o = wl_resource_create(c, &wl_data_offer_interface, wl_resource_get_version(d->r), 0);
    wl_resource_set_implementation(o, &off_impl, NULL, NULL);
    wl_data_device_send_data_offer(d->r, o);
    for (int i = 0; i < sel->n; i++) wl_data_offer_send_offer(o, sel->mime[i]);
    wl_data_device_send_selection(d->r, o);
}

static void src_offer(struct wl_client *c, struct wl_resource *r, const char *m) {
    struct dsrc *s = wl_resource_get_user_data(r);
    if (s->n < 8) { strncpy(s->mime[s->n], m, 63); s->n++; }
}
static void src_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void src_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) {}
static const struct wl_data_source_interface src_impl = { src_offer, src_destroy, src_actions };

static void src_free(struct wl_resource *r) {
    struct dsrc *s = wl_resource_get_user_data(r);
    struct dd *d;
    if (sel == s) {
        sel = NULL;
        wl_list_for_each(d, &ddevs, link) send_sel(d);
    }
    free(s);
}

static void dd_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, struct wl_resource *o, struct wl_resource *i, uint32_t sr) {}
static void dd_set_sel(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t sr) {
    struct dd *d;
    struct dsrc *old = sel;
    sel = s ? wl_resource_get_user_data(s) : NULL;
    if (old && old != sel) wl_data_source_send_cancelled(old->res);
    wl_list_for_each(d, &ddevs, link) send_sel(d);
}
static void dd_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_data_device_interface dd_impl = { dd_drag, dd_set_sel, dd_release };

static void dd_free(struct wl_resource *r) {
    struct dd *d = wl_resource_get_user_data(r);
    wl_list_remove(&d->link);
    free(d);
}

static void ddm_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct dsrc *s = calloc(1, sizeof(*s));
    s->res = wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &src_impl, s, src_free);
}

static void ddm_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    struct dd *d = calloc(1, sizeof(*d));
    d->r = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(d->r, &dd_impl, d, dd_free);
    wl_list_insert(&ddevs, &d->link);
    if (sel) send_sel(d);
}
static const struct wl_data_device_manager_interface ddm_impl = { ddm_source, ddm_device };

static void ddm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, ver > 3 ? 3 : ver, id);
    wl_resource_set_implementation(r, &ddm_impl, NULL, NULL);
}

void data_init(void) {
    wl_list_init(&ddevs);
    wl_global_create(dpy, &wl_data_device_manager_interface, 3, NULL, ddm_bind);
}
