#include "gui/login.h"
#include "gui/theme.h"
#include "gui/wm.h"
#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "drivers/keyboard.h"
#include "drivers/ata.h"
#include "fs/fs.h"
#include "core/string.h"
#include "core/heap.h"
#include "core/clock.h"
#include "core/task.h"
#include "core/io.h"

/* Login screen + the installer wizard. Both are plain full screen pages
   drawn straight to the framebuffer before the WM starts, keyboard only. */

char login_user[32] = "user";        /* what the shell prompt said before logins existed */

/* ---------------- md5-crypt ($1$), what musl's crypt() checks ---------------- */

typedef struct { uint32_t s[4]; uint32_t n; uint8_t buf[64]; } md5_t;

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_sh[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(uint32_t* s, const uint8_t* p) {
    uint32_t w[16], a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 16; i++)
        w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        int r = md5_sh[(i >> 4) * 4 + (i & 3)];
        uint32_t x = a + f + md5_k[i] + w[g];
        a = d; d = c; c = b;
        b += x << r | x >> (32 - r);
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

static void md5_init(md5_t* m) {
    m->s[0] = 0x67452301; m->s[1] = 0xefcdab89; m->s[2] = 0x98badcfe; m->s[3] = 0x10325476;
    m->n = 0;
}

static void md5_add(md5_t* m, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len--) {
        m->buf[m->n++ & 63] = *p++;
        if (!(m->n & 63)) md5_block(m->s, m->buf);
    }
}

static void md5_end(md5_t* m, uint8_t* out) {
    uint32_t bits = m->n * 8;
    uint8_t z = 0x80;
    md5_add(m, &z, 1);
    z = 0;
    while ((m->n & 63) != 56) md5_add(m, &z, 1);
    for (int i = 0; i < 8; i++) { z = i < 4 ? (uint8_t)(bits >> (i * 8)) : 0; md5_add(m, &z, 1); }
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(m->s[i >> 2] >> ((i & 3) * 8));
}

static const char a64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static char* to64(char* o, uint32_t v, int n) {
    while (n--) { *o++ = a64[v & 63]; v >>= 6; }
    return o;
}

void md5crypt(const char* pw, const char* salt, char* out) {
    uint32_t pl = strlen(pw), sl = 0;
    while (sl < 8 && salt[sl] && salt[sl] != '$') sl++;
    uint8_t f[16];
    md5_t m, alt;
    md5_init(&alt);
    md5_add(&alt, pw, pl); md5_add(&alt, salt, sl); md5_add(&alt, pw, pl);
    md5_end(&alt, f);
    md5_init(&m);
    md5_add(&m, pw, pl); md5_add(&m, "$1$", 3); md5_add(&m, salt, sl);
    for (int i = (int)pl; i > 0; i -= 16) md5_add(&m, f, i > 16 ? 16 : i);
    for (uint32_t i = pl; i; i >>= 1) md5_add(&m, (i & 1) ? "" : pw, 1);
    md5_end(&m, f);
    /* 1000 rounds of nonsense, thats the whole "slow" part of md5crypt */
    for (int i = 0; i < 1000; i++) {
        md5_init(&m);
        if (i & 1) md5_add(&m, pw, pl); else md5_add(&m, f, 16);
        if (i % 3) md5_add(&m, salt, sl);
        if (i % 7) md5_add(&m, pw, pl);
        if (i & 1) md5_add(&m, f, 16); else md5_add(&m, pw, pl);
        md5_end(&m, f);
    }
    char* o = out;
    memcpy(o, "$1$", 3); o += 3;
    memcpy(o, salt, sl); o += sl;
    *o++ = '$';
    o = to64(o, (uint32_t)f[0] << 16 | f[6] << 8 | f[12], 4);
    o = to64(o, (uint32_t)f[1] << 16 | f[7] << 8 | f[13], 4);
    o = to64(o, (uint32_t)f[2] << 16 | f[8] << 8 | f[14], 4);
    o = to64(o, (uint32_t)f[3] << 16 | f[9] << 8 | f[15], 4);
    o = to64(o, (uint32_t)f[4] << 16 | f[10] << 8 | f[5], 4);
    o = to64(o, f[11], 2);
    *o = 0;
}

/* ---------------- /etc ---------------- */

