/* fm: file manager for the samara desktop. plain libc, all the drawing and
   input goes through fm_be.h */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include "fm_be.h"

uint32_t *img_decode(const uint8_t *d, int n, int *w, int *h);
int img_peek(const uint8_t *d, int n, int *w, int *h);

#define BG      0x17181B
#define BG2     0x1E1F23
#define BAR     0x131417
#define WELL    0x101114
#define INK     0xE6E6E6
#define DIM     0x85868C
#define FAINT   0x55565C
#define RULE    0x2C2D32
#define HOVER   0x25262A
#define ACC     0xE39B32
#define SELBG   0x3A2E1A
#define DANGER  0xC43B30

#define TB_H   44
#define SB_W   160
#define ST_H   26
#define HDR_H  26
#define ROW_H  28
#define CELL_W 104
#define CELL_H 96
#define SCR_W  10

enum { K_DIR, K_TEXT, K_IMG, K_AUDIO, K_EXE, K_ARC, K_FILE };
enum { S_NAME, S_SIZE, S_DATE };
enum { M_BROWSE, M_TEXT, M_IMG, M_HEX };
enum { MD_NONE, MD_CONFIRM, MD_INPUT, MD_PROPS, MD_MSG, MD_PROG };
enum { IA_RENAME = 1, IA_NEWDIR, IA_NEWFILE };
enum { A_OPEN = 1, A_CUT, A_COPY, A_PASTE, A_RENAME, A_DELETE, A_NEWDIR, A_NEWFILE, A_PROPS,
       A_SNAME, A_SSIZE, A_SDATE, A_VIEW, A_HIDDEN, A_REFRESH };

typedef struct {
    char name[256];
    long long size;
    time_t mtime;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    int kind, sel, link;
} Ent;

typedef struct { char buf[512]; int len, pos, all; } Edit;
typedef struct { const char *s; int id; int on; } MI;

static Ent *ents;
static int n_ents, cap_ents;
static char cwd[1024];
static int sort_key = S_NAME, sort_rev, show_hid, grid;
static int cur = -1, anchor = -1, scroll;
static int hover = -1, mode = M_BROWSE;
static int dirty = 1;
static char hist[32][1024];
static int hn, hp;
static char home[256] = "/root";
static char msg[200];
static uint32_t msg_until;
static time_t dsig_m;
static int dsig_n;
static uint32_t last_scan;
static int fh_reg, fh_small, fh_mono;
static int mx, my;

static Edit addr, ed;
static int addr_on;

static int modal, modal_act;
static char modal_title[64], modal_text[400];
static char props[10][200];
static int n_props;
static int bx_ok[4], bx_cancel[4];

static MI menu[20];
static int n_menu, menu_x, menu_y, menu_hot = -1, menu_on;

static int click_idx = -1;
static uint32_t click_ms;
static int rb_on, rb_x0, rb_y0, rb_x1, rb_y1;
static int sb_drag;

static int cancel, quit;
static long long tot_bytes, done_bytes;
static char prog_name[256];
static uint32_t last_pump;

/* text and hex view read the file through a 256k window, nothing is loaded whole */
#define WIN (256 << 10)
static int v_fd = -1;
static long long v_size, tv_top;
static uint8_t *w_buf;
static long long w_off;
static int w_len;
static uint32_t *iv_px;
static int iv_w, iv_h, iv_ow, iv_oh;
static char v_name[256], v_path[1024];

/* line numbers: thread counts newlines, tab[k] = lines before byte k*64k */
typedef struct { long long *tab; long long lines; volatile int n, done, stop, rel; char path[1024]; long long size; } Idx;
static Idx *idx;

/* background image load: read, decode, shrink */
typedef struct {
    char path[1024], name[256], err[80];
    volatile int stop, state, phase, rel;
    volatile long long done, total;
    uint32_t *px;
    int w, h, ow, oh, ent;
} Job;
static Job *job;
static pthread_mutex_t dec_mx = PTHREAD_MUTEX_INITIALIZER;

static struct { char ext[16]; char cmd[256]; } assoc[64];
static int n_assoc;

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000u + ts.tv_nsec / 1000000;
}

static void say(const char *s) {
    strncpy(msg, s, sizeof msg - 1);
    msg_until = now_ms() + 4000;
    dirty = 1;
}

static void sayf(const char *a, const char *b) {
    snprintf(msg, sizeof msg, "%s: %s", a, b);
    msg_until = now_ms() + 4000;
    dirty = 1;
}

static void fmt_size(long long n, char *o) {
    if (n < 1024) sprintf(o, "%lld B", n);
    else if (n < 1024 * 1024) sprintf(o, "%lld.%lld KB", n >> 10, (n & 1023) * 10 >> 10);
    else if (n < 1024LL * 1024 * 1024) sprintf(o, "%lld.%lld MB", n >> 20, ((n >> 10) & 1023) * 10 >> 10);
    else sprintf(o, "%lld.%lld GB", n >> 30, ((n >> 20) & 1023) * 10 >> 10);
}

