#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "samara.h"
#include "dapps.h"

uint32_t c_bg = 0x17181B, c_bg2 = 0x1E1F23, c_bar = 0x131417, c_well = 0x101114, c_ink = 0xE6E6E6, c_dim = 0x85868C,
         c_faint = 0x55565C, c_rule = 0x2C2D32, c_hover = 0x25262A, c_acc = 0xE39B32, c_selbg = 0x3A2E1A,
         c_danger = 0xC43B30, c_ok = 0x5FB06A;
int fh_reg, fh_small, fh_mono;

void th_load(void) {
    FILE *f = fopen("/etc/samara-desktop.conf", "r");
    char l[128];
    int light = 0;
    if (f) {
        while (fgets(l, sizeof l, f)) if (!strncmp(l, "theme=light", 11)) light = 1;
        fclose(f);
    }
    if (light) {
        c_bg = 0xF4F4F2; c_bg2 = 0xEAEAE6; c_bar = 0xE2E2DE; c_well = 0xFFFFFF; c_ink = 0x1E1F23; c_dim = 0x62646B;
        c_faint = 0x9A9BA0; c_rule = 0xC8C8C4; c_hover = 0xE0E0DC; c_selbg = 0xF6DDB0;
    }
}

uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000u + ts.tv_nsec / 1000000;
}

int cp_utf8(int c, char *o) {
    int n = 0;
    if (c < 0x80) o[n++] = c;
    else if (c < 0x800) { o[n++] = 0xC0 | (c >> 6); o[n++] = 0x80 | (c & 63); }
    else { o[n++] = 0xE0 | (c >> 12); o[n++] = 0x80 | ((c >> 6) & 63); o[n++] = 0x80 | (c & 63); }
    return n;
}

int u8_len(const char *s) {
    unsigned char c = *s;
    return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

void fit(const char *s, int w, int font, char *out, int cap) {
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

void frame(int x, int y, int w, int h, uint32_t c) {
    be_fill(x, y, w, 1, c);
    be_fill(x, y + h - 1, w, 1, c);
    be_fill(x, y, 1, h, c);
    be_fill(x + w - 1, y, 1, h, c);
}

void txt(int x, int y, int h, const char *s, uint32_t c, int font) {
    int fh = font == F_MONO ? fh_mono : font == F_SMALL ? fh_small : fh_reg;
    be_text(x, y + (h - fh) / 2, s, c, font);
}

int button(int x, int y, int w, const char *label, int primary, int mx, int my) {
    int hot = mx >= x && mx < x + w && my >= y && my < y + 28;
    be_fill(x, y, w, 28, primary ? (hot ? 0xF0AC4A : c_acc) : (hot ? c_rule : c_bg2));
    if (!primary) frame(x, y, w, 28, c_rule);
    be_text(x + (w - be_text_w(label, F_BOLD)) / 2, y + (28 - fh_reg) / 2, label, primary ? 0x1A1A1A : c_ink, F_BOLD);
    return hot;
}

void le_set(LineEd *e, const char *s) {
    strncpy(e->buf, s, 500);
    e->buf[500] = 0;
    e->len = e->pos = strlen(e->buf);
    e->all = 1;
}

static void le_ins(LineEd *e, const char *t, int n) {
    if (e->all) { e->len = e->pos = 0; e->buf[0] = 0; e->all = 0; }
    if (e->len + n >= 500) return;
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos + 1);
    memcpy(e->buf + e->pos, t, n);
    e->pos += n; e->len += n;
}

static void le_del(LineEd *e, int fwd) {
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

int le_key(LineEd *e, FmEv *k) {
    int c = k->a;
    char t[4];
    switch (c) {
    case FK_ENTER: return 1;
    case FK_ESC: return 2;
    case FK_BACK: le_del(e, 0); break;
    case FK_DEL: le_del(e, 1); break;
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
            if (c == 'v') {
                char b[400];
                int n = sm_clip_get(b, sizeof b - 1), i, j = 0;
                for (i = 0; i < n; i++) if (b[i] != '\n' && b[i] != '\r') b[j++] = b[i];
                le_ins(e, b, j);
            }
            if (c == 'c' && e->len) sm_clip_set(e->buf, e->len);
            break;
        }
        if (c >= 32 && c < 0x200000) le_ins(e, t, cp_utf8(c, t));
    }
    return 0;
}

void le_draw(LineEd *e, int x, int y, int w, int h, int focus) {
    char tmp[512];
    int tw;
    be_fill(x, y, w, h, c_well);
    frame(x, y, w, h, focus ? c_acc : c_rule);
    if (e->all && e->len) {
        be_fill(x + 6, y + 4, be_text_w(e->buf, F_REG) + 2, h - 8, c_selbg);
    }
    fit(e->buf, w - 14, F_REG, tmp, sizeof tmp);
    txt(x + 7, y, h, tmp, c_ink, F_REG);
    if (focus && !e->all && (now_ms() / 500) % 2 == 0) {
        memcpy(tmp, e->buf, e->pos);
        tmp[e->pos] = 0;
        tw = e->pos ? be_text_w(tmp, F_REG) : 0;
        be_fill(x + 7 + tw, y + 4, 1, h - 8, c_ink);
    }
}

int le_click(LineEd *e, int x, int px) {
    char tmp[512];
    int i = 0;
    e->all = 0;
    for (;;) {
        if (i >= e->len) break;
        int n = u8_len(e->buf + i);
        memcpy(tmp, e->buf, i + n);
        tmp[i + n] = 0;
        if (x + 7 + be_text_w(tmp, F_REG) - 3 > px) break;
        i += n;
    }
    e->pos = i;
    return i;
}

void clip_text(const char *s, int n) { sm_clip_set(s, n); }

void run_bg(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(1);
    }
}