/* /etc lives in ram and gets rebuilt every boot, the real one is on the disk */
void login_load_etc(void) {
    static const char* names[] = { "passwd", "shadow", "group", "hostname" };
    for (int i = 0; i < 4; i++) {
        char src[32] = "/mnt/etc/", dst[32] = "/etc/";
        strcat(src, names[i]);
        strcat(dst, names[i]);
        fs_node_t* s = fs_resolve(fs_root(), src);
        if (!s || s->type != FS_FILE || !s->size) continue;
        fs_node_t* d = fs_resolve(fs_root(), dst);
        if (!d) d = fs_create(fs_root(), dst, FS_FILE);
        if (d) fs_write(d, s->data, s->size);
    }
}

/* finds "name:" line in an /etc file, returns field k (0 = name) */
static bool etc_field(const char* file, const char* name, int k, char* out, int cap) {
    fs_node_t* n = fs_resolve(fs_root(), file);
    if (!n || !n->data) return false;
    const char* p = n->data;
    const char* end = n->data + n->size;
    int nl = strlen(name);
    while (p < end) {
        const char* e = p;
        while (e < end && *e != '\n') e++;
        if (e - p > nl && !strncmp(p, name, nl) && p[nl] == ':') {
            for (int i = 0; i < k && p < e; i++) {
                while (p < e && *p != ':') p++;
                p++;
            }
            int j = 0;
            while (p < e && *p != ':' && j < cap - 1) out[j++] = *p++;
            out[j] = 0;
            return true;
        }
        p = e + 1;
    }
    return false;
}

bool check_pw(const char* user, const char* pw) {
    char h[128], out[64];
    if (!user[0] || !etc_field("/etc/shadow", user, 1, h, sizeof(h))) return false;
    if (!h[0]) return true;                           /* no password set */
    if (strncmp(h, "$1$", 3)) return false;           // TODO $5$/$6$ if busybox passwd wrote it
    md5crypt(pw, h + 3, out);
    return !strcmp(out, h);
}

/* ---------------- drawing ---------------- */

static uint32_t* bg;
static int W, H;
static int cx, cy, cw, ch;          /* the card */

static void bg_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    if (!bg) { gfx_rect_fill(x, y, w, h, C_DESK_TOP); return; }
    for (int r = 0; r < h; r++) gfx_blit_argb(x, y + r, w, 1, bg + (y + r) * W + x);
}

static void screen_init(void) {
    W = gfx_w();
    H = gfx_h();
    gfx_reset_clip();
    bg = wm_background(W, H);
    bg_rect(0, 0, W, H);
}

static int last_min = -1;

static void draw_clock(bool force) {
    uint32_t t = clock_epoch();
    int m = (int)(t / 60 % 1440);
    if (m == last_min && !force) return;
    last_min = m;
    char s[8];
    s[0] = '0' + m / 600; s[1] = '0' + m / 60 % 10; s[2] = ':';
    s[3] = '0' + m % 60 / 10; s[4] = '0' + m % 10; s[5] = 0;
    bg_rect(W / 2 - 100, 40, 200, 40);
    uif_draw_center(W / 2 - 100, 40, 200, 40, UIF_BIG, s, C_BAR_TEXT);
}

static void card(int w, int h) {
    if (w != cw || h != ch) {
        bg_rect(cx - 40, cy - 40, cw + 80, ch + 80);
        cw = w; ch = h;
        cx = (W - w) / 2; cy = (H - h) / 2;
        gfx_shadow(cx, cy, cw, ch, 0, 16, 160);
    }
    // square card + 1px edge, same as the windows
    gfx_rect_fill(cx, cy, cw, ch, C_OUTLINE);
    gfx_rect_fill(cx + 1, cy + 1, cw - 2, ch - 2, C_SURFACE);
}

typedef struct { const char* label; char* buf; int cap; bool secret; } fld_t;

static void draw_field(int x, int y, int w, fld_t* f, bool focus) {
    gfx_rect_fill(x, y, w, 56, C_SURFACE);
    uif_draw(x, y, UIF_SMALL, f->label, C_INK_DIM);
    gfx_rect_fill(x, y + 18, w, 36, focus ? C_ACCENT : C_OUTLINE);
    gfx_rect_fill(x + 1, y + 19, w - 2, 34, RGB(0x10, 0x11, 0x14));   // dark field, light text
    char tmp[64];
    int n = strlen(f->buf);
    if (f->secret) { for (int i = 0; i < n && i < 63; i++) tmp[i] = '*'; tmp[n < 63 ? n : 63] = 0; }
    else strcpy(tmp, f->buf);
    int ex = uif_draw_mid(x + 12, y + 36, UIF_REG, tmp, C_INK);
    if (focus) gfx_rect_fill(ex + 1, y + 27, 2, 18, C_ACCENT);
}