static void fmt_date(time_t t, char *o) {
    struct tm tm;
    localtime_r(&t, &tm);
    sprintf(o, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void fmt_mode(mode_t m, char *o) {
    static const char rwx[] = "rwxrwxrwx";
    int i;
    o[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
    for (i = 0; i < 9; i++) o[i + 1] = (m & (0400 >> i)) ? rwx[i] : '-';
    o[10] = 0;
}

static const char *ext_of(const char *n) {
    const char *d = strrchr(n, '.');
    return (d && d != n) ? d + 1 : "";
}

static int in_list(const char *e, const char *list) {
    int n = strlen(e);
    const char *p = list;
    if (!n) return 0;
    while (*p) {
        const char *q = strchr(p, ' ');
        int l = q ? q - p : (int)strlen(p);
        if (l == n && !strncasecmp(p, e, n)) return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

static int kind_of(const char *name, mode_t m) {
    const char *e = ext_of(name);
    if (S_ISDIR(m)) return K_DIR;
    if (in_list(e, "png jpg jpeg gif bmp ppm pgm pnm")) return K_IMG;
    if (in_list(e, "wav mp3 ogg mid flac")) return K_AUDIO;
    if (in_list(e, "tar gz tgz bz2 xz zip apk 7z zst")) return K_ARC;
    if (m & 0111) return K_EXE;
    if (in_list(e, "txt md c h cpp py sh conf cfg log ini json xml html css js csv lua mk rs go")) return K_TEXT;
    if (!strcmp(name, "Makefile") || !strcmp(name, "README") || !strcmp(name, "LICENSE")) return K_TEXT;
    return K_FILE;
}

static void join(char *out, const char *dir, const char *name) {
    if (!strcmp(dir, "/")) snprintf(out, 1024, "/%s", name);
    else snprintf(out, 1024, "%s/%s", dir, name);
}

static void cp_utf8(int c, char *o, int *n) {
    if (c < 0x80) o[(*n)++] = c;
    else if (c < 0x800) { o[(*n)++] = 0xC0 | (c >> 6); o[(*n)++] = 0x80 | (c & 63); }
    else { o[(*n)++] = 0xE0 | (c >> 12); o[(*n)++] = 0x80 | ((c >> 6) & 63); o[(*n)++] = 0x80 | (c & 63); }
}

/* cut s so it fits w pixels, "..." at the end */
static void fit(const char *s, int w, int font, char *out, int cap) {
    int n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    if (be_text_w(out, font) <= w) return;
    while (n > 0) {
        n--;
        while (n > 0 && (out[n] & 0xC0) == 0x80) n--;
        memcpy(out + n, "...", 4);
        if (be_text_w(out, font) <= w) return;
        out[n] = 0;
    }
}

static void frame(int x, int y, int w, int h, uint32_t c) {
    be_fill(x, y, w, 1, c);
    be_fill(x, y + h - 1, w, 1, c);
    be_fill(x, y, 1, h, c);
    be_fill(x + w - 1, y, 1, h, c);
}

static void disc(int cx, int cy, int r, uint32_t c) {
    int y, x;
    for (y = -r; y <= r; y++) {
        x = 0;
        while ((x + 1) * (x + 1) + y * y <= r * r) x++;
        be_fill(cx - x, cy + y, 2 * x + 1, 1, c);
    }
}

#define P(v) ((v) * sz / 32 ? (v) * sz / 32 : 1)

static void ico(int kind, int x, int y, int sz) {
    int i, r;
    switch (kind) {
    case K_DIR:
        be_fill(x + P(2), y + P(6), P(12), P(5), 0xB87A22);
        be_fill(x + P(2), y + P(9), P(28), P(19), 0xCF8B28);
        be_fill(x + P(2), y + P(13), P(28), P(15), ACC);
        break;
    case K_TEXT:
    case K_FILE:
        be_fill(x + P(6), y + P(3), P(20), P(26), 0xD8D8DC);
        be_fill(x + P(20), y + P(3), P(6), P(6), 0x8E8F95);
        if (kind == K_TEXT)
            for (i = 0; i < 4; i++) be_fill(x + P(10), y + P(12 + i * 4), P(12), P(2), 0x6A6B70);
        break;
    case K_IMG:
        be_fill(x + P(3), y + P(5), P(26), P(22), 0x3E7CCF);
        be_fill(x + P(21), y + P(8), P(5), P(5), 0xF2C94C);
        for (r = 0; r < P(9); r++) be_fill(x + P(12) - r, y + P(18) + r, 2 * r + 1, 1, 0x5FA05E);
        break;
    case K_AUDIO:
        disc(x + P(11), y + P(23), P(5), INK);
        be_fill(x + P(15), y + P(7), P(2), P(16), INK);
        be_fill(x + P(15), y + P(7), P(10), P(4), INK);
        break;
    case K_EXE:
        be_fill(x + P(3), y + P(5), P(26), P(22), 0x2A2B30);
        frame(x + P(3), y + P(5), P(26), P(22), 0x5FB06A);
        for (i = 0; i < P(9); i++) be_fill(x + P(12) + i, y + P(16) - (P(9) - i) / 2 - 1, 1, P(9) - i + 1, 0x5FB06A);
        break;
    case K_ARC:
        be_fill(x + P(5), y + P(8), P(22), P(20), 0xB08A5A);
        be_fill(x + P(4), y + P(5), P(24), P(6), 0xC9A06A);
        for (i = 0; i < 5; i++) be_fill(x + P(15) + (i & 1) * P(2), y + P(11) + i * P(3), P(2), P(2), 0x4A3A22);
        break;
    }
}

static void glyph_nav(int id, int x, int y, uint32_t c) {
    int i;
    for (i = 0; i < 2; i++) {
        switch (id) {
        case 0: be_line(x + 17 + i, y + 7, x + 9 + i, y + 14, c); be_line(x + 9 + i, y + 14, x + 17 + i, y + 21, c); break;
        case 1: be_line(x + 11 + i, y + 7, x + 19 + i, y + 14, c); be_line(x + 19 + i, y + 14, x + 11 + i, y + 21, c); break;
        case 2: be_line(x + 7, y + 15 + i, x + 14, y + 8 + i, c); be_line(x + 14, y + 8 + i, x + 21, y + 15 + i, c); break;
        }
    }
    if (id == 2) be_fill(x + 13, y + 10, 2, 11, c);
    if (id == 3) {
        be_line(x + 6, y + 14, x + 14, y + 7, c); be_line(x + 14, y + 7, x + 22, y + 14, c);
        be_fill(x + 9, y + 14, 11, 8, c); be_fill(x + 10, y + 15, 9, 7, BG); be_fill(x + 12, y + 17, 4, 5, c);
    }
}

static int cmp_ent(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    int r = 0;
    if ((x->kind == K_DIR) != (y->kind == K_DIR)) return x->kind == K_DIR ? -1 : 1;
    if (sort_key == S_SIZE) r = x->size < y->size ? -1 : x->size > y->size;
    else if (sort_key == S_DATE) r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
    if (r) return sort_rev ? -r : r;
    r = strcasecmp(x->name, y->name);
    if (!r) r = strcmp(x->name, y->name);
    return (sort_rev && sort_key == S_NAME) ? -r : r;
}

static void dir_sig(void) {
    struct stat st;
    DIR *d;
    int n = 0;
    dsig_m = 0;
    if (!stat(cwd, &st)) dsig_m = st.st_mtime;
    d = opendir(cwd);
    if (d) { while (readdir(d)) n++; closedir(d); }
    dsig_n = n;
}

static void load_dir(void) {
    DIR *d = opendir(cwd);
    struct dirent *de;
    Ent *old = ents;
    int nold = n_ents, i, j;
    char curname[256] = "";
    if (cur >= 0 && cur < nold) strcpy(curname, old[cur].name);
    ents = 0; n_ents = cap_ents = 0;
    if (!d) { sayf(cwd, strerror(errno)); free(old); return; }
    while ((de = readdir(d))) {
        Ent *e;
        struct stat st, ls;
        char p[1024];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.' && !show_hid) continue;
        join(p, cwd, de->d_name);
        if (lstat(p, &ls) < 0) continue;
        st = ls;
        if (S_ISLNK(ls.st_mode) && stat(p, &st) < 0) st = ls;
        if (n_ents == cap_ents) {
            cap_ents = cap_ents ? cap_ents * 2 : 64;
            ents = realloc(ents, cap_ents * sizeof(Ent));
        }
        e = &ents[n_ents++];
        memset(e, 0, sizeof *e);
        strncpy(e->name, de->d_name, 255);
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->mode = st.st_mode;
        e->uid = ls.st_uid;
        e->gid = ls.st_gid;
        e->link = S_ISLNK(ls.st_mode);
        e->kind = kind_of(e->name, st.st_mode);
    }
    closedir(d);
    qsort(ents, n_ents, sizeof(Ent), cmp_ent);
    cur = -1;
    for (i = 0; i < n_ents; i++) {
        for (j = 0; j < nold; j++)
            if (!strcmp(old[j].name, ents[i].name)) { ents[i].sel = old[j].sel; break; }
        if (!strcmp(ents[i].name, curname)) cur = i;
    }
    free(old);
    if (anchor >= n_ents) anchor = -1;
    dir_sig();
    last_scan = now_ms();
    hover = -1;
    dirty = 1;
}

static void hist_add(void) {
    if (hn && !strcmp(hist[hp], cwd)) return;
    if (hp < hn - 1) hn = hp + 1;
    if (hn == 32) { memmove(hist[0], hist[1], 31 * 1024); hn--; }
    strcpy(hist[hn], cwd);
    hp = hn++;
}

static void enter_dir(const char *p, int add) {
    char rp[1024], t[1100];
    if (!realpath(p, rp)) { sayf(p, strerror(errno)); return; }
    if (access(rp, R_OK | X_OK) < 0) { sayf(rp, "permission denied"); return; }
    strcpy(cwd, rp);
    if (add) hist_add();
    free(ents);
    ents = 0; n_ents = cap_ents = 0;
    cur = anchor = -1;
    scroll = 0;
    load_dir();
    snprintf(t, sizeof t, "Files - %s", cwd);
    be_title(t);
}

static void go_up(void) {
    char p[1024], *s;
    strcpy(p, cwd);
    s = strrchr(p, '/');
    if (!s || s == p) { if (strcmp(cwd, "/")) enter_dir("/", 1); return; }
    *s = 0;
    enter_dir(p, 1);
}

static void go_hist(int d) {
    int n = hp + d;
    if (n < 0 || n >= hn) return;
    hp = n;
    enter_dir(hist[hp], 0);
}

static int cont_x(void) { return SB_W + 1; }
static int cont_y(void) { return TB_H + (grid ? 0 : HDR_H); }
static int cont_w(void) { return be_w() - SB_W - 1; }
static int cont_h(void) { return be_h() - ST_H - cont_y(); }
static int cols(void) { int c = (cont_w() - SCR_W) / CELL_W; return c < 1 ? 1 : c; }

static int total_h(void) {
    if (!grid) return n_ents * ROW_H;
    return (n_ents + cols() - 1) / cols() * CELL_H + 8;
}

/* item rect in content coords (before scroll) */
static void irect(int i, int *x, int *y, int *w, int *h) {
    if (!grid) { *x = 0; *y = i * ROW_H; *w = cont_w() - SCR_W; *h = ROW_H; return; }
    *x = 4 + (i % cols()) * CELL_W;
    *y = 4 + (i / cols()) * CELL_H;
    *w = CELL_W - 4;
    *h = CELL_H - 4;
}

static void clamp_scroll(void) {
    int m = total_h() - cont_h();
    if (scroll > m) scroll = m;
    if (scroll < 0) scroll = 0;
}

static void show_cur(void) {
    int x, y, w, h;
    if (cur < 0) return;
    irect(cur, &x, &y, &w, &h);
    if (y < scroll) scroll = y;
    if (y + h > scroll + cont_h()) scroll = y + h - cont_h();
    clamp_scroll();
}

static int hit_ent(int px, int py) {
    int x = px - cont_x(), y = py - cont_y() + scroll, i;
    if (px < cont_x() || py < cont_y() || py >= be_h() - ST_H) return -1;
    if (x >= cont_w() - SCR_W) return -1;
    if (!grid) i = y / ROW_H;
    else {
        int c = (x - 4) / CELL_W, r = (y - 4) / CELL_H;
        if (x < 4 || y < 4 || c >= cols()) return -1;
        i = r * cols() + c;
    }
    return (i >= 0 && i < n_ents) ? i : -1;
}

static void sel_none(void) {
    int i;
    for (i = 0; i < n_ents; i++) ents[i].sel = 0;
}

static int n_sel(void) {
    int i, n = 0;
    for (i = 0; i < n_ents; i++) n += ents[i].sel;
    return n;
}

/* ---------- file ops ---------- */

static void pump(int draw_now);
static void draw(void);

static int exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

static void unique(char *p) {
    char base[1024];
    int i;
    if (!exists(p)) return;
    strcpy(base, p);
    for (i = 1; i < 1000; i++) {
        if (i == 1) snprintf(p, 1024, "%s (copy)", base);
        else snprintf(p, 1024, "%s (copy %d)", base, i);
        if (!exists(p)) return;
    }
}

static long long du(const char *p) {
    struct stat st;
    long long t = 0;
    DIR *d;
    struct dirent *de;
    static int cnt;
    if (lstat(p, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) return st.st_size;
    if (!(++cnt & 31)) pump(0);
    d = opendir(p);
    if (!d) return 0;
    while (!cancel && (de = readdir(d))) {
        char q[1024];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        join(q, p, de->d_name);
        t += du(q);
    }
    closedir(d);
    return t;
}

static int copy_file(const char *s, const char *d, mode_t m) {
    int a = open(s, O_RDONLY), b, n, ret = 0;
    static char buf[65536];
    if (a < 0) return -1;
    b = open(d, O_WRONLY | O_CREAT | O_TRUNC, m & 07777);
    if (b < 0) { close(a); return -1; }
    while ((n = read(a, buf, sizeof buf)) > 0) {
        char *p = buf;
        while (n > 0) {
            int w = write(b, p, n);
            if (w <= 0) { ret = -1; goto out; }
            p += w; n -= w; done_bytes += w;
        }
        pump(0);
        if (cancel) { ret = -1; break; }
    }
out:
    close(a);
    close(b);
    if (ret < 0) unlink(d);
    return ret;
}

static int copy_tree(const char *s, const char *d) {
    struct stat st;
    if (lstat(s, &st) < 0) return -1;
    strncpy(prog_name, strrchr(s, '/') ? strrchr(s, '/') + 1 : s, 255);
    if (S_ISLNK(st.st_mode)) {
        char t[1024];
        int n = readlink(s, t, sizeof t - 1);
        if (n < 0) return -1;
        t[n] = 0;
        return symlink(t, d);
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dd;
        struct dirent *de;
        int ret = 0;
        if (mkdir(d, st.st_mode & 07777) < 0 && errno != EEXIST) return -1;
        dd = opendir(s);
        if (!dd) return -1;
        while (!cancel && (de = readdir(dd))) {
            char a[1024], b[1024];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            join(a, s, de->d_name);
            join(b, d, de->d_name);
            if (copy_tree(a, b) < 0) ret = -1;
        }
        closedir(dd);
        return ret;
    }
    return copy_file(s, d, st.st_mode);
}

static int rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *de;
        int ret = 0;
        if (!d) return -1;
        while (!cancel && (de = readdir(d))) {
            char q[1024];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            join(q, p, de->d_name);
            if (rm_tree(q) < 0) ret = -1;
        }
        closedir(d);
        return ret < 0 ? ret : rmdir(p);
    }
    strncpy(prog_name, strrchr(p, '/') ? strrchr(p, '/') + 1 : p, 255);
    done_bytes += st.st_size + 1;
    pump(0);
    return unlink(p);
}

static void prog_start(const char *title) {
    modal = MD_PROG;
    strncpy(modal_title, title, 63);
    cancel = 0;
    done_bytes = tot_bytes = 0;
    prog_name[0] = 0;
    last_pump = 0;
}

static void prog_end(void) {
    modal = MD_NONE;
    dirty = 1;
}

/* called from the long loops: keep the window alive, redraw the bar */
static void pump(int draw_now) {
    FmEv e;
    uint32_t t = now_ms();
    while (be_wait(&e, 0) > 0) {
        if (e.type == EV_CLOSE) { cancel = 1; quit = 1; }
        if (e.type == EV_KEY && e.a == FK_ESC) cancel = 1;
        if (e.type == EV_DOWN && modal == MD_PROG && e.a >= bx_cancel[0] && e.a < bx_cancel[0] + bx_cancel[2] &&
            e.b >= bx_cancel[1] && e.b < bx_cancel[1] + bx_cancel[3]) cancel = 1;
    }
    if (modal == MD_PROG && (draw_now || t - last_pump > 80)) {
        last_pump = t;
        draw();
        be_flip();
    }
}

static void clip_write(const char *how) {
    FILE *f = fopen("/tmp/.fm-clip", "w");
    int i;
    char p[1024];
    if (!f) { say("cant write clipboard"); return; }
    fprintf(f, "%s\n", how);
    for (i = 0; i < n_ents; i++)
        if (ents[i].sel) { join(p, cwd, ents[i].name); fprintf(f, "%s\n", p); }
    fclose(f);
    sprintf(p, "%d items %s", n_sel(), how[1] == 'o' ? "copied" : "cut");
    say(p);
}

static int clip_has(void) { return access("/tmp/.fm-clip", R_OK) == 0; }

static void paste(void) {
    FILE *f = fopen("/tmp/.fm-clip", "r");
    char line[1100], how[16], *srcs[256];
    int n = 0, i, cut, fail = 0;
    if (!f) return;
    if (!fgets(how, sizeof how, f)) { fclose(f); return; }
    cut = how[1] == 'u';
    while (n < 256 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0]) srcs[n++] = strdup(line);
    }
    fclose(f);
    prog_start(cut ? "Moving" : "Copying");
    if (!cut) for (i = 0; i < n && !cancel; i++) tot_bytes += du(srcs[i]);
    for (i = 0; i < n && !cancel; i++) {
        char dst[1024], *b = strrchr(srcs[i], '/');
        int sl = strlen(srcs[i]);
        join(dst, cwd, b ? b + 1 : srcs[i]);
        if (!strncmp(dst, srcs[i], sl) && (dst[sl] == '/' || !dst[sl]) && strcmp(dst, srcs[i])) {
            say("cant copy a folder into itself");
            fail = 1;
            continue;
        }
        if (cut && !strcmp(dst, srcs[i])) continue;
        unique(dst);
        if (cut) {
            if (rename(srcs[i], dst) == 0) continue;
            if (copy_tree(srcs[i], dst) < 0 || rm_tree(srcs[i]) < 0) fail = 1;
        } else if (copy_tree(srcs[i], dst) < 0) fail = 1;
    }
    for (i = 0; i < n; i++) free(srcs[i]);
    if (cut && !fail && !cancel) unlink("/tmp/.fm-clip");
    prog_end();
    if (cancel) say("cancelled");
    else if (fail) say("some files failed");
    load_dir();
}

static void del_sel(void) {
    int i, fail = 0;
    char p[1024];
    prog_start("Deleting");
    for (i = 0; i < n_ents; i++)
        if (ents[i].sel) { join(p, cwd, ents[i].name); tot_bytes += du(p) + 1; }
    for (i = 0; i < n_ents && !cancel; i++) {
        if (!ents[i].sel) continue;
        join(p, cwd, ents[i].name);
        if (rm_tree(p) < 0) fail = 1;
    }
    prog_end();
    if (fail) say("couldnt delete everything");
    sel_none();
    cur = -1;
    load_dir();
}

/* ---------- text fields and dialogs ---------- */

static void edit_set(Edit *e, const char *s) {
    strncpy(e->buf, s, 500);
    e->buf[500] = 0;
    e->len = e->pos = strlen(e->buf);
    e->all = 1;
}

static void edit_ins(Edit *e, int c) {
    char t[4];
    int n = 0;
    cp_utf8(c, t, &n);
    if (e->all) { e->len = e->pos = 0; e->buf[0] = 0; e->all = 0; }
    if (e->len + n >= 500) return;
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos + 1);
    memcpy(e->buf + e->pos, t, n);
    e->pos += n; e->len += n;
}

static void edit_del(Edit *e, int fwd) {
    int a = e->pos, b;
    if (e->all) { e->len = e->pos = 0; e->buf[0] = 0; e->all = 0; return; }
    if (fwd) {
        if (a >= e->len) return;
        b = a + 1;
        while (b < e->len && (e->buf[b] & 0xC0) == 0x80) b++;
    } else {
        if (!a) return;
        b = a;
        a--;
        while (a > 0 && (e->buf[a] & 0xC0) == 0x80) a--;
    }
    memmove(e->buf + a, e->buf + b, e->len - b + 1);
    e->len -= b - a;
    e->pos = a;
}

/* 1 = enter, 2 = esc */
static int edit_key(Edit *e, FmEv *k) {
    int c = k->a;
    dirty = 1;
    switch (c) {
    case FK_ENTER: return 1;
    case FK_ESC: return 2;
    case FK_BACK: edit_del(e, 0); break;
    case FK_DEL: edit_del(e, 1); break;
    case FK_LEFT:
        e->all = 0;
        if (e->pos) { e->pos--; while (e->pos > 0 && (e->buf[e->pos] & 0xC0) == 0x80) e->pos--; }
        break;
    case FK_RIGHT:
        e->all = 0;
        if (e->pos < e->len) { e->pos++; while (e->pos < e->len && (e->buf[e->pos] & 0xC0) == 0x80) e->pos++; }
        break;
    case FK_HOME: e->all = 0; e->pos = 0; break;
    case FK_END: e->all = 0; e->pos = e->len; break;
    default:
        if (k->mods & MOD_CTRL) {
            if (c == 'a') { e->all = 1; e->pos = e->len; }
            break;
        }
        if (c >= 32 && c < 0x200000) edit_ins(e, c);
    }
    return 0;
}

static void ask_input(const char *title, const char *text, const char *def, int act) {
    modal = MD_INPUT;
    modal_act = act;
    strncpy(modal_title, title, 63);
    strncpy(modal_text, text, 399);
    edit_set(&ed, def);
    dirty = 1;
}

static void ask_msg(const char *title, const char *text) {
    modal = MD_MSG;
    strncpy(modal_title, title, 63);
    strncpy(modal_text, text, 399);
    dirty = 1;
}

static void new_name(const char *base, char *out) {
    char p[1024];
    int i;
    strcpy(out, base);
    for (i = 2; i < 1000; i++) {
        join(p, cwd, out);
        if (!exists(p)) return;
        sprintf(out, "%s %d", base, i);
    }
}

static void input_done(void) {
    char p[1024], q[1024];
    int act = modal_act, i;
    modal = MD_NONE;
    dirty = 1;
    if (!ed.buf[0] || strchr(ed.buf, '/')) { say("bad name"); return; }
    join(p, cwd, ed.buf);
    if (act == IA_RENAME) {
        if (cur < 0) return;
        join(q, cwd, ents[cur].name);
        if (!strcmp(q, p)) return;
        if (exists(p)) { say("already exists"); return; }
        if (rename(q, p) < 0) sayf("rename", strerror(errno));
    } else if (act == IA_NEWDIR) {
        if (mkdir(p, 0755) < 0) sayf("mkdir", strerror(errno));
    } else if (act == IA_NEWFILE) {
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) sayf("create", strerror(errno)); else close(fd);
    }
    sel_none();
    load_dir();
    for (i = 0; i < n_ents; i++)
        if (!strcmp(ents[i].name, ed.buf)) { ents[i].sel = 1; cur = anchor = i; show_cur(); }
}

