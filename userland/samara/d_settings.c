#include "samara.h"
#include "dapps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CONF "/etc/samara-desktop.conf"

uint32_t *img_decode(const uint8_t *d, int n, int *w, int *h);

static char conf[1024];
static int mx, my;
static char msg[96];

static const char *get(const char *k, const char *def) {
    static char v[64];
    char *p = conf;
    int kl = strlen(k);
    while (*p) {
        if (!strncmp(p, k, kl) && p[kl] == '=') {
            int n = strcspn(p + kl + 1, "\n");
            if (n > 63) n = 63;
            memcpy(v, p + kl + 1, n);
            v[n] = 0;
            return v;
        }
        p += strcspn(p, "\n");
        if (*p) p++;
    }
    return def;
}

static void conf_read(void) {
    FILE *f = fopen(CONF, "r");
    int n = 0;
    if (f) { n = fread(conf, 1, sizeof conf - 1, f); fclose(f); }
    conf[n] = 0;
}

static void set(const char *k, const char *v) {
    char out[1024] = "", *p = conf;
    int kl = strlen(k), done = 0;
    FILE *f;
    while (*p) {
        int n = strcspn(p, "\n");
        if (!strncmp(p, k, kl) && p[kl] == '=') {
            if (!done) { strcat(out, k); strcat(out, "="); strcat(out, v); strcat(out, "\n"); done = 1; }
        } else if (n) {
            strncat(out, p, n);
            strcat(out, "\n");
        }
        p += n;
        if (*p) p++;
    }
    if (!done) { strcat(out, k); strcat(out, "="); strcat(out, v); strcat(out, "\n"); }
    strcpy(conf, out);
    f = fopen(CONF, "w");
    if (f) { fputs(conf, f); fclose(f); }
    th_load();
    sm_ctl(SM_CTL_RELOAD);
}

static void put32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }

// decode any image we know, scale down to the screen, dump as 24bpp bmp
static int set_wall(const char *path) {
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    uint32_t *px;
    int n, w, h, sw, sh, x, y, sc, ow, oh;
    FILE *o;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(n);
    if (fread(d, 1, n, f) != n) { fclose(f); free(d); return -1; }
    fclose(f);
    if (d[0] == 'B' && d[1] == 'M') {      // already bmp, just copy
        o = fopen("/root/.wallpaper.bmp", "wb");
        if (!o) { free(d); return -1; }
        fwrite(d, 1, n, o);
        fclose(o);
        free(d);
        return 0;
    }
    px = img_decode(d, n, &w, &h);
    free(d);
    if (!px) return -2;
    sc = sm_screen();
    sw = sc >> 16; sh = sc & 0xFFFF;
    ow = w; oh = h;
    if (ow > sw) { oh = (long long)oh * sw / ow; ow = sw; }
    if (oh > sh) { ow = (long long)ow * sh / oh; oh = sh; }
    mkdir("/root", 0755);
    o = fopen("/root/.wallpaper.bmp", "wb");
    if (!o) { free(px); return -1; }
    fputs("BM", o);
    put32(o, 54 + ((ow * 3 + 3) & ~3) * oh);
    put32(o, 0); put32(o, 54); put32(o, 40);
    put32(o, ow); put32(o, oh);
    put32(o, 1 | 24 << 16); put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0);
    for (y = oh - 1; y >= 0; y--) {
        int pad = ((ow * 3 + 3) & ~3) - ow * 3;
        for (x = 0; x < ow; x++) {
            uint32_t c = px[(long)(y * h / oh) * w + x * w / ow];
            fputc(c & 255, o); fputc((c >> 8) & 255, o); fputc((c >> 16) & 255, o);
        }
        while (pad--) fputc(0, o);
    }
    fclose(o);
    free(px);
    return 0;
}

typedef struct { int x, y, w; } Hit;
static Hit hits[40];
static int nh;

static int opt(int x, int y, int w, const char *l, int on) {
    hits[nh].x = x; hits[nh].y = y; hits[nh].w = w;
    button(x, y, w, l, on, mx, my);
    return nh++;
}