/* typing into a field, true if it changed */
static bool edit(fld_t* f, char k) {
    int n = strlen(f->buf);
    if (k == '\b') {
        if (!n) return false;
        f->buf[n - 1] = 0;
        return true;
    }
    if ((uint8_t)k < 32 || (uint8_t)k >= 127 || n >= f->cap - 1) return false;   /* ascii only, no cp866 in passwords */
    f->buf[n] = k;
    f->buf[n + 1] = 0;
    return true;
}

static void msg(int y, const char* s, uint32_t c) {
    gfx_rect_fill(cx + 20, y, cw - 40, 20, C_SURFACE);
    uif_draw_center(cx, y, cw, 20, UIF_SMALL, s, c);
}

static char wait_key(void) {
    for (;;) {
        if (kbd_has_key()) return kbd_trygetc();
        draw_clock(false);
        task_yield();
    }
}

/* ---------------- login ---------------- */

void login_screen(void) {
    char user[32] = "root", pw[64] = "", host[32] = "samara";
    /* first normal user from /etc/passwd if there is one */
    fs_node_t* pn = fs_resolve(fs_root(), "/etc/passwd");
    if (pn && pn->data) {
        const char* p = pn->data;
        const char* end = p + pn->size;
        while (p < end) {
            const char* e = p;
            while (e < end && *e != '\n') e++;
            const char* c = p;
            while (c < e && *c != ':') c++;
            int len = c - p;
            char uid[12];
            char nm[32];
            if (len > 0 && len < 32) {
                memcpy(nm, p, len); nm[len] = 0;
                if (etc_field("/etc/passwd", nm, 2, uid, sizeof(uid)) && atoi(uid) >= 1000) { strcpy(user, nm); break; }
            }
            p = e + 1;
        }
    }
    fs_node_t* hn = fs_resolve(fs_root(), "/etc/hostname");
    if (hn && hn->data && hn->size) {
        int k = 0;
        while (k < 31 && k < (int)hn->size && hn->data[k] != '\n') { host[k] = hn->data[k]; k++; }
        host[k] = 0;
    }

    fld_t f[2] = { { "User", user, sizeof(user), false }, { "Password", pw, sizeof(pw), true } };
    int foc = 1;

    screen_init();
    cw = ch = 0;
    card(400, 300);
    draw_clock(true);
    uif_draw_center(cx, cy + 24, cw, 30, UIF_BIG, host, C_INK);
    msg(cy + 56, "SamaraOS", C_INK_DIM);
    for (;;) {
        for (int i = 0; i < 2; i++) draw_field(cx + 40, cy + 96 + i * 66, cw - 80, &f[i], foc == i);
        msg(cy + 256, "Enter: log in    Tab: next field", C_INK_DIM);
        char k = wait_key();
        if (k == '\t' || k == (char)K_UP || k == (char)K_DOWN) { foc ^= 1; continue; }
        if (k == '\n') {
            if (foc == 0) { foc = 1; continue; }
            msg(cy + 228, "checking...", C_INK_DIM);
            if (check_pw(user, pw)) break;
            msg(cy + 228, "wrong user or password", C_DANGER);
            pw[0] = 0;
            continue;
        }
        if (edit(&f[foc], k) && foc == 1) msg(cy + 228, "", C_INK);
    }
    strncpy(login_user, user, sizeof(login_user) - 1);
    char home[48] = "/home/";
    strcat(home, user);
    if (strcmp(user, "root") && !fs_resolve(fs_root(), home)) fs_create(fs_root(), home, FS_DIR);
    // kprintf("login %s\n", user);
}