static void props_line(const char *k, const char *v) {
    if (n_props < 10) snprintf(props[n_props++], 200, "%s\t%s", k, v);
}

static void show_props(void) {
    char b[300], t[64], u[64];
    int i, ns = n_sel();
    long long sz = 0;
    Ent *e = 0;
    n_props = 0;
    if (ns == 0 && cur >= 0) { ents[cur].sel = 1; ns = 1; }
    if (ns == 1) for (i = 0; i < n_ents; i++) if (ents[i].sel) e = &ents[i];
    strcpy(modal_title, "Properties");
    modal = MD_PROPS;
    cancel = 0;
    if (e) {
        char p[1024], mb[16];
        struct passwd *pw = getpwuid(e->uid);
        struct group *gr = getgrgid(e->gid);
        join(p, cwd, e->name);
        props_line("Name", e->name);
        props_line("Type", e->link ? "Link" : e->kind == K_DIR ? "Folder" : e->kind == K_IMG ? "Image" :
                   e->kind == K_TEXT ? "Text file" : e->kind == K_EXE ? "Program" : e->kind == K_AUDIO ? "Audio" :
                   e->kind == K_ARC ? "Archive" : "File");
        props_line("Location", cwd);
        if (e->link) {
            int n = readlink(p, b, sizeof b - 1);
            if (n > 0) { b[n] = 0; props_line("Points to", b); }
        }
        sz = e->kind == K_DIR ? du(p) : e->size;
        fmt_size(sz, t);
        sprintf(b, "%s (%lld bytes)", t, sz);
        props_line("Size", b);
        fmt_mode(e->mode, mb);
        sprintf(b, "%s (%04o)", mb, e->mode & 07777);
        props_line("Permissions", b);
        if (pw) snprintf(u, 64, "%s (%d)", pw->pw_name, e->uid); else sprintf(u, "%d", e->uid);
        if (gr) snprintf(b, 200, "%s / %s (%d)", u, gr->gr_name, e->gid); else snprintf(b, 200, "%s / %d", u, e->gid);
        props_line("Owner", b);
        fmt_date(e->mtime, t);
        props_line("Modified", t);
    } else {
        char p[1024];
        struct stat st;
        if (ns > 1) {
            sprintf(b, "%d items", ns);
            props_line("Name", b);
            for (i = 0; i < n_ents; i++)
                if (ents[i].sel) { join(p, cwd, ents[i].name); sz += ents[i].kind == K_DIR ? du(p) : ents[i].size; }
        } else {
            props_line("Name", cwd);
            sprintf(b, "%d items", n_ents);
            props_line("Contains", b);
            sz = du(cwd);
            if (!stat(cwd, &st)) { fmt_date(st.st_mtime, t); props_line("Modified", t); }
        }
        fmt_size(sz, t);
        sprintf(b, "%s (%lld bytes)", t, sz);
        props_line("Size", b);
    }
    dirty = 1;
}

/* ---------- opening things ---------- */

static void conf_load(void) {
    FILE *f = fopen("/etc/samara-fm.conf", "r");
    char l[400];
    if (!f) return;
    while (fgets(l, sizeof l, f) && n_assoc < 64) {
        char *eq, *k, *v;
        l[strcspn(l, "\r\n")] = 0;
        k = l;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || !*k) continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        while (*v == ' ') v++;
        while (eq > k && (eq[-1] == ' ' || eq[-1] == '\t')) *--eq = 0;
        strncpy(assoc[n_assoc].ext, k, 15);
        strncpy(assoc[n_assoc].cmd, v, 255);
        n_assoc++;
    }
    fclose(f);
}

