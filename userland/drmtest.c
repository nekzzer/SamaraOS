// kms smoke test: modeset, dumb buffers, page flip + events, cursor, mode change
// build: toolchain/musl/i686-linux-musl-cross/bin/i686-linux-musl-gcc -static -O2 -o drmtest userland/drmtest.c
// usage: drmtest [hold_seconds] [/dev/dri/cardN]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct mode { uint32_t clock; uint16_t hd, hss, hse, ht, hsk, vd, vss, vse, vt, vsc; uint32_t vref, flags, type; char name[32]; };
struct res { uint64_t fb, crtc, conn, enc; uint32_t cfb, ccrtc, cconn, cenc, minw, maxw, minh, maxh; };
struct crtc { uint64_t conns; uint32_t nconn, id, fb, x, y, gamma, valid; struct mode m; };
struct conn { uint64_t encs, modes, props, vals; uint32_t nmodes, nprops, nencs, enc_id, id, type, type_id, st, mmw, mmh, sub, pad; };
struct dumb { uint32_t h, w, bpp, flags, handle, pitch; uint64_t size; };
struct mapd { uint32_t handle, pad; uint64_t off; };
struct fbc { uint32_t id, w, h, pitch, bpp, depth, handle; };
struct flip { uint32_t crtc, fb, flags, res; uint64_t ud; };
struct cur { uint32_t flags, crtc, x, y, w, h, handle; int32_t hx, hy; };
struct ev { uint32_t type, len; uint64_t ud; uint32_t sec, usec, seq, crtc; };
struct cap { uint64_t c, v; };

#define IOW(nr, t) _IOC(_IOC_READ | _IOC_WRITE, 'd', nr, sizeof(t))
#define CHK(x) do { if ((x) < 0) { printf("fail %s: %s\n", #x, strerror(errno)); return 1; } } while (0)

int fd;
struct dumb bo[2];
uint32_t *map[2], fbid[2];

static void fill(int n, int w, int h, int ph) {
    uint32_t* p = map[n];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t r = (x + ph * 8) * 255 / w & 255, g = y * 255 / h, b = (n ? 255 : 40);
            p[y * (bo[n].pitch / 4) + x] = r << 16 | g << 8 | b;
        }
    // moving box so a flip is visible
    int bx = (ph * 24) % (w - 60);
    for (int y = h / 2; y < h / 2 + 50; y++)
        for (int x = bx; x < bx + 50; x++) p[y * (bo[n].pitch / 4) + x] = 0xffffff;
}

static int mkbuf(int n, int w, int h) {
    bo[n] = (struct dumb){ .w = w, .h = h, .bpp = 32 };
    CHK(ioctl(fd, IOW(0xb2, struct dumb), &bo[n]));
    struct fbc f = { .w = w, .h = h, .pitch = bo[n].pitch, .bpp = 32, .depth = 24, .handle = bo[n].handle };
    CHK(ioctl(fd, IOW(0xae, struct fbc), &f));
    fbid[n] = f.id;
    struct mapd m = { .handle = bo[n].handle };
    CHK(ioctl(fd, IOW(0xb3, struct mapd), &m));
    map[n] = mmap(0, bo[n].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.off);
    if (map[n] == MAP_FAILED) { printf("mmap: %s\n", strerror(errno)); return -1; }
    return 0;
}