// called by the wm (super+l / idle). draws a dimmed copy of the desktop, blocks till the password is right
void lock_screen(void) {
    char pw[64] = "";
    fld_t f = { "Password", pw, sizeof(pw), true };
    W = gfx_w();
    H = gfx_h();
    uint32_t* shot = kmalloc_big((size_t)W * H * 4);
    gfx_reset_clip();
    if (shot) {
        gfx_save_rect(0, 0, W, H, shot);
        for (int i = 0; i < W * H; i++) shot[i] = (shot[i] >> 2) & 0x3F3F3F;
    }
    bg = shot;
    bg_rect(0, 0, W, H);
    cw = ch = 0;
    card(360, 170);
    last_min = -1;
    uint32_t t = clock_epoch();
    char clk[8];
    for (;;) {
        int m = (int)(t / 60 % 1440);
        clk[0] = '0' + m / 600; clk[1] = '0' + m / 60 % 10; clk[2] = ':';
        clk[3] = '0' + m % 60 / 10; clk[4] = '0' + m % 10; clk[5] = 0;
        bg_rect(W / 2 - 200, cy - 150, 400, 110);
        uif_draw_center(W / 2 - 200, cy - 150, 400, 110, UIF_HUGE, clk, C_BAR_TEXT);
        draw_field(cx + 30, cy + 24, cw - 60, &f, true);
        gfx_present();
        char k;
        for (;;) {
            if (kbd_has_key()) { k = kbd_trygetc(); break; }
            uint32_t n = clock_epoch();
            if (n / 60 != t / 60) { t = n; k = 0; break; }
            task_yield();
        }
        if (!k) continue;
        if (k == '\n') {
            msg(cy + 110, "checking...", C_INK_DIM);
            gfx_present();
            if (check_pw(login_user[0] ? login_user : "root", pw)) break;
            msg(cy + 110, "wrong password", C_DANGER);
            pw[0] = 0;
            continue;
        }
        if (edit(&f, k)) msg(cy + 110, "", C_INK);
    }
    bg = NULL;
    if (shot) kfree(shot);
}

/* ---------------- installer ---------------- */

enum { P_HELLO, P_DISK, P_USER, P_SURE, P_WORK, P_DONE, P_FAIL };

static int disks[DISK_MAX], ndisks;

void install_progress(const char* s, int pct) {
    int bx = cx + 40, by = cy + 200, bw = cw - 80;
    gfx_rrect_fill(bx, by, bw, 14, 7, GFX_CORNERS_ALL, C_WELL);
    if (pct > 0) gfx_rrect_fill(bx, by, 14 + (bw - 14) * pct / 100, 14, 7, GFX_CORNERS_ALL, C_ACCENT);
    msg(by + 30, s, C_INK_DIM);
}

static void size_str(int d, char* out) {
    uint32_t mb = ata_drive_sectors(d) / 2048;
    char tmp[16];
    strcpy(out, ata_drive_name(d));
    strcat(out, "   ");
    if (mb >= 10240) { itoa((int)(mb / 1024), tmp, 10); strcat(out, tmp); strcat(out, " GB"); }
    else { itoa((int)mb, tmp, 10); strcat(out, tmp); strcat(out, " MB"); }
}

static bool name_ok(const char* s) {
    if (!*s || *s == '-') return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return false;
    return true;
}

