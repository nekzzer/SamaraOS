/* glue for toaru's yutani on samara: pex over loopback tcp, fswait on poll,
   /dev/input -> toaru mouse/kbd pipes, framebuffer through write() on /dev/fb0.
   toaru code itself is NCSA (c) K. Lange, see toolchain/ports/toaruos/LICENSE */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <toaru/pex.h>
#include <toaru/graphics.h>
#include <toaru/mouse.h>

/* ---- pex ---- */

// packets on the wire: u32 size, then data. source id = client fd + 1, 0 = broadcast
static int srv_fd = -1;
static int clients[256];
static int nclients;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;

static int pex_port(const char* name) {
    uint32_t h = 5381;
    while (*name) h = h * 33 + (uint8_t)*name++;
    return 20000 + h % 20000;
}

static int readall(int fd, void* buf, size_t n) {
    char* p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

static int writeall(int fd, const void* buf, size_t n) {
    const char* p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

static int send_pkt(int fd, size_t size, const char* blob) {
    char tmp[4 + MAX_PACKET_SIZE];
    uint32_t sz = size;
    memcpy(tmp, &sz, 4);
    memcpy(tmp + 4, blob, size);
    pthread_mutex_lock(&send_lock);
    int r = writeall(fd, tmp, 4 + size);   // one write, so the header doesn't go alone
    pthread_mutex_unlock(&send_lock);
    return r < 0 ? -1 : (int)size;
}

FILE* pex_bind(char* target) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(pex_port(target)) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(s, 16) < 0) { close(s); return NULL; }
    srv_fd = s;
    FILE* f = fdopen(s, "a+");
    if (f) setbuf(f, NULL);
    return f;
}

FILE* pex_connect(char* target) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(pex_port(target)) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) { close(s); return NULL; }
    FILE* f = fdopen(s, "r+");
    if (f) setbuf(f, NULL);
    return f;
}

size_t pex_send(FILE* sock, uintptr_t rcpt, size_t size, char* blob) {
    if (size > MAX_PACKET_SIZE) return -E2BIG;
    if (rcpt) return send_pkt(rcpt - 1, size, blob);
    for (int i = 0; i < nclients; i++) send_pkt(clients[i], size, blob);
    return size;
}

size_t pex_broadcast(FILE* sock, size_t size, char* blob) {
    return pex_send(sock, 0, size, blob);
}

// fill pfd with the listen socket + every client
static int srv_pollset(struct pollfd* p) {
    p[0].fd = srv_fd; p[0].events = POLLIN; p[0].revents = 0;
    for (int i = 0; i < nclients; i++) { p[i + 1].fd = clients[i]; p[i + 1].events = POLLIN; p[i + 1].revents = 0; }
    return nclients + 1;
}

size_t pex_listen(FILE* sock, pex_packet_t* packet) {
    struct pollfd p[257];
    for (;;) {
        int n = srv_pollset(p);
        if (poll(p, n, -1) <= 0) continue;
        if (p[0].revents & POLLIN) {
            int c = accept(srv_fd, NULL, NULL);
            if (c >= 0 && nclients < 256) clients[nclients++] = c;
            else if (c >= 0) close(c);
        }
        for (int i = 1; i < n; i++) {
            if (!p[i].revents) continue;
            int fd = p[i].fd;
            uint32_t sz;
            packet->source = fd + 1;
            if (readall(fd, &sz, 4) < 0 || sz > MAX_PACKET_SIZE || readall(fd, packet->data, sz) < 0) {
                // gone: size 0 tells yutani to drop its windows
                for (int k = 0; k < nclients; k++)
                    if (clients[k] == fd) { clients[k] = clients[--nclients]; break; }
                close(fd);
                packet->size = 0;
                return 0;
            }
            packet->size = sz;
            return sz;
        }
    }
}

size_t pex_reply(FILE* sock, size_t size, char* blob) {
    return send_pkt(fileno(sock), size, blob);
}