int main(int argc, char** argv) {
    int hold = argc > 1 ? atoi(argv[1]) : 0;
    fd = open(argc > 2 ? argv[2] : "/dev/dri/card0", O_RDWR);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }

    char nm[32] = "";
    struct { int a, b, c; size_t nl; char* n; size_t dl; char* d; size_t xl; char* x; } v = { 0, 0, 0, 31, nm, 0, 0, 0, 0 };
    CHK(ioctl(fd, IOW(0x00, v), &v));
    printf("driver %s %d.%d\n", nm, v.a, v.b);

    struct cap c = { 1, 0 };
    CHK(ioctl(fd, IOW(0x0c, c), &c));
    printf("dumb=%d\n", (int)c.v);
    c = (struct cap){ 1, 1 };
    ioctl(fd, IOW(0x0d, c), &c);                    // universal planes

    struct res r = {0};
    CHK(ioctl(fd, IOW(0xa0, r), &r));
    uint32_t crtcs[8], conns[8];
    r.crtc = (uintptr_t)crtcs; r.conn = (uintptr_t)conns; r.fb = 0; r.enc = 0;
    r.cfb = 0; r.cenc = 0;
    CHK(ioctl(fd, IOW(0xa0, r), &r));
    printf("crtcs=%d conns=%d\n", r.ccrtc, r.cconn);

    struct conn cn = { .id = conns[0] };
    CHK(ioctl(fd, IOW(0xa7, cn), &cn));
    struct mode* ms = calloc(cn.nmodes, sizeof *ms);
    uint32_t encs[4], props[8]; uint64_t vals[8];
    cn.modes = (uintptr_t)ms; cn.encs = (uintptr_t)encs; cn.props = (uintptr_t)props; cn.vals = (uintptr_t)vals;
    CHK(ioctl(fd, IOW(0xa7, cn), &cn));
    printf("connector %u: %u modes, state %u, first %s\n", cn.id, cn.nmodes, cn.st, ms[0].name);
    struct mode m0 = ms[0];
    struct mode m1 = ms[cn.nmodes > 2 ? 2 : 0];

    if (mkbuf(0, m0.hd, m0.vd) || mkbuf(1, m0.hd, m0.vd)) return 1;
    fill(0, m0.hd, m0.vd, 0);
    fill(1, m0.hd, m0.vd, 1);

    struct crtc cr = { .id = crtcs[0], .fb = fbid[0], .valid = 1, .m = m0, .nconn = 1, .conns = (uintptr_t)conns };
    CHK(ioctl(fd, IOW(0xa2, cr), &cr));
    printf("modeset %s ok\n", m0.name);

    int cur_fb = 0, ev_n = 0;
    for (int i = 0; i < 12; i++) {
        int nx = cur_fb ^ 1;
        fill(nx, m0.hd, m0.vd, i + 2);
        struct flip f = { .crtc = crtcs[0], .fb = fbid[nx], .flags = 1, .ud = 100 + i };
        CHK(ioctl(fd, IOW(0xb0, struct flip), &f));
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0) { printf("no flip event %d\n", i); return 1; }
        struct ev e;
        if (read(fd, &e, sizeof e) != sizeof e || e.type != 2 || e.ud != 100 + i) { printf("bad event\n"); return 1; }
        ev_n++;
        cur_fb = nx;
    }
    printf("flips ok (%d events)\n", ev_n);

    struct dumb cb = { .w = 64, .h = 64, .bpp = 32 };
    CHK(ioctl(fd, IOW(0xb2, struct dumb), &cb));
    struct mapd cm = { .handle = cb.handle };
    CHK(ioctl(fd, IOW(0xb3, struct mapd), &cm));
    uint32_t* cp = mmap(0, cb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, cm.off);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int d = (x - 32) * (x - 32) + (y - 32) * (y - 32);
            cp[y * 64 + x] = d < 900 ? 0xffff2020 : 0;
        }
    struct cur cc = { .flags = 1 | 2, .crtc = crtcs[0], .x = 200, .y = 150, .w = 64, .h = 64, .handle = cb.handle };
    int cr2 = ioctl(fd, IOW(0xbb, cc), &cc);
    printf("cursor %s\n", cr2 < 0 ? strerror(errno) : "ok");
    for (int i = 0; i < 10; i++) {
        struct cur mv = { .flags = 2, .crtc = crtcs[0], .x = 200 + i * 20, .y = 150 + i * 10 };
        ioctl(fd, IOW(0xa3, mv), &mv);
        usleep(20000);
    }

    if (cn.nmodes > 2) {
        struct crtc c2 = { .id = crtcs[0], .valid = 1, .m = m1, .nconn = 1, .conns = (uintptr_t)conns };
        // new fb of the right size
        int w = m1.hd, h = m1.vd;
        if (mkbuf(1, w, h)) return 1;
        fill(1, w, h, 3);
        c2.fb = fbid[1];
        CHK(ioctl(fd, IOW(0xa2, c2), &c2));
        struct crtc g = { .id = crtcs[0] };
        CHK(ioctl(fd, IOW(0xa1, g), &g));
        printf("mode change -> %s, now %dx%d\n", m1.name, g.m.hd, g.m.vd);
    }
    printf("done, holding %ds\n", hold);
    sleep(hold);
    return 0;
}