static void spawn(const char *cmd) {
    pid_t p = fork();
    if (p < 0) { say("fork failed"); return; }
    if (!p) {
        int fd = open("/dev/null", O_RDWR);
        setsid();
        if (chdir(cwd)) {}
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
}

static void quote(const char *s, char *o) {
    *o++ = '\'';
    for (; *s; s++) {
        if (*s == '\'') { memcpy(o, "'\\''", 4); o += 4; }
        else *o++ = *s;
    }
    *o++ = '\'';
    *o = 0;
}

static void idx_rel(Idx *x) { if (x && __sync_add_and_fetch(&x->rel, 1) == 2) { free(x->tab); free(x); } }
static void job_rel(Job *j) { if (j && __sync_add_and_fetch(&j->rel, 1) == 2) { free(j->px); free(j); } }

static void job_cancel(void) {
    if (!job) return;
    job->stop = 1;
    job_rel(job);
    job = 0;
}

static void free_viewers(void) {
    if (idx) { idx->stop = 1; idx_rel(idx); idx = 0; }
    job_cancel();
    free(iv_px);
    iv_px = 0;
    if (v_fd >= 0) close(v_fd);
    v_fd = -1;
    free(w_buf);
    w_buf = 0;
    w_len = 0;
    v_size = 0;
}

static uint32_t *load_pnm_bmp(const uint8_t *d, int n, int *w, int *h) {
    uint32_t *px;
    int i, x, y;
    if (n > 10 && d[0] == 'P' && (d[1] == '6' || d[1] == '5')) {
        int pos = 2, v[3], k = 0;
        while (k < 3 && pos < n) {
            while (pos < n && (d[pos] == ' ' || d[pos] == '\n' || d[pos] == '\r' || d[pos] == '\t')) pos++;
            if (pos < n && d[pos] == '#') { while (pos < n && d[pos] != '\n') pos++; continue; }
            v[k] = 0;
            while (pos < n && d[pos] >= '0' && d[pos] <= '9') v[k] = v[k] * 10 + d[pos++] - '0';
            k++;
        }
        pos++;
        *w = v[0]; *h = v[1];
        if (k < 3 || *w <= 0 || *h <= 0 || *w > 8192 || *h > 8192 || v[2] > 255) return 0;
        i = d[1] == '6' ? 3 : 1;
        if (pos + (long)*w * *h * i > n) return 0;
        px = malloc((size_t)*w * *h * 4);
        if (!px) return 0;
        for (y = 0; y < *h * *w; y++) {
            const uint8_t *s = d + pos + y * i;
            px[y] = i == 3 ? (s[0] << 16 | s[1] << 8 | s[2]) : (s[0] * 0x010101);
        }
        return px;
    }
    if (n > 54 && d[0] == 'B' && d[1] == 'M') {
        int off = d[10] | d[11] << 8 | d[12] << 16, bpp = d[28], comp = d[30];
        int bw = d[18] | d[19] << 8 | d[20] << 16, bh = (int)(d[22] | d[23] << 8 | d[24] << 16 | d[25] << 24);
        int flip = bh > 0, stride;
        if (bh < 0) bh = -bh;
        if ((bpp != 24 && bpp != 32) || comp > 3 || bw <= 0 || bh <= 0 || bw > 8192 || bh > 8192) return 0;
        stride = (bw * bpp / 8 + 3) & ~3;
        if (off + (long)stride * bh > n) return 0;
        px = malloc((size_t)bw * bh * 4);
        if (!px) return 0;
        for (y = 0; y < bh; y++) {
            const uint8_t *s = d + off + (long)(flip ? bh - 1 - y : y) * stride;
            for (x = 0; x < bw; x++) {
                int b = bpp / 8;
                px[y * bw + x] = s[x * b + 2] << 16 | s[x * b + 1] << 8 | s[x * b];
            }
        }
        *w = bw; *h = bh;
        return px;
    }
    return 0;
}

static const uint8_t *vget(long long off, int *n) {
    if (off < 0 || off >= v_size) { *n = 0; return w_buf; }
    if (!w_buf) w_buf = malloc(WIN);
    if (!(off >= w_off && off < w_off + w_len && (off < w_off + w_len - 8192 || w_off + w_len >= v_size))) {
        long long st = off > 65536 ? (off - 65536) & ~4095LL : 0;
        int r = 0, q;
        while (r < WIN && (q = pread(v_fd, w_buf + r, WIN - r, st + r)) > 0) r += q;
        w_off = st;
        w_len = r;
        if (off >= w_off + w_len) { *n = 0; return w_buf; }
    }
    *n = w_off + w_len - off;
    return w_buf + (off - w_off);
}

static int vbyte(long long i) {
    int n;
    const uint8_t *p = vget(i, &n);
    return n ? *p : -1;
}

/* a line is cut at 4k, minified 100M one-liners would kill the scrolling */
static long long ln_next(long long off) {
    long long o = off;
    int lim = 4096;
    while (o < v_size && lim > 0) {
        int n;
        const uint8_t *p = vget(o, &n), *q;
        if (!n) break;
        if (n > lim) n = lim;
        q = memchr(p, '\n', n);
        if (q) return o + (q - p) + 1;
        o += n;
        lim -= n;
    }
    return o;
}

static long long ln_prev(long long off) {
    long long i;
    if (off <= 0) return 0;
    for (i = off - 2; i >= 0 && i > off - 4098; i--)
        if (vbyte(i) == '\n') return i + 1;
    return i < 0 ? 0 : i + 1;
}

static void *idx_run(void *a) {
    Idx *x = a;
    int fd = open(x->path, O_RDONLY), r, k;
    uint8_t *b = malloc(65536);
    long long off = 0, lines = 0;
    if (fd >= 0 && b) {
        for (;;) {
            r = 0;
            while (r < 65536) {                       // short reads happen
                int q = pread(fd, b + r, 65536 - r, off + r);
                if (q <= 0) break;
                r += q;
            }
            if (x->stop || r <= 0) break;
            x->tab[off >> 16] = lines;
            __sync_synchronize();
            x->n = (off >> 16) + 1;
            for (k = 0; k < r; k++) lines += b[k] == '\n';
            off += r;
            if (r < 65536) break;
        }
        if (!x->stop) {
            uint8_t l = 0;
            if (off > 0 && pread(fd, &l, 1, off - 1) == 1 && l != '\n') lines++;
            x->lines = lines;
            __sync_synchronize();
            x->done = 1;
        }
    }
    if (fd >= 0) close(fd);
    free(b);
    idx_rel(x);
    return 0;
}

static void idx_start(void) {
    pthread_t t;
    Idx *x = calloc(1, sizeof *x);
    if (!x) return;
    x->tab = calloc((v_size >> 16) + 2, sizeof(long long));
    if (!x->tab) { free(x); return; }
    strcpy(x->path, v_path);
    x->size = v_size;
    idx = x;
    if (pthread_create(&t, 0, idx_run, x)) { idx = 0; free(x->tab); free(x); return; }
    pthread_detach(t);
}

static int open_view(const char *p, const char *name, int hex) {
    uint8_t b[4096];
    struct stat st;
    int fd = open(p, O_RDONLY), n, i;
    if (fd < 0) { sayf(name, strerror(errno)); return -1; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); say("not a regular file"); return -1; }
    n = pread(fd, b, sizeof b, 0);
    if (!hex)
        for (i = 0; i < n; i++) if (!b[i]) { hex = 1; break; }
    free_viewers();
    v_fd = fd;
    v_size = st.st_size;
    if (!v_size && n > 0) v_size = n;          // /proc stuff
    tv_top = 0;
    strncpy(v_name, name, 255);
    strncpy(v_path, p, 1023);
    mode = hex ? M_HEX : M_TEXT;
    if (!hex && v_size > 0) idx_start();
    dirty = 1;
    return 0;
}

static int view_text(const char *p, const char *name) { return open_view(p, name, 0); }

static void jfail(Job *j, const char *m) {
    snprintf(j->err, sizeof j->err, "%s", m);
    __sync_synchronize();
    j->state = 2;
}

static uint32_t *shrink_px(uint32_t *px, int *w, int *h, int max) {
    int f = (*w > *h ? *w : *h) / max + 1, nw = *w / f, nh = *h / f, x, y, i, j;
    uint32_t *o;
    if (f < 2) return px;
    o = malloc((size_t)nw * nh * 4);
    if (!o) return px;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (j = 0; j < f; j++)
                for (i = 0; i < f; i++) {
                    uint32_t c = px[(y * f + j) * *w + x * f + i];
                    r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255;
                }
            o[y * nw + x] = (r / (f * f)) << 16 | (g / (f * f)) << 8 | (b / (f * f));
        }
    free(px);
    *w = nw; *h = nh;
    return o;
}

static void *job_run(void *a) {
    Job *j = a;
    struct stat st;
    int fd = open(j->path, O_RDONLY), w = 0, h = 0, r, pk;
    long long got = 0;
    uint8_t *b = 0;
    uint32_t *px;
    char t[80];
    if (fd < 0) { jfail(j, strerror(errno)); goto out; }
    if (fstat(fd, &st) || st.st_size <= 0) { close(fd); jfail(j, "empty file"); goto out; }
    if (st.st_size > (64 << 20)) {
        close(fd);
        sprintf(t, "file too big (%d MB)", (int)(st.st_size >> 20));
        jfail(j, t);
        goto out;
    }
    j->total = st.st_size;
    b = malloc(st.st_size + 1);
    if (!b) { close(fd); jfail(j, "no memory"); goto out; }
    while (got < st.st_size && !j->stop) {
        long long want = st.st_size - got;
        r = read(fd, b + got, want > 262144 ? 262144 : want);
        if (r <= 0) break;
        got += r;
        j->done = got;
    }
    close(fd);
    if (j->stop) goto out;
    j->phase = 1;
    pk = img_peek(b, got, &w, &h);
    if (pk) {
        j->ow = w; j->oh = h;
        if (w <= 0 || h <= 0 || (long long)w * h > 120000000 || (b[0] != 0xFF && (w > 4096 || h > 4096))) {
            sprintf(t, "image too large (%dx%d)", w, h);
            jfail(j, t);
            goto out;
        }
    }
    pthread_mutex_lock(&dec_mx);
    px = load_pnm_bmp(b, got, &w, &h);
    if (!px) px = img_decode(b, got, &w, &h);
    pthread_mutex_unlock(&dec_mx);
    free(b);
    b = 0;
    if (!px) { jfail(j, "cant decode"); goto out; }
    if (j->stop) { free(px); goto out; }
    if (!pk) { j->ow = w; j->oh = h; }
    px = shrink_px(px, &w, &h, 2048);
    j->px = px;
    j->w = w; j->h = h;
    __sync_synchronize();
    j->state = 1;
out:
    free(b);
    job_rel(j);
    return 0;
}

static int view_img(const char *p, const char *name, int ent) {
    pthread_t t;
    pthread_attr_t at;
    Job *j = calloc(1, sizeof *j);
    job_cancel();
    if (!j) return -1;
    strncpy(j->path, p, 1023);
    strncpy(j->name, name, 255);
    j->ent = ent;
    job = j;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    if (pthread_create(&t, &at, job_run, j)) { job = 0; free(j); sayf(name, "no thread"); return -1; }
    pthread_detach(t);
    dirty = 1;
    return 0;
}

/* called from tick(): swap in the picture or show why not */
static void job_poll(void) {
    Job *j = job;
    if (!j || !j->state) return;
    if (j->state == 1) {
        uint32_t *px = j->px;
        int w = j->w, h = j->h, ow = j->ow, oh = j->oh, e = j->ent;
        char nm[256];
        strcpy(nm, j->name);
        j->px = 0;
        free_viewers();
        iv_px = px; iv_w = w; iv_h = h; iv_ow = ow; iv_oh = oh;
        mode = M_IMG;
        strcpy(v_name, nm);
        if (e >= 0 && e < n_ents) { cur = e; sel_none(); ents[e].sel = 1; anchor = e; }
    } else {
        sayf(j->name, j->err);
        job_cancel();
    }
    dirty = 1;
}