size_t pex_recv(FILE* sock, char* blob) {
    uint32_t sz;
    memset(blob, 0, MAX_PACKET_SIZE);
    if (readall(fileno(sock), &sz, 4) < 0 || sz > MAX_PACKET_SIZE) return 0;
    if (readall(fileno(sock), blob, sz) < 0) return 0;
    return sz;
}

size_t pex_query(FILE* sock) {
    struct pollfd p = { fileno(sock), POLLIN, 0 };
    return poll(&p, 1, 0) > 0;
}

/* ---- fswait ---- */

int fswait3(int count, int* fds, int timeout, int* out) {
    // the server fd counts as ready when any client has data too
    struct pollfd p[300];
    int n = 0, owner[300];
    for (int i = 0; i < count; i++) {
        if (fds[i] == srv_fd && srv_fd >= 0) {
            struct pollfd tmp[257];
            int k = srv_pollset(tmp);
            for (int j = 0; j < k; j++) { p[n] = tmp[j]; owner[n++] = i; }
        } else {
            p[n].fd = fds[i]; p[n].events = POLLIN; p[n].revents = 0; owner[n++] = i;
        }
    }
    int r = poll(p, n, timeout);
    if (out) for (int i = 0; i < count; i++) out[i] = 0;
    int first = count;
    for (int i = 0; i < n && r > 0; i++) {
        if (!p[i].revents) continue;
        if (out) out[owner[i]] = 1;
        if (owner[i] < first) first = owner[i];
    }
    return first;
}

int fswait2(int count, int* fds, int timeout) { return fswait3(count, fds, timeout, NULL); }
int fswait(int count, int* fds) { return fswait3(count, fds, -1, NULL); }

/* ---- input ---- */

// /dev/input gives linux input_events. keys: linux keycodes == set 1 scancodes
// for the plain ones, the rest need an e0 prefix
static int mpipe[2] = { -1, -1 }, kpipe[2] = { -1, -1 };

static int e0key(int code) {
    switch (code) {
        case 96: return 0x1C; case 97: return 0x1D; case 98: return 0x35; case 100: return 0x38;
        case 102: return 0x47; case 103: return 0x48; case 104: return 0x49; case 105: return 0x4B;
        case 106: return 0x4D; case 107: return 0x4F; case 108: return 0x50; case 109: return 0x51;
        case 110: return 0x52; case 111: return 0x53; case 125: return 0x5B; case 126: return 0x5C;
    }
    return 0;
}

static void* input_thread(void* arg) {
    int fd = (int)(intptr_t)arg;
    struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } ev[64];
    mouse_device_packet_t mp = { MOUSE_MAGIC, 0, 0, 0 };
    int btn = 0;
    for (;;) {
        ssize_t r = read(fd, ev, sizeof(ev));
        if (r <= 0) { usleep(10000); continue; }
        int dirty = 0;
        for (int i = 0; i < r / (int)sizeof(ev[0]); i++) {
            int t = ev[i].type, c = ev[i].code, v = ev[i].value;
            if (t == 2) {                                     /* EV_REL */
                if (c == 0) mp.x_difference += v;
                if (c == 1) mp.y_difference -= v;             // toaru: up is +
                if (c == 8) { mp.buttons = btn | (v > 0 ? MOUSE_SCROLL_UP : MOUSE_SCROLL_DOWN); dirty = 1; }
                else dirty = 1;
            } else if (t == 1 && c >= 0x110 && c <= 0x112) {  /* BTN_LEFT/RIGHT/MIDDLE */
                int b = c == 0x110 ? LEFT_CLICK : c == 0x111 ? RIGHT_CLICK : MIDDLE_CLICK;
                btn = v ? (btn | b) : (btn & ~b);
                dirty = 1;
            } else if (t == 1 && v != 2) {                    // keys, skip autorepeat
                unsigned char b[2];
                int n = 0, e = e0key(c);
                if (e) { b[n++] = 0xE0; b[n++] = e | (v ? 0 : 0x80); }
                else if (c < 0x59) b[n++] = c | (v ? 0 : 0x80);
                if (n) write(kpipe[1], b, n);
            }
        }
        // our /dev/input sends no EV_SYN, so one packet per read() batch
        if (dirty) {
            if (!(mp.buttons & (MOUSE_SCROLL_UP | MOUSE_SCROLL_DOWN))) mp.buttons = btn;
            write(mpipe[1], &mp, sizeof(mp));
            mp.x_difference = mp.y_difference = 0;
            mp.buttons = btn;
        }
    }
    return NULL;
}