typedef struct { char name[256]; int dir; long long size; } DEnt;
static int dcmp(const void *a, const void *b) {
    const DEnt *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

int ask_yn(const char *title, const char *q) {
    FmEv e;
    int mx = 0, my = 0;
    for (;;) {
        int W = be_w(), H = be_h(), bx = W / 2 - 110, by = H / 2 + 10;
        be_fill(0, 0, W, H, c_bg);
        be_text(W / 2 - be_text_w(title, F_BOLD) / 2, H / 2 - 50, title, c_ink, F_BOLD);
        be_text(W / 2 - be_text_w(q, F_REG) / 2, H / 2 - 20, q, c_dim, F_REG);
        button(bx, by, 100, "Yes", 1, mx, my);
        button(bx + 120, by, 100, "No", 0, mx, my);
        be_flip();
        if (be_wait(&e, 200) <= 0) continue;
        if (e.type == EV_CLOSE) return 0;
        if (e.type == EV_MOVE) { mx = e.a; my = e.b; }
        if (e.type == EV_DOWN && e.c == 1 && e.b >= by && e.b < by + 28) {
            if (e.a >= bx && e.a < bx + 100) return 1;
            if (e.a >= bx + 120 && e.a < bx + 220) return 0;
        }
        if (e.type == EV_KEY) {
            if (e.a == FK_ENTER || e.a == 'y') return 1;
            if (e.a == FK_ESC || e.a == 'n') return 0;
        }
    }
}

// full window file picker. save=1: name field is editable and enter on it accepts
int ask_file(const char *title, char *path, int save) {
    char dir[1024] = "/", name_in[512];
    DEnt *ents = 0;
    int n = 0, cap = 0, sel = -1, scroll = 0, i, mx = 0, my = 0, hid = 0, foc = 0, reload = 1;
    LineEd nm;
    FmEv e;
    char *sl = strrchr(path, '/');
    if (sl && sl != path) { memcpy(dir, path, sl - path); dir[sl - path] = 0; le_set(&nm, sl + 1); }
    else { if (sl == path) strcpy(dir, "/"); else if (getcwd(dir, sizeof dir) == 0) strcpy(dir, "/"); le_set(&nm, sl ? sl + 1 : path); }
    nm.all = 0;
    for (;;) {
        int W = be_w(), H = be_h(), rowh = fh_reg + 10, top = 76, bot = H - 70, vis = (bot - top) / rowh;
        if (reload) {
            DIR *d = opendir(dir);
            struct dirent *de;
            n = 0; sel = -1; scroll = 0; reload = 0;
            if (d) {
                while ((de = readdir(d))) {
                    struct stat st;
                    char p[1300];
                    if (!strcmp(de->d_name, ".") || (de->d_name[0] == '.' && !hid && strcmp(de->d_name, ".."))) continue;
                    if (n == cap) { cap = cap ? cap * 2 : 256; ents = realloc(ents, cap * sizeof *ents); }
                    snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
                    strncpy(ents[n].name, de->d_name, 255);
                    ents[n].name[255] = 0;
                    ents[n].size = 0;
                    ents[n].dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
                    if (!ents[n].dir) ents[n].size = st.st_size;
                    n++;
                }
                closedir(d);
            }
            qsort(ents, n, sizeof *ents, dcmp);
        }
        be_fill(0, 0, W, H, c_bg);
        be_text(14, 10, title, c_ink, F_BOLD);
        {
            char t[300];
            fit(dir, W - 28, F_REG, t, sizeof t);
            be_text(14, 36, t, c_dim, F_REG);
        }
        be_fill(0, top - 4, W, 1, c_rule);
        for (i = 0; i < vis && scroll + i < n; i++) {
            DEnt *d = &ents[scroll + i];
            char t[300], sz[32];
            int y = top + i * rowh;
            if (scroll + i == sel) be_fill(6, y, W - 12, rowh, c_selbg);
            else if (mx > 6 && mx < W - 6 && my >= y && my < y + rowh) be_fill(6, y, W - 12, rowh, c_hover);
            fit(d->name, W - 160, F_REG, t, sizeof t);
            txt(16, y, rowh, d->dir ? "\xE2\x96\xB8" : " ", c_acc, F_REG);
            txt(34, y, rowh, t, d->dir ? c_acc : c_ink, F_REG);
            if (!d->dir) {
                if (d->size < 1024) sprintf(sz, "%lld B", d->size);
                else if (d->size < 1 << 20) sprintf(sz, "%lld KB", d->size >> 10);
                else sprintf(sz, "%lld MB", d->size >> 20);
                txt(W - 100, y, rowh, sz, c_dim, F_SMALL);
            }
        }
        if (n > vis) {
            int sh = (bot - top) * vis / n, sy = top + (bot - top - sh) * scroll / (n - vis);
            be_fill(W - 6, sy, 4, sh, c_rule);
        }
        be_fill(0, bot, W, H - bot, c_bar);
        le_draw(&nm, 14, bot + 10, W - 250, 30, foc);
        button(W - 220, bot + 11, 100, save ? "Save" : "Open", 1, mx, my);
        button(W - 110, bot + 11, 96, "Cancel", 0, mx, my);
        txt(14, bot + 44, 20, hid ? "Ctrl+H: hide dotfiles" : "Ctrl+H: show dotfiles", c_faint, F_SMALL);
        be_flip();
        if (be_wait(&e, 250) <= 0) continue;
        if (e.type == EV_CLOSE) { free(ents); return 0; }
        if (e.type == EV_MOVE) { mx = e.a; my = e.b; }
        if (e.type == EV_WHEEL) {
            scroll += e.a * 3;
            if (scroll > n - vis) scroll = n - vis;
            if (scroll < 0) scroll = 0;
        }
        if (e.type == EV_DOWN && e.c == 1) {
            if (e.b >= bot + 11 && e.b < bot + 39) {
                if (e.a >= W - 220 && e.a < W - 120) goto accept;
                if (e.a >= W - 110) { free(ents); return 0; }
            }
            if (e.b >= bot + 10 && e.b < bot + 40 && e.a < W - 240) { foc = 1; le_click(&nm, 14, e.a); }
            if (e.b >= top && e.b < bot) {
                int k = scroll + (e.b - top) / rowh;
                static int lastk = -1;
                static uint32_t lastt;
                if (k < n) {
                    sel = k;
                    if (!ents[k].dir) { le_set(&nm, ents[k].name); nm.all = 0; }
                    if (lastk == k && now_ms() - lastt < 450) { e.type = EV_KEY; e.a = FK_ENTER; e.mods = 0; foc = 0; }
                    lastk = k; lastt = now_ms();
                }
            }
        }
        if (e.type == EV_KEY) {
            if ((e.mods & MOD_CTRL) && e.a == 'h') { hid = !hid; reload = 1; continue; }
            if (foc) {
                int r = le_key(&nm, &e);
                if (r == 1) goto accept;
                if (r == 2) foc = 0;
                if (e.a == FK_TAB) foc = 0;
                continue;
            }
            if (e.a == FK_ESC) { free(ents); return 0; }
            if (e.a == FK_TAB) { foc = 1; continue; }
            if (e.a == FK_DOWN && sel < n - 1) sel++;
            if (e.a == FK_UP && sel > 0) sel--;
            if (e.a == FK_PGDN) sel = sel + vis < n ? sel + vis : n - 1;
            if (e.a == FK_PGUP) sel = sel - vis > 0 ? sel - vis : 0;
            if (e.a == FK_BACK) { sel = 0; if (n && !strcmp(ents[0].name, "..")) e.a = FK_ENTER; }
            if (sel >= 0 && sel < scroll) scroll = sel;
            if (sel >= scroll + vis) scroll = sel - vis + 1;
            if (e.a == FK_ENTER) {
                if (sel >= 0 && sel < n && ents[sel].dir) {
                    char np[1100];
                    if (!strcmp(ents[sel].name, "..")) {
                        char *s = strrchr(dir, '/');
                        if (s == dir) dir[1] = 0; else if (s) *s = 0;
                    } else {
                        snprintf(np, sizeof np, "%s/%s", strcmp(dir, "/") ? dir : "", ents[sel].name);
                        strcpy(dir, np);
                    }
                    reload = 1;
                } else if (sel >= 0 && sel < n) goto accept;
                else foc = 1;
            }
        }
        continue;
accept:
        if (!nm.len) continue;
        if (nm.buf[0] == '/') strncpy(name_in, nm.buf, 500);
        else snprintf(name_in, sizeof name_in, "%s/%s", strcmp(dir, "/") ? dir : "", nm.buf);
        {
            struct stat st;
            if (!save && (stat(name_in, &st) != 0 || S_ISDIR(st.st_mode))) {
                if (S_ISDIR(st.st_mode)) { strcpy(dir, name_in); reload = 1; le_set(&nm, ""); continue; }
                continue;
            }
            if (save && stat(name_in, &st) == 0 && !ask_yn("Overwrite?", nm.buf)) continue;
        }
        strcpy(path, name_in);
        free(ents);
        return 1;
    }
}

int main(int argc, char **argv) {
    const char *p = strrchr(argv[0], '/');
    const char *me = p ? p + 1 : argv[0];
    if (!strcmp(me, "dapps") && argc > 1) { me = argv[1]; argv++; argc--; }
    if (!strcmp(me, "notify-send-lite")) {
        char b[96] = "";
        if (argc < 2) { printf("usage: notify-send-lite title [body]\n"); return 1; }
        snprintf(b, sizeof b, "%s%s%s", argv[1], argc > 2 ? "\n" : "", argc > 2 ? argv[2] : "");
        sm_notify(b);
        return 0;
    }
    if (!strcmp(me, "screenshot")) { sm_ctl(SM_CTL_SHOT); return 0; }
    if (!strcmp(me, "lock")) { sm_ctl(SM_CTL_LOCK); return 0; }
    th_load();
    if (!strcmp(me, "edit")) return edit_main(argc, argv);
    if (!strcmp(me, "sysmon")) return sysmon_main(argc, argv);
    if (!strcmp(me, "pkg")) return pkg_main(argc, argv);
    if (!strcmp(me, "settings")) return settings_main(argc, argv);
    printf("dapps: edit sysmon pkg settings notify-send-lite screenshot lock\n");
    return 1;
}

int ui_open(int w, int h, const char *title) {
    if (be_open(w, h, title) < 0) { fprintf(stderr, "%s: no desktop\n", title); return -1; }
    fh_reg = be_font_h(F_REG);
    fh_small = be_font_h(F_SMALL);
    fh_mono = be_font_h(F_MONO);
    return 0;
}