/* looks like text? */
static int sniff_text(const char *p) {
    uint8_t b[512];
    int fd = open(p, O_RDONLY), n, i;
    if (fd < 0) return 0;
    n = read(fd, b, sizeof b);
    close(fd);
    if (n <= 0) return 0;
    for (i = 0; i < n; i++)
        if (b[i] < 9 || (b[i] > 13 && b[i] < 32)) return 0;
    return 1;
}

static void open_ent(int i) {
    Ent *e = &ents[i];
    char p[1024], q[1100], c[1500];
    const char *x = ext_of(e->name), *cmd = 0;
    int k;
    join(p, cwd, e->name);
    if (e->kind == K_DIR) { enter_dir(p, 1); return; }
    if (e->kind == K_EXE && !*x) { quote(p, q); spawn(q); say("started"); return; }
    for (k = 0; k < n_assoc; k++)
        if (!strcasecmp(assoc[k].ext, x)) { cmd = assoc[k].cmd; break; }
    if (cmd) {
        if (!strcmp(cmd, "@view")) { view_text(p, e->name); return; }
        if (!strcmp(cmd, "@image")) { view_img(p, e->name, i); return; }
        quote(p, q);
        if (strstr(cmd, "%f")) {
            const char *s = strstr(cmd, "%f");
            snprintf(c, sizeof c, "%.*s%s%s", (int)(s - cmd), cmd, q, s + 2);
        } else snprintf(c, sizeof c, "%s %s", cmd, q);
        spawn(c);
        say("started");
        return;
    }
    if (e->kind == K_EXE) { quote(p, q); spawn(q); say("started"); return; }
    if (e->kind == K_IMG) { view_img(p, e->name, i); return; }
    if (e->kind == K_TEXT || sniff_text(p)) { view_text(p, e->name); return; }
    open_view(p, e->name, 1);
}

static void img_step(int d) {
    int i = cur, n;
    for (n = 0; n < n_ents; n++) {
        char p[1024];
        i += d;
        if (i < 0 || i >= n_ents) return;
        if (ents[i].kind != K_IMG) continue;
        join(p, cwd, ents[i].name);
        view_img(p, ents[i].name, i);
        return;
    }
}

static void close_viewer(void) {
    free_viewers();
    mode = M_BROWSE;
    show_cur();
    dirty = 1;
}

/* ---------- drawing ---------- */

static const char *places[][2] = {
    { "Home", 0 }, { "Root", "/" }, { "Disk (/mnt)", "/mnt" }, { "Usr", "/usr" }, { "Tmp", "/tmp" },
};
#define N_PLACES 5

static const char *place_path(int i) { return i ? places[i][1] : home; }

static void txt(int x, int y, int w, int h, const char *s, uint32_t c, int font) {
    int fh = font == F_MONO ? fh_mono : font == F_SMALL ? fh_small : fh_reg;
    be_text(x, y + (h - fh) / 2, s, c, font);
}

static void btn(int *r, const char *label, int primary, int x, int y, int w) {
    int hot = mx >= x && mx < x + w && my >= y && my < y + 28;
    r[0] = x; r[1] = y; r[2] = w; r[3] = 28;
    be_fill(x, y, w, 28, primary ? (hot ? 0xF0AC4A : ACC) : (hot ? RULE : BG2));
    if (!primary) frame(x, y, w, 28, RULE);
    be_text(x + (w - be_text_w(label, F_BOLD)) / 2, y + (28 - fh_reg) / 2, label, primary ? 0x1A1A1A : INK, F_BOLD);
}

static void draw_toolbar(void) {
    int i, W = be_w(), x;
    be_fill(0, 0, W, TB_H, BG);
    be_fill(0, TB_H - 1, W, 1, RULE);
    for (i = 0; i < 4; i++) {
        int on = i == 0 ? hp > 0 : i == 1 ? hp < hn - 1 : i == 2 ? strcmp(cwd, "/") != 0 : 1;
        x = 8 + i * 34;
        if (on && mx >= x && mx < x + 30 && my >= 8 && my < 36) be_fill(x, 8, 30, 28, HOVER);
        glyph_nav(i, x + 1, 8, on ? INK : FAINT);
    }
    x = 8 + 4 * 34 + 6;
    be_fill(x, 8, W - x - 48, 28, WELL);
    frame(x, 8, W - x - 48, 28, addr_on ? ACC : RULE);
    {
        char t[1100];
        const char *s = addr_on ? addr.buf : cwd;
        fit(s, W - x - 48 - 16, F_REG, t, sizeof t);
        if (addr_on && addr.all && addr.len) be_fill(x + 7, 12, be_text_w(t, F_REG) + 2, 20, SELBG);
        txt(x + 8, 8, 0, 28, t, INK, F_REG);
        if (addr_on) {
            char tmp[512];
            memcpy(tmp, addr.buf, addr.pos);
            tmp[addr.pos] = 0;
            be_fill(x + 8 + be_text_w(tmp, F_REG), 13, 1, 18, ACC);
        }
    }
    x = W - 40;
    if (mx >= x && mx < x + 32 && my >= 8 && my < 36) be_fill(x, 8, 32, 28, HOVER);
    if (grid) {
        for (i = 0; i < 3; i++) be_fill(x + 8, 14 + i * 6, 16, 2, INK);
    } else {
        for (i = 0; i < 4; i++) be_fill(x + 8 + (i & 1) * 9, 14 + (i >> 1) * 9, 7, 7, INK);
    }
}

static void draw_sidebar(void) {
    int i, y = TB_H + 14, H = be_h();
    be_fill(0, TB_H, SB_W, H - TB_H, BAR);
    be_fill(SB_W, TB_H, 1, H - TB_H, RULE);
    be_text(14, y, "PLACES", FAINT, F_SMALL);
    y += 22;
    for (i = 0; i < N_PLACES; i++) {
        const char *pp = place_path(i);
        int n = strlen(pp), on = !strncmp(cwd, pp, n) && (!cwd[n] || cwd[n] == '/' || n == 1);
        int j, better = 0;
        for (j = 0; j < N_PLACES; j++) {
            int m = strlen(place_path(j));
            if (j != i && m > n && !strncmp(cwd, place_path(j), m) && (!cwd[m] || cwd[m] == '/')) better = 1;
        }
        if (better) on = 0;
        if (on) { be_fill(0, y, SB_W, 30, 0x232428); be_fill(0, y, 3, 30, ACC); }
        else if (mx < SB_W && my >= y && my < y + 30) be_fill(0, y, SB_W, 30, HOVER);
        ico(K_DIR, 14, y + 7, 16);
        txt(40, y, 0, 30, places[i][0], on ? INK : DIM, F_REG);
        y += 32;
    }
}

static void draw_item(int i, int sx, int sy, int w, int h) {
    Ent *e = &ents[i];
    char t[300], b[64];
    uint32_t col = e->link ? 0x8FB8E8 : INK;
    int hot = i == hover;
    if (e->sel) be_fill(sx, sy, w, h, SELBG);
    else if (hot) be_fill(sx, sy, w, h, HOVER);
    if (i == cur) frame(sx, sy, w, h, e->sel ? ACC : FAINT);
    if (grid) {
        ico(e->kind, sx + (w - 48) / 2, sy + 6, 48);
        fit(e->name, w - 6, F_SMALL, t, sizeof t);
        be_text(sx + (w - be_text_w(t, F_SMALL)) / 2, sy + 62, t, col, F_SMALL);
        return;
    }
    {
        int W = w, dx = sx + W - 12, nw;
        ico(e->kind, sx + 10, sy + 2, 24);
        if (W > 560) { fmt_mode(e->mode, b); dx -= 100; txt(dx, sy, 0, h, b, DIM, F_SMALL); }
        dx -= 140;
        fmt_date(e->mtime, b);
        if (W > 400) txt(dx, sy, 0, h, b, DIM, F_SMALL);
        dx -= 90;
        if (e->kind != K_DIR) {
            fmt_size(e->size, b);
            txt(dx + 80 - be_text_w(b, F_SMALL), sy, 0, h, b, DIM, F_SMALL);
        }
        nw = dx - (sx + 44) - 8;
        fit(e->name, nw > 40 ? nw : 40, F_REG, t, sizeof t);
        txt(sx + 44, sy, 0, h, t, col, F_REG);
    }
}

static void draw_list_head(void) {
    int W = cont_w() - SCR_W, x = cont_x(), dx = x + W - 12;
    static const char *names[] = { "Name", "Size", "Modified" };
    char t[40];
    be_fill(x, TB_H, cont_w(), HDR_H, BG2);
    be_fill(x, TB_H + HDR_H - 1, cont_w(), 1, RULE);
    be_text(x + 44, TB_H + (HDR_H - fh_small) / 2, "Name", sort_key == S_NAME ? INK : DIM, F_SMALL);
    if (sort_key == S_NAME) be_text(x + 44 + be_text_w("Name", F_SMALL) + 6, TB_H + (HDR_H - fh_small) / 2, sort_rev ? "v" : "^", ACC, F_SMALL);
    if (W > 560) { dx -= 100; be_text(dx, TB_H + (HDR_H - fh_small) / 2, "Rights", DIM, F_SMALL); }
    dx -= 140;
    if (W > 400) {
        sprintf(t, "%s%s", names[2], sort_key == S_DATE ? (sort_rev ? " v" : " ^") : "");
        be_text(dx, TB_H + (HDR_H - fh_small) / 2, t, sort_key == S_DATE ? INK : DIM, F_SMALL);
    }
    dx -= 90;
    sprintf(t, "%s%s", names[1], sort_key == S_SIZE ? (sort_rev ? " v" : " ^") : "");
    be_text(dx + 80 - be_text_w(t, F_SMALL), TB_H + (HDR_H - fh_small) / 2, t, sort_key == S_SIZE ? INK : DIM, F_SMALL);
}

/* which column header is at x (list view) */
static int head_hit(int px) {
    int W = cont_w() - SCR_W, dx = cont_x() + W - 12;
    if (W > 560) dx -= 100;
    dx -= 140;
    if (W > 400 && px >= dx - 6 && px < dx + 130) return S_DATE;
    dx -= 90;
    if (px >= dx - 6 && px < dx + 84) return S_SIZE;
    if (px < dx - 6) return S_NAME;
    return -1;
}

static void draw_browse(void) {
    int i, cy = cont_y(), ch = cont_h(), cx = cont_x(), th = total_h();
    be_fill(cx, cy, cont_w(), ch, BG);
    for (i = 0; i < n_ents; i++) {
        int x, y, w, h;
        irect(i, &x, &y, &w, &h);
        y -= scroll;
        if (y + h < 0 || y > ch) continue;
        draw_item(i, cx + x, cy + y, w, h);
    }
    if (rb_on) {
        int x0 = rb_x0 < rb_x1 ? rb_x0 : rb_x1, x1 = rb_x0 < rb_x1 ? rb_x1 : rb_x0;
        int y0 = rb_y0 < rb_y1 ? rb_y0 : rb_y1, y1 = rb_y0 < rb_y1 ? rb_y1 : rb_y0;
        frame(cx + x0, cy + y0 - scroll, x1 - x0 + 1, y1 - y0 + 1, ACC);
    }
    if (!n_ents) be_text(cx + 20, cy + 20, "Empty folder", FAINT, F_REG);
    if (!grid) draw_list_head();
    if (th > ch) {
        int th2 = ch * ch / th, ty;
        if (th2 < 24) th2 = 24;
        ty = cy + (long)scroll * (ch - th2) / (th - ch);
        be_fill(cx + cont_w() - SCR_W + 3, cy, 1, ch, BG2);
        be_fill(cx + cont_w() - SCR_W + 2, ty, 6, th2, sb_drag ? ACC : FAINT);
    }
}