static void input_start(void) {
    if (mpipe[0] >= 0) return;
    int fd = open("/dev/input-all", O_RDONLY);
    if (fd < 0) { perror("/dev/input-all"); return; }
    pipe(mpipe);
    pipe(kpipe);
    pthread_t t;
    pthread_create(&t, NULL, input_thread, (void*)(intptr_t)fd);
}

int samara_mouse_fd(void) { input_start(); return mpipe[0]; }
int samara_kbd_fd(void) { input_start(); return kpipe[0]; }

/* ---- framebuffer ---- */

extern void toaru_flip(gfx_context_t* ctx);
static int fb_fd = -1;
static gfx_context_t* fb_ctx;

gfx_context_t* init_graphics_fullscreen(void) {
    if (fb_fd < 0) fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) return NULL;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo fx;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &v);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fx);
    gfx_context_t* out = calloc(1, sizeof(gfx_context_t));
    out->width = v.xres;
    out->height = v.yres;
    out->depth = 32;
    out->stride = fx.line_length;
    out->size = out->stride * out->height;
    out->buffer = calloc(1, out->size);   // never shown, flip writes the backbuffer to fb0
    out->backbuffer = out->buffer;
    fb_ctx = out;
    return out;
}

void reinit_graphics_fullscreen(gfx_context_t* out) {
    // no mode switching here
}

uint32_t framebuffer_stride(void) {
    return fb_ctx ? fb_ctx->stride : 0;
}

void flip(gfx_context_t* ctx) {
    if (ctx != fb_ctx) { toaru_flip(ctx); return; }
    uint32_t s = ctx->stride;
    if (!ctx->clips) {
        lseek(fb_fd, 0, SEEK_SET);
        write(fb_fd, ctx->backbuffer, s * ctx->height);
        return;
    }
    // only the dirty rows, glued into runs
    int y = 0, h = ctx->height;
    while (y < h) {
        if (y >= ctx->clips_size || !ctx->clips[y]) { y++; continue; }
        int y0 = y;
        while (y < h && y < ctx->clips_size && ctx->clips[y]) y++;
        lseek(fb_fd, (off_t)y0 * s, SEEK_SET);
        write(fb_fd, ctx->backbuffer + (size_t)y0 * s, (size_t)(y - y0) * s);
    }
}

/* TOARU_SEGV=1: dump the stack on a crash, then addr2line by hand */
#include <signal.h>
#include <ucontext.h>
static void segv(int s, siginfo_t* si, void* uc) {
    ucontext_t* u = uc;   // gregs 7 = esp, 14 = eip (REG_* need _GNU_SOURCE before the shim)
    uint32_t* sp = (uint32_t*)u->uc_mcontext.gregs[7];
    fprintf(stderr, "segv eip=%08x addr=%p esp=%p\n", u->uc_mcontext.gregs[14], si->si_addr, (void*)sp);
    for (int i = 0; i < 64; i++) fprintf(stderr, "%08x%c", sp[i], i % 8 == 7 ? '\n' : ' ');
    _exit(139);
}
__attribute__((constructor)) static void segv_init(void) {
    if (!getenv("TOARU_SEGV")) return;
    struct sigaction sa = { 0 };
    sa.sa_sigaction = segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
}