static void draw(void) {
    int W = be_w(), H = be_h(), y = 20, x0 = 190, i;
    char s[64];
    const char *th = get("theme", "dark"), *ly = get("layout", "us"), *hk = get("hotkey", "alt"), *lk = get("lock", "10");
    const char *rs = get("res", "");
    nh = 0;
    be_fill(0, 0, W, H, c_bg);

    txt(24, y, 30, "Theme", c_ink, F_BOLD);
    opt(x0, y, 90, "Dark", !strcmp(th, "dark"));
    opt(x0 + 100, y, 90, "Light", !strcmp(th, "light"));
    y += 52;
    txt(24, y, 30, "Layout", c_ink, F_BOLD);
    opt(x0, y, 90, "US", !strcmp(ly, "us"));
    opt(x0 + 100, y, 90, "RU", !strcmp(ly, "ru"));
    y += 40;
    txt(24, y, 30, "Switch with", c_ink, F_BOLD);
    opt(x0, y, 120, "Alt+Shift", !strcmp(hk, "alt"));
    opt(x0 + 130, y, 120, "Ctrl+Shift", !strcmp(hk, "ctrl"));
    y += 52;
    txt(24, y, 30, "Wallpaper", c_ink, F_BOLD);
    opt(x0, y, 130, "Choose image...", 0);
    opt(x0 + 140, y, 90, "Default", 0);
    y += 52;
    txt(24, y, 30, "Auto lock", c_ink, F_BOLD);
    opt(x0, y, 40, "-", 0);
    sprintf(s, atoi(lk) ? "%s min" : "off", lk);
    txt(x0 + 52, y, 30, s, c_ink, F_REG);
    opt(x0 + 130, y, 40, "+", 0);
    y += 52;
    txt(24, y, 30, "Resolution", c_ink, F_BOLD);
    {
        static const char *rl[] = { "1024x768", "1280x720", "1600x900", "1920x1080" };
        for (i = 0; i < 4; i++) opt(x0 + i * 110, y, 100, rl[i], !strcmp(rs, rl[i]));
    }
    txt(x0, y + 34, 24, "applies on next boot", c_faint, F_SMALL);
    y += 70;
    txt(24, y, 30, "Window size", c_ink, F_BOLD);
    opt(x0, y, 90, "100%", ui_scale == 100);
    opt(x0 + 100, y, 90, "125%", ui_scale == 125);
    opt(x0 + 200, y, 90, "150%", ui_scale == 150);
    txt(x0 + 300, y, 30, "for newly opened apps", c_faint, F_SMALL);
    be_fill(0, H - 24, W, 24, c_bar);
    txt(10, H - 24, 24, msg, c_acc, F_SMALL);
    be_flip();
}

static void click(int id) {
    char v[16];
    int m;
    switch (id) {
    case 0: set("theme", "dark"); break;
    case 1: set("theme", "light"); break;
    case 2: set("layout", "us"); break;
    case 3: set("layout", "ru"); break;
    case 4: set("hotkey", "alt"); break;
    case 5: set("hotkey", "ctrl"); break;
    case 6: {
        char p[1024] = "/root/";
        if (ask_file("Pick image", p, 0)) {
            int r = set_wall(p);
            strcpy(msg, r == 0 ? "wallpaper set" : r == -1 ? "can't read/write file" : "unknown image format");
            sm_ctl(SM_CTL_RELOAD);
        }
        break;
    }
    case 7: unlink("/root/.wallpaper.bmp"); sm_ctl(SM_CTL_RELOAD); strcpy(msg, "default wallpaper"); break;
    case 8: case 9:
        m = atoi(get("lock", "10"));
        m += id == 8 ? (m > 5 ? -5 : -1) : (m >= 5 ? 5 : 1);
        if (m < 0) m = 0;
        sprintf(v, "%d", m);
        set("lock", v);
        break;
    case 10: set("res", "1024x768"); strcpy(msg, "resolution saved, reboot"); break;
    case 11: set("res", "1280x720"); strcpy(msg, "resolution saved, reboot"); break;
    case 12: set("res", "1600x900"); strcpy(msg, "resolution saved, reboot"); break;
    case 13: set("res", "1920x1080"); strcpy(msg, "resolution saved, reboot"); break;
    case 14: case 15: case 16:
        ui_scale = 100 + (id - 14) * 25;
        sprintf(v, "%d", ui_scale);
        set("scale", v);
        break;
    }
}

int settings_main(int argc, char **argv) {
    FmEv e;
    conf_read();
    if (ui_open(640, 460, "settings") < 0) return 1;
    for (;;) {
        int r;
        draw();
        r = be_wait(&e, 1000);
        if (r < 0) break;
        if (r == 0) continue;
        do {
            if (e.type == EV_CLOSE) goto out;
            if (e.type == EV_MOVE) { mx = e.a; my = e.b; }
            if (e.type == EV_KEY && e.a == FK_ESC) goto out;
            if (e.type == EV_DOWN) {
                int i;
                mx = e.a; my = e.b;
                for (i = 0; i < nh; i++)
                    if (e.a >= hits[i].x && e.a < hits[i].x + hits[i].w && e.b >= hits[i].y && e.b < hits[i].y + 30) { msg[0] = 0; click(i); break; }
            }
        } while (be_wait(&e, 0) > 0);
    }
out:
    be_close();
    return 0;
}