static void draw_status(void) {
    int H = be_h(), n = n_sel(), i;
    char t[200], b[40];
    be_fill(SB_W + 1, H - ST_H, be_w() - SB_W - 1, ST_H, BAR);
    be_fill(SB_W + 1, H - ST_H, be_w() - SB_W - 1, 1, RULE);
    if (now_ms() < msg_until) { txt(SB_W + 12, H - ST_H, 0, ST_H, msg, ACC, F_SMALL); return; }
    if (n) {
        long long s = 0;
        for (i = 0; i < n_ents; i++) if (ents[i].sel && ents[i].kind != K_DIR) s += ents[i].size;
        fmt_size(s, b);
        sprintf(t, "%d of %d selected, %s", n, n_ents, b);
    } else sprintf(t, "%d items%s", n_ents, show_hid ? " (hidden shown)" : "");
    txt(SB_W + 12, H - ST_H, 0, ST_H, t, DIM, F_SMALL);
}

static long long cur_line(void) {
    long long k, l, o, top = tv_top;
    if (!idx) return -1;
    k = top >> 16;
    if (k >= idx->n) return -1;
    l = idx->tab[k];
    for (o = k << 16; o < top; ) {
        int n, m;
        const uint8_t *p = vget(o, &n);
        if (!n) break;
        if (n > top - o) n = top - o;
        for (m = 0; m < n; m++) l += p[m] == '\n';
        o += n;
    }
    return l + 1;
}

static void draw_text_view(void) {
    int W = be_w(), H = be_h(), rows = (H - TB_H - ST_H - 8) / fh_mono, i, y = TB_H + 6;
    char t[320], hd[300];
    long long o = tv_top, l;
    be_fill(0, 0, W, H, BG);
    for (i = 0; i < rows && o < v_size; i++) {
        long long e = ln_next(o), q = o;
        int k = 0;
        while (q < e && k < 300) {
            int n, m;
            const uint8_t *s = vget(q, &n);
            if (!n) break;
            if (n > e - q) n = e - q;
            for (m = 0; m < n && k < 300; m++) {
                uint8_t c = s[m];
                if (c == '\t') { do t[k++] = ' '; while (k & 3); }
                else if (c == '\r' || c == '\n') continue;
                else t[k++] = c >= 32 ? c : '.';
            }
            q += n;
        }
        if (k > 300) k = 300;
        t[k] = 0;
        if (k) be_text(14, y + i * fh_mono, t, INK, F_MONO);
        o = e;
    }
    be_fill(0, 0, W, TB_H, BG2);
    be_fill(0, TB_H - 1, W, 1, RULE);
    be_text(14, (TB_H - fh_reg) / 2, v_name, INK, F_BOLD);
    l = cur_line();
    if (l < 0) sprintf(hd, "%d%%  (counting lines)   Esc: back", v_size ? (int)(tv_top * 100 / v_size) : 0);
    else if (idx && idx->done) sprintf(hd, "line %lld / %lld   %d%%   Esc: back", l, idx->lines, (int)(tv_top * 100 / v_size));
    else sprintf(hd, "line %lld   %d%%   Esc: back", l, (int)(tv_top * 100 / v_size));
    be_text(W - be_text_w(hd, F_SMALL) - 14, (TB_H - fh_small) / 2, hd, DIM, F_SMALL);
}

static void draw_hex_view(void) {
    int W = be_w(), H = be_h(), rows = (H - TB_H - ST_H - 8) / fh_mono, i, y = TB_H + 6;
    char t[120], hd[300];
    be_fill(0, 0, W, H, BG);
    for (i = 0; i < rows; i++) {
        long long o = tv_top + i * 16;
        uint8_t b[16];
        int k, n = 0, m, c;
        if (o >= v_size) break;
        while (n < 16 && o + n < v_size) {
            const uint8_t *p = vget(o + n, &m);
            if (!m) break;
            if (m > 16 - n) m = 16 - n;
            memcpy(b + n, p, m);
            n += m;
        }
        c = sprintf(t, "%08llx  ", o);
        for (k = 0; k < 16; k++) {
            if (k < n) c += sprintf(t + c, "%02x ", b[k]);
            else c += sprintf(t + c, "   ");
            if (k == 7) t[c++] = ' ';
        }
        t[c++] = ' ';
        for (k = 0; k < n; k++) t[c++] = b[k] >= 32 && b[k] < 127 ? b[k] : '.';
        t[c] = 0;
        be_text(14, y + i * fh_mono, t, INK, F_MONO);
    }
    be_fill(0, 0, W, TB_H, BG2);
    be_fill(0, TB_H - 1, W, 1, RULE);
    be_text(14, (TB_H - fh_reg) / 2, v_name, INK, F_BOLD);
    sprintf(hd, "hex  %llx / %llx   %d%%   Esc: back", tv_top, v_size, v_size ? (int)(tv_top * 100 / v_size) : 0);
    be_text(W - be_text_w(hd, F_SMALL) - 14, (TB_H - fh_small) / 2, hd, DIM, F_SMALL);
}

static void draw_job(void) {
    int W = be_w(), H = be_h(), w = 380, h = 112, x = (W - w) / 2, y = (H - h) / 2, bw = w - 40;
    char t[300];
    be_fill(x, y, w, h, BG2);
    frame(x, y, w, h, RULE);
    fit(job->name, w - 40, F_BOLD, t, sizeof t);
    be_text(x + 20, y + 16, t, INK, F_BOLD);
    be_fill(x + 20, y + 52, bw, 10, WELL);
    if (job->phase == 0) {
        long long d = job->done, tt = job->total;
        be_fill(x + 20, y + 52, tt ? (int)(d * bw / tt) : 0, 10, ACC);
        sprintf(t, "reading, %d%%", tt ? (int)(d * 100 / tt) : 0);
    } else {
        int p = now_ms() / 8 % (bw + 80);
        int a = p - 80 < 0 ? 0 : p - 80, b = p > bw ? bw : p;
        if (b > a) be_fill(x + 20 + a, y + 52, b - a, 10, ACC);
        strcpy(t, "decoding...");
    }
    be_text(x + 20, y + 70, t, DIM, F_SMALL);
    be_text(x + w - 20 - be_text_w("Esc: cancel", F_SMALL), y + 70, "Esc: cancel", DIM, F_SMALL);
}