bool install_screen(void) {
    static char host[32] = "samara", user[32] = "", pw[64] = "", pw2[64] = "";
    fld_t f[4] = {
        { "Computer name", host, sizeof(host), false },
        { "User name", user, sizeof(user), false },
        { "Password", pw, sizeof(pw), true },
        { "Password again", pw2, sizeof(pw2), true },
    };
    int page = P_HELLO, sel = 0, foc = 1;
    bool skip = false;
    const char* err = NULL;

    ndisks = 0;
    for (int i = 0; i < DISK_MAX; i++) if (ata_drive_present(i)) disks[ndisks++] = i;

    screen_init();
    cw = ch = 0;
    draw_clock(true);
    int tx = 0, tw = 0;
    for (;;) {
        if (skip) goto key;     /* only a field changed, dont blink the whole card */
        card(560, 440);
        tx = cx + 40; tw = cw - 80;
        switch (page) {
        case P_HELLO:
            uif_draw(tx, cy + 32, UIF_BIG, "Install SamaraOS", C_INK);
            uif_draw_wrap(tx, cy + 80, tw, 22, UIF_REG,
                          "This puts SamaraOS on a hard disk together with the GRUB "
                          "boot loader, so it starts without the CD.\n\n"
                          "The disk gets formatted (FAT32), everything on it is lost.", C_INK);
            msg(cy + 400, "Enter: start    Esc: just try it", C_INK_DIM);
            break;
        case P_DISK:
            uif_draw(tx, cy + 32, UIF_BIG, "Pick a disk", C_INK);
            if (!ndisks) uif_draw(tx, cy + 90, UIF_REG, "No disks found. Attach one and reboot.", C_DANGER);
            for (int i = 0; i < ndisks; i++) {
                char s[48];
                size_str(disks[i], s);
                bool small = ata_drive_sectors(disks[i]) < 64 * 2048;
                if (small) strcat(s, "  (too small)");
                int y = cy + 84 + i * 40;
                gfx_rrect_fill(tx, y, tw, 34, 6, GFX_CORNERS_ALL, i == sel ? C_BTN_HOVER : C_SURFACE);
                uif_draw_mid(tx + 14, y + 17, i == sel ? UIF_MED : UIF_REG, s, small ? C_INK_DIM : C_INK);
            }
            msg(cy + 400, "Up/Down, Enter: next    Esc: back", C_INK_DIM);
            break;
        case P_USER:
            uif_draw(tx, cy + 32, UIF_BIG, "Your account", C_INK);
            for (int i = 0; i < 4; i++) draw_field(tx, cy + 76 + i * 66, tw, &f[i], foc == i);
            if (err) msg(cy + 346, err, C_DANGER);
            msg(cy + 400, "Tab/Up/Down: field    Enter: next    Esc: back", C_INK_DIM);
            break;
        case P_SURE: {
            char s[96] = "Everything on ", d[48];
            size_str(disks[sel], d);
            strcat(s, d);
            strcat(s, " will be erased.");
            uif_draw(tx, cy + 32, UIF_BIG, "Sure?", C_INK);
            uif_draw_wrap(tx, cy + 84, tw, 22, UIF_MED, s, C_DANGER);
            uif_draw_wrap(tx, cy + 130, tw, 22, UIF_REG,
                          "Then reboot without the CD and log in with your password.", C_INK);
            msg(cy + 400, "Enter: install    Esc: back", C_INK_DIM);
            break;
        }
        case P_WORK:
            uif_draw(tx, cy + 32, UIF_BIG, "Installing...", C_INK);
            install_progress("starting", 0);
            err = install_run(disks[sel], host, user, pw);
            page = err ? P_FAIL : P_DONE;
            continue;
        case P_DONE:
            uif_draw(tx, cy + 32, UIF_BIG, "Done", C_INK);
            install_progress("SamaraOS is on the disk", 100);
            msg(cy + 400, "Take the CD out, Enter: reboot    Esc: keep going", C_INK_DIM);
            break;
        case P_FAIL:
            uif_draw(tx, cy + 32, UIF_BIG, "Install failed", C_DANGER);
            msg(cy + 120, err, C_INK);
            msg(cy + 400, "Esc: back", C_INK_DIM);
            break;
        }

key:
        skip = false;
        char k = wait_key();
        if (k == 0x1B) {
            if (page == P_HELLO || page == P_DONE) return false;
            page = page == P_FAIL ? P_SURE : page - 1;
            err = NULL;
            continue;
        }
        switch (page) {
        case P_HELLO:
            if (k == '\n') page = P_DISK;
            break;
        case P_DISK:
            if (k == (char)K_UP && sel > 0) sel--;
            if (k == (char)K_DOWN && sel < ndisks - 1) sel++;
            if (k == '\n' && ndisks && ata_drive_sectors(disks[sel]) >= 64 * 2048) page = P_USER;
            break;
        case P_USER:
            if (k == '\t' || k == (char)K_DOWN) { foc = (foc + 1) % 4; break; }
            if (k == (char)K_UP) { foc = (foc + 3) % 4; break; }
            if (k != '\n') {
                if (edit(&f[foc], k)) draw_field(tx, cy + 76 + foc * 66, tw, &f[foc], true);
                skip = true;
                break;
            }
            if (foc < 3) { foc++; break; }
            err = !name_ok(host) ? "computer name: a-z, 0-9, - and _ only"
                : !name_ok(user) ? "user name: a-z, 0-9, - and _ only"
                : !strcmp(user, "root") ? "pick something else than root"
                : !pw[0] ? "empty password, nope"
                : strcmp(pw, pw2) ? "passwords dont match" : NULL;
            if (err) { pw2[0] = 0; foc = 3; break; }
            page = P_SURE;
            break;
        case P_SURE:
            if (k == '\n') page = P_WORK;
            break;
        case P_DONE:
            if (k == '\n') {
                while (inb(0x64) & 0x02) {}
                outb(0x64, 0xFE);
            }
            break;
        }
    }
}
