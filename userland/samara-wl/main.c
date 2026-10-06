#include "swl.h"
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>

struct wl_display *dpy;
struct wl_list tls;
int scr_w, scr_h;
int scale = 1;
static uint32_t serial;
static struct wl_event_source *ftimer;
static struct wl_event_loop *loop;

uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000u + ts.tv_nsec / 1000000;
}

uint64_t in_us;
uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

uint32_t next_serial(void) { return ++serial; }

long sm(long op, long a, long b, long c) { return syscall(500, op, a, b, c); }

static void win_events(struct tl *t) {
    uev_t e;
    for (int n = 0; n < 64 && t->h >= 0 && sm(OP_EVENT, t->h, (long)&e, 0) == 1; n++) {
        switch (e.type) {
        case EV_CLOSE:
            sm(OP_CLOSE, t->h, 0, 0);
            t->h = -1;
            t->dead = true;
            t->dead_t = now_ms();
            if (t->xwin) xwm_close(t); else xdg_close(t);
            break;
        case EV_RESIZE:
            tl_resized(t, e.a, e.b);
            break;
        case EV_FOCUS:
            t->focus = e.a;
            if (e.a) {
                kbd_enter(t);
                if (t->xwin) xwm_focus(t);
            } else if (kfocus == t) kbd_leave();
            if (!t->xwin) xdg_top_configure(t, t->cw, t->ch);
            break;
        case EV_RAWKEY:
            if (!in_us) in_us = now_us();
            if (kfocus == t) kbd_event(e.a, e.b, e.c);
            break;
        case EV_PENTER: case EV_PLEAVE: case EV_PMOVE: case EV_PBTN: case EV_WHEEL:
            if (!in_us) in_us = now_us();
            ptr_event(t, &e);
            break;
        }
    }
}

static int frame_timer(void *d) {
    frames_tick();
    /* a closed window whose client does not leave gets thrown out */
    struct tl *t;
    uint32_t now = now_ms();
    wl_list_for_each(t, &tls, link)
        if (t->dead && now - t->dead_t > 4000) {
            t->dead_t = now;
            if (t->xwin) { /* kill_client via xwm_close if it has no delete protocol */ t->del_ok = false; xwm_close(t); }
            else { wl_client_destroy(t->s->cl); break; }
        }
    wl_event_source_timer_update(ftimer, 16);
    return 0;
}

static int on_term(int sig, void *d) {
    unlink("/run/user/0/wayland-0");
    exit(0);
}

int main(int argc, char **argv) {
    wl_list_init(&tls);
    long r;
    for (int i = 0; i < 100 && (r = sm(OP_SCREEN, 0, 0, 0)) <= 0; i++) usleep(100000);
    scr_w = r >> 16; scr_h = r & 0xFFFF;
    if (scr_w <= 0) { fprintf(stderr, "samara-wl: no desktop\n"); return 1; }

    char *e = getenv("SAMARA_SCALE");
    FILE *f = fopen("/etc/samara-de.conf", "r");
    char ln[64];
    if (e) scale = atoi(e);
    else while (f && fgets(ln, sizeof ln, f)) sscanf(ln, "scale=%d", &scale);
    if (f) fclose(f);
    if (scale < 1 || scale > 4) scale = 1;

    mkdir("/run", 0755);
    mkdir("/run/user", 0755);
    mkdir("/run/user/0", 0700);
    setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
    unlink("/run/user/0/wayland-0");
    unlink("/run/user/0/wayland-0.lock");
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/0/bus", 1);
    if (!fork()) { execl("/bin/sh", "sh", "/etc/samara-session", (char *)NULL); _exit(1); }

    dpy = wl_display_create();
    if (wl_display_add_socket(dpy, "wayland-0")) { fprintf(stderr, "samara-wl: no socket\n"); return 1; }
    comp_init();
    seat_init();
    xdg_init();
    loop = wl_display_get_event_loop(dpy);
    ftimer = wl_event_loop_add_timer(loop, frame_timer, NULL);
    wl_event_source_timer_update(ftimer, 16);
    wl_event_loop_add_signal(loop, SIGTERM, on_term, NULL);
    if (!access("/usr/bin/Xwayland", X_OK)) xwm_start();
    fprintf(stderr, "samara-wl: %dx%d up\n", scr_w, scr_h);

    for (;;) {
        wl_event_loop_dispatch(loop, 4);
        struct tl *t, *n;
        wl_list_for_each_safe(t, n, &tls, link) win_events(t);
        wl_display_flush_clients(dpy);
    }
}