static void draw_img_view(void) {
    int W = be_w(), H = be_h(), aw = W - 20, ah = H - TB_H - 20, w = iv_w, h = iv_h;
    char hd[300];
    be_fill(0, 0, W, H, WELL);
    if (w > aw || h > ah) {
        if ((long)aw * h < (long)ah * w) { h = (long)h * aw / w; w = aw; }
        else { w = (long)w * ah / h; h = ah; }
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    be_blit((W - w) / 2, TB_H + (H - TB_H - h) / 2, w, h, iv_px, iv_w, iv_h);
    be_fill(0, 0, W, TB_H, BG2);
    be_fill(0, TB_H - 1, W, 1, RULE);
    be_text(14, (TB_H - fh_reg) / 2, v_name, INK, F_BOLD);
    if (iv_ow && (iv_ow != iv_w || iv_oh != iv_h)) sprintf(hd, "%dx%d (shown %dx%d)   arrows: next, Esc: back", iv_ow, iv_oh, iv_w, iv_h);
    else sprintf(hd, "%dx%d   arrows: next, Esc: back", iv_w, iv_h);
    be_text(W - be_text_w(hd, F_SMALL) - 14, (TB_H - fh_small) / 2, hd, DIM, F_SMALL);
}

static void draw_menu(void) {
    int i, w = 190, h = 8, y;
    for (i = 0; i < n_menu; i++) h += menu[i].id ? 28 : 9;
    if (menu_x + w > be_w()) menu_x = be_w() - w - 2;
    if (menu_y + h > be_h()) menu_y = be_h() - h - 2;
    be_fill(menu_x + 3, menu_y + 4, w, h, 0x0A0A0C);
    be_fill(menu_x, menu_y, w, h, 0x14151A);
    frame(menu_x, menu_y, w, h, RULE);
    y = menu_y + 4;
    for (i = 0; i < n_menu; i++) {
        if (!menu[i].id) { be_fill(menu_x + 8, y + 4, w - 16, 1, RULE); y += 9; continue; }
        if (i == menu_hot && menu[i].on) be_fill(menu_x + 3, y, w - 6, 28, 0x21222A);
        txt(menu_x + 16, y, 0, 28, menu[i].s, menu[i].on ? INK : FAINT, F_REG);
        y += 28;
    }
}

static void draw_modal(void) {
    int w = 440, h, x, y, i, W = be_w(), H = be_h();
    char t[300];
    h = modal == MD_PROPS ? 76 + n_props * 24 + 44 : modal == MD_PROG ? 140 : modal == MD_INPUT ? 160 : 150;
    x = (W - w) / 2; y = (H - h) / 2;
    be_fill(x, y, w, h, BG2);
    frame(x, y, w, h, RULE);
    be_text(x + 20, y + 16, modal_title, INK, F_BIG);
    switch (modal) {
    case MD_CONFIRM:
    case MD_MSG: {
        char *s = modal_text, *nl;
        int ly = y + 54;
        while (*s) {
            nl = strchr(s, '\n');
            i = nl ? nl - s : (int)strlen(s);
            memcpy(t, s, i > 299 ? 299 : i);
            t[i > 299 ? 299 : i] = 0;
            be_text(x + 20, ly, t, DIM, F_REG);
            ly += 22;
            s += i + (nl ? 1 : 0);
        }
        break;
    }
    case MD_INPUT: {
        char tmp[512];
        be_text(x + 20, y + 52, modal_text, DIM, F_REG);
        be_fill(x + 20, y + 76, w - 40, 28, WELL);
        frame(x + 20, y + 76, w - 40, 28, ACC);
        fit(ed.buf, w - 56, F_REG, t, sizeof t);
        if (ed.all && ed.len) be_fill(x + 27, y + 80, be_text_w(t, F_REG) + 2, 20, SELBG);
        txt(x + 28, y + 76, 0, 28, t, INK, F_REG);
        memcpy(tmp, ed.buf, ed.pos);
        tmp[ed.pos] = 0;
        be_fill(x + 28 + be_text_w(tmp, F_REG), y + 81, 1, 18, ACC);
        break;
    }
    case MD_PROPS:
        for (i = 0; i < n_props; i++) {
            char *tab = strchr(props[i], '\t');
            char k[40], v[300];
            int kl = tab - props[i];
            memcpy(k, props[i], kl);
            k[kl] = 0;
            fit(tab + 1, w - 150, F_REG, v, sizeof v);
            be_text(x + 20, y + 58 + i * 24, k, DIM, F_SMALL);
            be_text(x + 120, y + 56 + i * 24, v, INK, F_REG);
        }
        break;
    case MD_PROG: {
        int bw = w - 40, fill = tot_bytes ? (int)((long long)done_bytes * bw / tot_bytes) : 0;
        fit(prog_name, w - 40, F_REG, t, sizeof t);
        be_text(x + 20, y + 52, t, DIM, F_REG);
        be_fill(x + 20, y + 80, bw, 10, WELL);
        be_fill(x + 20, y + 80, fill > bw ? bw : fill, 10, ACC);
        sprintf(t, "%d%%", tot_bytes ? (int)(done_bytes * 100 / tot_bytes) : 0);
        be_text(x + 20, y + 98, t, DIM, F_SMALL);
        break;
    }
    }
    bx_ok[0] = bx_cancel[0] = -1;
    bx_ok[2] = bx_cancel[2] = 0;
    if (modal == MD_CONFIRM) { btn(bx_ok, "Delete", 1, x + w - 100, y + h - 44, 80); btn(bx_cancel, "Cancel", 0, x + w - 190, y + h - 44, 80); }
    else if (modal == MD_INPUT) { btn(bx_ok, "OK", 1, x + w - 100, y + h - 44, 80); btn(bx_cancel, "Cancel", 0, x + w - 190, y + h - 44, 80); }
    else if (modal == MD_PROG) btn(bx_cancel, "Cancel", 0, x + w - 100, y + h - 44, 80);
    else btn(bx_ok, "OK", 1, x + w - 100, y + h - 44, 80);
}

static void draw(void) {
    int W = be_w(), H = be_h();
    if (mode == M_TEXT) draw_text_view();
    else if (mode == M_HEX) draw_hex_view();
    else if (mode == M_IMG) draw_img_view();
    else {
        be_fill(0, 0, W, H, BG);
        draw_browse();
        draw_sidebar();
        draw_toolbar();
        draw_status();
    }
    if (menu_on) draw_menu();
    if (modal) draw_modal();
    if (job) draw_job();
}

/* ---------- input ---------- */

static void add_mi(const char *s, int id, int on) {
    menu[n_menu].s = s; menu[n_menu].id = id; menu[n_menu].on = on;
    n_menu++;
}

static void open_menu(int px, int py, int on_ent) {
    int ns = n_sel();
    n_menu = 0;
    if (on_ent) {
        add_mi("Open", A_OPEN, ns == 1);
        add_mi(0, 0, 0);
        add_mi("Cut", A_CUT, 1);
        add_mi("Copy", A_COPY, 1);
        add_mi("Paste", A_PASTE, ns == 1 && ents[cur].kind == K_DIR && clip_has());
        add_mi(0, 0, 0);
        add_mi("Rename", A_RENAME, ns == 1);
        add_mi("Delete", A_DELETE, 1);
        add_mi(0, 0, 0);
        add_mi("Properties", A_PROPS, 1);
    } else {
        add_mi("New folder", A_NEWDIR, 1);
        add_mi("New file", A_NEWFILE, 1);
        add_mi("Paste", A_PASTE, clip_has());
        add_mi(0, 0, 0);
        add_mi("Sort by name", A_SNAME, 1);
        add_mi("Sort by size", A_SSIZE, 1);
        add_mi("Sort by date", A_SDATE, 1);
        add_mi(0, 0, 0);
        add_mi(grid ? "List view" : "Icon view", A_VIEW, 1);
        add_mi(show_hid ? "Hide hidden files" : "Show hidden files", A_HIDDEN, 1);
        add_mi("Refresh", A_REFRESH, 1);
        add_mi(0, 0, 0);
        add_mi("Properties", A_PROPS, 1);
    }
    menu_x = px; menu_y = py;
    menu_hot = -1;
    menu_on = 1;
    dirty = 1;
}

static int menu_at(int px, int py) {
    int i, y = menu_y + 4;
    if (px < menu_x || px >= menu_x + 190) return -2;
    for (i = 0; i < n_menu; i++) {
        int h = menu[i].id ? 28 : 9;
        if (py >= y && py < y + h) return menu[i].id ? i : -1;
        y += h;
    }
    return -2;
}

static void set_sort(int k) {
    if (sort_key == k) sort_rev = !sort_rev;
    else { sort_key = k; sort_rev = 0; }
    load_dir();
}

static void do_paste_into(void) {
    char keep[1024];
    if (n_sel() == 1 && ents[cur].kind == K_DIR) {
        strcpy(keep, cwd);
        join(cwd, keep, ents[cur].name);
        paste();
        strcpy(cwd, keep);
        load_dir();
    } else paste();
}

static void action(int id) {
    menu_on = 0;
    dirty = 1;
    switch (id) {
    case A_OPEN: if (cur >= 0) open_ent(cur); break;
    case A_CUT: clip_write("cut"); break;
    case A_COPY: clip_write("copy"); break;
    case A_PASTE: do_paste_into(); break;
    case A_RENAME: if (cur >= 0) ask_input("Rename", "New name:", ents[cur].name, IA_RENAME); break;
    case A_DELETE:
        if (!n_sel()) break;
        {
            char t[100];
            if (n_sel() == 1) sprintf(t, "Delete \"%.60s\"?\nThis cannot be undone.", ents[cur].name);
            else sprintf(t, "Delete %d items?\nThis cannot be undone.", n_sel());
            modal = MD_CONFIRM;
            strcpy(modal_title, "Delete");
            strcpy(modal_text, t);
        }
        break;
    case A_NEWDIR: { char n[300]; new_name("New folder", n); ask_input("New folder", "Folder name:", n, IA_NEWDIR); break; }
    case A_NEWFILE: { char n[300]; new_name("New file", n); ask_input("New file", "File name:", n, IA_NEWFILE); break; }
    case A_PROPS: show_props(); break;
    case A_SNAME: set_sort(S_NAME); break;
    case A_SSIZE: set_sort(S_SIZE); break;
    case A_SDATE: set_sort(S_DATE); break;
    case A_VIEW: grid = !grid; scroll = 0; clamp_scroll(); show_cur(); break;
    case A_HIDDEN: show_hid = !show_hid; load_dir(); break;
    case A_REFRESH: load_dir(); break;
    }
}

static void select_range(int a, int b) {
    int i, lo = a < b ? a : b, hi = a < b ? b : a;
    for (i = 0; i < n_ents; i++) ents[i].sel = i >= lo && i <= hi;
}

static void move_cur(int n, int mods) {
    if (!n_ents) return;
    if (n < 0) n = 0;
    if (n >= n_ents) n = n_ents - 1;
    if (mods & MOD_SHIFT) select_range(anchor < 0 ? n : anchor, n);
    else if (!(mods & MOD_CTRL)) { sel_none(); ents[n].sel = 1; anchor = n; }
    cur = n;
    show_cur();
    dirty = 1;
}

static void addr_go(void) {
    struct stat st;
    char *s = addr.buf;
    char p[1024];
    if (s[0] == '~') { snprintf(p, sizeof p, "%s%s", home, s + 1); s = p; }
    addr_on = 0;
    dirty = 1;
    if (stat(s, &st) < 0) { sayf(s, strerror(errno)); return; }
    if (S_ISDIR(st.st_mode)) enter_dir(s, 1);
    else {
        char d[1024], *sl;
        strcpy(d, s);
        sl = strrchr(d, '/');
        if (sl) {
            int i;
            char nm[256];
            strncpy(nm, sl + 1, 255);
            nm[255] = 0;
            *sl = 0;
            enter_dir(sl == d ? "/" : d, 1);
            for (i = 0; i < n_ents; i++)
                if (!strcmp(ents[i].name, nm)) { move_cur(i, 0); open_ent(i); }
        }
    }
}

static void type_ahead(int c) {
    char s[5];
    int n = 0, i, l;
    cp_utf8(c, s, &n);
    s[n] = 0;
    l = n;
    for (i = 1; i <= n_ents; i++) {
        int k = (cur + i) % n_ents;
        if (!strncasecmp(ents[k].name, s, l)) { move_cur(k, 0); return; }
    }
}

static void key_browse(FmEv *e) {
    int c = e->a, m = e->mods, rows = cont_h() / (grid ? CELL_H : ROW_H), step = grid ? cols() : 1;
    if (rows < 1) rows = 1;
    if (cur < 0 && n_ents && (c == FK_UP || c == FK_DOWN || c == FK_LEFT || c == FK_RIGHT)) { move_cur(0, m); return; }
    switch (c) {
    case FK_UP: move_cur(cur - step, m); break;
    case FK_DOWN: move_cur(cur + step, m); break;
    case FK_LEFT:
        if (m & MOD_ALT) go_hist(-1);
        else if (grid) move_cur(cur - 1, m);
        break;
    case FK_RIGHT:
        if (m & MOD_ALT) go_hist(1);
        else if (grid) move_cur(cur + 1, m);
        break;
    case FK_HOME: move_cur(0, m); break;
    case FK_END: move_cur(n_ents - 1, m); break;
    case FK_PGUP: move_cur(cur - rows * step, m); break;
    case FK_PGDN: move_cur(cur + rows * step, m); break;
    case FK_ENTER: if (cur >= 0) open_ent(cur); break;
    case FK_BACK: go_up(); break;
    case FK_DEL: action(A_DELETE); break;
    case FK_F2: if (cur >= 0) action(A_RENAME); break;
    case FK_F5: load_dir(); break;
    case FK_ESC: sel_none(); dirty = 1; break;
    default:
        if (m & MOD_CTRL) {
            if (c == 'a') { int i; for (i = 0; i < n_ents; i++) ents[i].sel = 1; dirty = 1; }
            else if (c == 'c' || c == 'x') { if (n_sel()) clip_write(c == 'c' ? "copy" : "cut"); }
            else if (c == 'v') action(A_PASTE);
            else if (c == 'h') action(A_HIDDEN);
            else if (c == 'l') { addr_on = 1; edit_set(&addr, cwd); dirty = 1; }
            else if (c == 'n') action(A_NEWDIR);
            else if (c == ' ' && cur >= 0) { ents[cur].sel = !ents[cur].sel; dirty = 1; }
        } else if (c == ' ' && cur >= 0 && (m & MOD_SHIFT)) { ents[cur].sel = !ents[cur].sel; dirty = 1; }
        else if (c > 32 && c < 0x200000) type_ahead(c);
    }
}

static void key_text(FmEv *e) {
    int rows = (be_h() - TB_H - ST_H - 8) / fh_mono, i, hex = mode == M_HEX;
    long long last = hex ? (v_size ? (v_size - 1) & ~15LL : 0) : v_size;
    switch (e->a) {
    case FK_ESC: case FK_BACK: case 'q': close_viewer(); return;
    case FK_UP: if (hex) tv_top -= 16; else tv_top = ln_prev(tv_top); break;
    case FK_DOWN: if (hex) tv_top += 16; else if (ln_next(tv_top) < v_size) tv_top = ln_next(tv_top); break;
    case FK_PGUP:
        if (hex) tv_top -= (rows - 1) * 16;
        else for (i = 0; i < rows - 1; i++) tv_top = ln_prev(tv_top);
        break;
    case FK_PGDN: case ' ':
        if (hex) tv_top += (rows - 1) * 16;
        else for (i = 0; i < rows - 1 && ln_next(tv_top) < v_size; i++) tv_top = ln_next(tv_top);
        break;
    case FK_HOME: tv_top = 0; break;
    case FK_END:
        if (hex) tv_top = last - (rows - 1) * 16;
        else { tv_top = v_size; for (i = 0; i < rows - 1; i++) tv_top = ln_prev(tv_top); }
        break;
    case 'h': case 'x': if (!hex && (e->mods & MOD_CTRL) == 0) { mode = M_HEX; tv_top &= ~15LL; } break;
    case 't': if (hex) { mode = M_TEXT; tv_top = ln_prev(ln_next(tv_top)); } break;
    }
    if (hex && tv_top > last) tv_top = last;
    if (tv_top < 0) tv_top = 0;
    dirty = 1;
}

static void on_key(FmEv *e) {
    if (job) {
        if (e->a == FK_ESC) { job_cancel(); say("cancelled"); dirty = 1; }
        return;
    }
    if (modal) {
        if (modal == MD_INPUT) {
            int r = edit_key(&ed, e);
            if (r == 1) input_done();
            else if (r == 2) modal = MD_NONE;
        } else if (e->a == FK_ENTER || e->a == FK_ESC) {
            int was = modal;
            modal = MD_NONE;
            if (was == MD_CONFIRM && e->a == FK_ENTER) del_sel();
        }
        dirty = 1;
        return;
    }
    if (menu_on) {
        int n = n_menu, i;
        if (e->a == FK_ESC) menu_on = 0;
        else if (e->a == FK_DOWN || e->a == FK_UP) {
            int d = e->a == FK_DOWN ? 1 : -1;
            i = menu_hot;
            do i = (i + d + n) % n; while (!menu[i].id || !menu[i].on);
            menu_hot = i;
        } else if (e->a == FK_ENTER && menu_hot >= 0) action(menu[menu_hot].id);
        dirty = 1;
        return;
    }
    if (addr_on) {
        int r = edit_key(&addr, e);
        if (r == 1) addr_go();
        else if (r == 2) addr_on = 0;
        dirty = 1;
        return;
    }
    if (mode == M_TEXT || mode == M_HEX) { key_text(e); return; }
    if (mode == M_IMG) {
        if (e->a == FK_ESC || e->a == FK_BACK || e->a == 'q') close_viewer();
        else if (e->a == FK_LEFT || e->a == FK_UP) img_step(-1);
        else if (e->a == FK_RIGHT || e->a == FK_DOWN || e->a == ' ') img_step(1);
        dirty = 1;
        return;
    }
    key_browse(e);
}

static int in_box(int *b, int px, int py) {
    return b[2] && px >= b[0] && px < b[0] + b[2] && py >= b[1] && py < b[1] + b[3];
}

static void band_select(int add) {
    int i, x0 = rb_x0 < rb_x1 ? rb_x0 : rb_x1, x1 = rb_x0 < rb_x1 ? rb_x1 : rb_x0;
    int y0 = rb_y0 < rb_y1 ? rb_y0 : rb_y1, y1 = rb_y0 < rb_y1 ? rb_y1 : rb_y0;
    for (i = 0; i < n_ents; i++) {
        int x, y, w, h, hit;
        irect(i, &x, &y, &w, &h);
        hit = x < x1 && x + w > x0 && y < y1 && y + h > y0;
        if (hit) ents[i].sel = 1;
        else if (!add) ents[i].sel = 0;
    }
}

static void on_down(FmEv *e) {
    int px = e->a, py = e->b, m = e->mods, i;
    uint32_t t = now_ms();
    mx = px; my = py;
    if (job) return;
    dirty = 1;
    if (modal) {
        if (modal == MD_PROG) return;
        if (in_box(bx_ok, px, py)) {
            int was = modal;
            if (was == MD_INPUT) input_done();
            else { modal = MD_NONE; if (was == MD_CONFIRM) del_sel(); }
        } else if (in_box(bx_cancel, px, py)) modal = MD_NONE;
        return;
    }
    if (menu_on) {
        i = menu_at(px, py);
        if (i >= 0 && menu[i].on) { action(menu[i].id); return; }
        menu_on = 0;
        if (i != -2 || e->c != 2) return;
    }
    if (mode != M_BROWSE) return;
    if (e->c == 2) {
        addr_on = 0;
        if (px < SB_W || py < TB_H || py >= be_h() - ST_H) return;
        i = hit_ent(px, py);
        if (i >= 0) {
            if (!ents[i].sel) { sel_none(); ents[i].sel = 1; }
            cur = anchor = i;
            open_menu(px, py, 1);
        } else {
            sel_none();
            open_menu(px, py, 0);
        }
        return;
    }
    if (py < TB_H) {
        int x = 8 + 4 * 34 + 6;
        if (py >= 8 && py < 36) {
            for (i = 0; i < 4; i++)
                if (px >= 8 + i * 34 && px < 38 + i * 34) {
                    if (i == 0) go_hist(-1);
                    else if (i == 1) go_hist(1);
                    else if (i == 2) go_up();
                    else enter_dir(home, 1);
                    addr_on = 0;
                    return;
                }
            if (px >= x && px < be_w() - 48) {
                if (!addr_on) { addr_on = 1; edit_set(&addr, cwd); }
                else {
                    /* click in the field: put the caret near the click */
                    char tmp[512];
                    int k;
                    addr.all = 0;
                    for (k = 0; k <= addr.len; k++) {
                        memcpy(tmp, addr.buf, k);
                        tmp[k] = 0;
                        if (x + 8 + be_text_w(tmp, F_REG) >= px && (k == addr.len || (addr.buf[k] & 0xC0) != 0x80)) break;
                    }
                    addr.pos = k > addr.len ? addr.len : k;
                }
                return;
            }
            if (px >= be_w() - 40 && px < be_w() - 8) { grid = !grid; scroll = 0; clamp_scroll(); addr_on = 0; return; }
        }
        addr_on = 0;
        return;
    }
    addr_on = 0;
    if (px < SB_W) {
        int y = TB_H + 36;
        for (i = 0; i < N_PLACES; i++, y += 32)
            if (py >= y && py < y + 30) enter_dir(place_path(i), 1);
        return;
    }
    if (py >= be_h() - ST_H) return;
    if (!grid && py < TB_H + HDR_H) {
        int k = head_hit(px);
        if (k >= 0) set_sort(k);
        return;
    }
    if (px >= be_w() - SCR_W - 2 && total_h() > cont_h()) {
        sb_drag = 1;
        scroll = (long)(py - cont_y()) * (total_h() - cont_h()) / cont_h();
        clamp_scroll();
        return;
    }
    i = hit_ent(px, py);
    if (i >= 0) {
        if (m & MOD_SHIFT) select_range(anchor < 0 ? i : anchor, i);
        else if (m & MOD_CTRL) { ents[i].sel = !ents[i].sel; anchor = i; }
        else { sel_none(); ents[i].sel = 1; anchor = i; }
        cur = i;
        if (!(m & (MOD_SHIFT | MOD_CTRL)) && click_idx == i && t - click_ms < 450) {
            click_idx = -1;
            open_ent(i);
            return;
        }
        click_idx = i;
        click_ms = t;
        return;
    }
    click_idx = -1;
    if (!(m & (MOD_SHIFT | MOD_CTRL))) sel_none();
    rb_on = 1;
    rb_x0 = rb_x1 = px - cont_x();
    rb_y0 = rb_y1 = py - cont_y() + scroll;
}

static void on_move(FmEv *e) {
    int h;
    if (e->a == mx && e->b == my) return;
    mx = e->a; my = e->b;
    dirty = 1;
    if (menu_on) { int i = menu_at(mx, my); menu_hot = i >= 0 ? i : -1; return; }
    if (sb_drag) {
        scroll = (long)(my - cont_y()) * (total_h() - cont_h()) / cont_h();
        clamp_scroll();
        return;
    }
    if (rb_on) {
        if (my < cont_y()) scroll -= 24;
        if (my > be_h() - ST_H) scroll += 24;
        clamp_scroll();
        rb_x1 = mx - cont_x();
        rb_y1 = my - cont_y() + scroll;
        band_select(0);
        return;
    }
    h = (mode == M_BROWSE && !modal) ? hit_ent(mx, my) : -1;
    hover = h;
}

static void on_wheel(int d) {
    if (modal || menu_on) return;
    if (job) return;
    if (mode == M_TEXT) {
        for (; d > 0 && ln_next(tv_top) < v_size; d--) tv_top = ln_next(tv_top);
        for (; d < 0; d++) tv_top = ln_prev(tv_top);
    } else if (mode == M_HEX) {
        tv_top += d * 48;
        if (tv_top > ((v_size - 1) & ~15LL)) tv_top = (v_size - 1) & ~15LL;
        if (tv_top < 0) tv_top = 0;
    } else if (mode == M_BROWSE) {
        scroll += d * (grid ? 40 : ROW_H * 2);
        clamp_scroll();
        hover = hit_ent(mx, my);
    }
    dirty = 1;
}

static void tick(void) {
    uint32_t t = now_ms();
    if (job) { job_poll(); dirty = 1; }
    static int had_msg;
    while (waitpid(-1, 0, WNOHANG) > 0) {}
    if (had_msg && t >= msg_until) { had_msg = 0; dirty = 1; }
    if (t < msg_until) had_msg = 1;
    if (mode == M_BROWSE && !modal && t - last_scan > 1500) {
        struct stat st;
        DIR *d;
        int n = 0;
        time_t mt = 0;
        last_scan = t;
        if (!stat(cwd, &st)) mt = st.st_mtime;
        d = opendir(cwd);
        if (d) { while (readdir(d)) n++; closedir(d); }
        if (mt != dsig_m || n != dsig_n) load_dir();
    }
}

int main(int argc, char **argv) {
    FmEv e;
    int r;
    if (getenv("HOME")) strncpy(home, getenv("HOME"), 255);
    if (be_open(900, 600, "Files") < 0) {
        fprintf(stderr, "fm: no desktop\n");
        return 1;
    }
    fh_reg = be_font_h(F_REG);
    fh_small = be_font_h(F_SMALL);
    fh_mono = be_font_h(F_MONO);
    conf_load();
    sort_key = S_NAME;
    enter_dir(argc > 1 ? argv[1] : home, 1);
    if (!cwd[0]) enter_dir("/", 1);
    while (!quit) {
        r = be_wait(&e, job ? 40 : 250);
        if (r < 0) break;
        while (r > 0) {
            switch (e.type) {
            case EV_CLOSE: quit = 1; break;
            case EV_RESIZE: clamp_scroll(); dirty = 1; break;
            case EV_KEY: on_key(&e); break;
            case EV_DOWN: on_down(&e); break;
            case EV_UP: sb_drag = 0; rb_on = 0; dirty = 1; break;
            case EV_MOVE: on_move(&e); break;
            case EV_WHEEL: on_wheel(e.a); break;
            }
            if (quit) break;
            r = be_wait(&e, 0);
        }
        tick();
        if (dirty && !quit) {
            dirty = 0;
            draw();
            if (be_flip() < 0) break;
        }
    }
    free_viewers();
    be_close();
    return 0;
}
