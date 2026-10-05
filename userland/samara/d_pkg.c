#include "dapps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG "/tmp/pkg.log"
#define OUT "/tmp/pkg.out"

typedef struct { char name[64]; char desc[160]; int inst; } Pk;

static Pk *pk;
static int npk, cap, tab, sel, top;
static LineEd q;
static int job, job_kind;      // pid, 1 list job 2 log job
static char status[128] = "";
static int log_top = -1;
static int mx, my;

static void job_start(const char *cmd, int kind, const char *outf) {
    char c[1024];
    if (job) return;
    snprintf(c, sizeof c, "(%s) > %s 2>&1; echo \"[exit $?]\" >> %s", cmd, outf, outf);
    job = fork();
    if (job == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", c, (char *)0);
        _exit(1);
    }
    job_kind = kind;
}

static void add(const char *n, const char *d, int inst) {
    if (npk == cap) { cap = cap ? cap * 2 : 256; pk = realloc(pk, cap * sizeof(Pk)); }
    snprintf(pk[npk].name, sizeof pk[npk].name, "%s", n);
    snprintf(pk[npk].desc, sizeof pk[npk].desc, "%s", d);
    pk[npk].inst = inst;
    npk++;
}

// "[-] name-1.2_1 some description"  or  "name-1.2_1 desc" for -l
static void parse_out(int mode) {
    FILE *f = fopen(OUT, "r");
    char l[512];
    npk = 0; sel = -1; top = 0;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        char *p = l, *e, *d;
        int inst = 0;
        l[strcspn(l, "\n")] = 0;
        if (!l[0] || l[0] == '[' && l[1] == 'e') continue;
        if (p[0] == '[' && p[2] == ']') { inst = p[1] == '*'; p += 4; }
        else if (mode == 2) inst = 1;
        if (mode == 2) { // xbps-query -l: "ii name-ver desc"
            if (p[0] == 'i' && p[1] == 'i' && p[2] == ' ') p += 3;
        }
        e = p + strcspn(p, " ");
        d = *e ? e + 1 : e;
        *e = 0;
        while (*d == ' ') d++;
        add(p, d, inst);
    }
    fclose(f);
}

static void do_search(void) {
    char c[300];
    if (!q.len) return;
    // no quoting trouble: strip anything weird
    {
        int i, j = 0;
        char t[256];
        for (i = 0; q.buf[i] && j < 200; i++) if (q.buf[i] > ' ' && !strchr("\"'`$\\;&|<>()", q.buf[i])) t[j++] = q.buf[i];
        t[j] = 0;
        snprintf(c, sizeof c, "xbps-query -Rs %s", t);
    }
    unlink(OUT);
    job_start(c, 1, OUT);
    snprintf(status, sizeof status, "searching...");
}

static void do_installed(void) {
    unlink(OUT);
    job_start("xbps-query -l", 3, OUT);
    snprintf(status, sizeof status, "reading db...");
}

static void do_act(const char *verb, const char *pkg) {
    char c[300];
    if (job) { snprintf(status, sizeof status, "busy"); return; }
    if (!strcmp(verb, "update")) snprintf(c, sizeof c, "xbps-install -Syu");
    else if (!strcmp(verb, "install")) snprintf(c, sizeof c, "xbps-install -Sy %s", pkg);
    else snprintf(c, sizeof c, "xbps-remove -Ry %s", pkg);
    unlink(LOG);
    job_start(c, 2, LOG);
    log_top = -1;
    snprintf(status, sizeof status, "%s %s...", verb, pkg ? pkg : "");
}

static void strip_ver(const char *in, char *o) {
    char *d;
    strcpy(o, in);
    d = strrchr(o, '-');
    if (d) *d = 0;
}

static char logb[1 << 16];
static int nlog;

static int log_lines(char **ls, int max) {
    FILE *f = fopen(LOG, "r");
    int n = 0, i;
    nlog = 0;
    if (f) { nlog = fread(logb, 1, sizeof logb - 1, f); fclose(f); }
    logb[nlog] = 0;
    // ncurses-ish progress uses \r, just treat as newline
    for (i = 0; i <= nlog && n < max; ) {
        ls[n++] = logb + i;
        while (i < nlog && logb[i] != '\n' && logb[i] != '\r') i++;
        if (i >= nlog) break;
        logb[i++] = 0;
    }
    return n;
}

static void draw(void) {
    int W = be_w(), H = be_h(), i, rh = fh_reg + 10, y;
    const char *tn[] = { "Search", "Installed", "Update" };
    char s[300];
    be_fill(0, 0, W, H, c_bg);
    be_fill(0, 0, W, 34, c_bar);
    for (i = 0; i < 3; i++) {
        int tw = 100;
        if (i == tab) { be_fill(i * tw + 6, 4, tw - 4, 30, c_bg); be_fill(i * tw + 6, 4, tw - 4, 2, c_acc); }
        txt(i * tw + 6 + (tw - 4 - be_text_w(tn[i], F_REG)) / 2, 4, 30, tn[i], i == tab ? c_ink : c_dim, F_REG);
    }
    y = 44;
    if (tab == 0) {
        le_draw(&q, 12, y, W - 200, 30, 1);
        button(W - 176, y, 70, "Search", 1, mx, my);
        if (!q.len) txt(20, y, 30, "xbps package name...", c_faint, F_REG);
        y += 40;
    }
    if (tab == 2) {
        char *ls[400];
        int n, vis, base;
        button(12, y, 160, job ? "Working..." : "Update system", 1, mx, my);
        y += 40;
        n = log_lines(ls, 400);
        vis = (H - y - 30) / (fh_mono + 2);
        base = n > vis ? n - vis : 0;
        be_fill(12, y, W - 24, H - y - 30, c_well);
        for (i = 0; i < vis && base + i < n; i++) {
            fit(ls[base + i], W - 40, F_MONO, s, sizeof s);
            txt(20, y + 2 + i * (fh_mono + 2), fh_mono, s, c_ink, F_MONO);
        }
        goto bottom;
    }
    // list
    {
        int vis = (H - y - 30 - (job_kind == 2 || log_top >= 0 ? 130 : 0)) / rh;
        if (top > npk - vis) top = npk - vis;
        if (top < 0) top = 0;
        for (i = 0; i < vis && top + i < npk; i++) {
            Pk *p = &pk[top + i];
            int yy = y + i * rh;
            if (top + i == sel) be_fill(0, yy, W, rh, c_selbg);
            else if (my >= yy && my < yy + rh && mx < W - 150) be_fill(0, yy, W, rh, c_hover);
            txt(14, yy, rh, p->name, p->inst ? c_ok : c_ink, F_BOLD);
            fit(p->desc, W - 520, F_REG, s, sizeof s);
            txt(260, yy, rh, s, c_dim, F_REG);
            if (top + i == sel) button(W - 110, yy + 1, 90, p->inst ? "Remove" : "Install", p->inst ? 0 : 1, mx, my);
        }
        if (!npk && !job) txt(14, y + 4, rh, tab ? "nothing here" : "type a name and press Enter", c_faint, F_REG);
        if (job_kind == 2 || log_top >= 0) {
            char *ls[400];
            int n = log_lines(ls, 400), v = 130 / (fh_mono + 2) - 1, base = n > v ? n - v : 0;
            be_fill(12, H - 30 - 124, W - 24, 124, c_well);
            for (i = 0; i < v && base + i < n; i++) {
                fit(ls[base + i], W - 40, F_MONO, s, sizeof s);
                txt(20, H - 30 - 120 + i * (fh_mono + 2), fh_mono, s, c_ink, F_MONO);
            }
        }
    }
bottom:
    be_fill(0, H - 24, W, 24, c_bar);
    txt(10, H - 24, 24, status, job ? c_acc : c_dim, F_SMALL);
    be_flip();
}

static void tab_set(int t) {
    tab = t; npk = 0; sel = -1; top = 0;
    if (t == 1) do_installed();
}

static void finish(void) {
    if (job_kind == 1) { parse_out(1); snprintf(status, sizeof status, "%d found", npk); }
    else if (job_kind == 3) { parse_out(2); snprintf(status, sizeof status, "%d installed", npk); }
    else {
        char *ls[400];
        int n = log_lines(ls, 400);
        snprintf(status, sizeof status, "%s", n ? ls[n - 1] : "done");
        if (!status[0] && n > 1) snprintf(status, sizeof status, "%s", ls[n - 2]);
    }
}

int pkg_main(int argc, char **argv) {
    FmEv e;
    if (ui_open(900, 600, "pkg") < 0) return 1;
    le_set(&q, argc > 1 ? argv[1] : "");
    for (;;) {
        int r;
        if (job) {
            int st;
            if (waitpid(job, &st, WNOHANG) > 0) { job = 0; finish(); if (job_kind == 2) job_kind = 2; }
        }
        draw();
        r = be_wait(&e, 300);
        if (r < 0) break;
        if (r == 0) continue;
        do {
            int W = be_w(), H = be_h();
            switch (e.type) {
            case EV_CLOSE: goto out;
            case EV_MOVE: mx = e.a; my = e.b; break;
            case EV_WHEEL: top += e.a * 3; break;
            case EV_DOWN: {
                int i, rh = fh_reg + 10, y0 = tab == 0 ? 84 : 44;
                mx = e.a; my = e.b;
                if (e.b < 34) { i = e.a / 100; if (i < 3 && e.a > 6) tab_set(i); break; }
                if (tab == 0 && e.b >= 44 && e.b < 74) {
                    if (e.a >= W - 176 && e.a < W - 106) do_search();
                    else le_click(&q, 12, e.a);
                    break;
                }
                if (tab == 2) {
                    if (e.a >= 12 && e.a < 172 && e.b >= 44 && e.b < 74) do_act("update", 0);
                    break;
                }
                i = top + (e.b - y0) / rh;
                if (e.b >= y0 && i < npk) {
                    if (i == sel && e.a >= W - 110 && e.a < W - 20) {
                        char n[64];
                        strip_ver(pk[i].name, n);
                        do_act(pk[i].inst ? "remove" : "install", n);
                    }
                    sel = i;
                }
                break;
            }
            case EV_KEY:
                if (tab == 0) {
                    int k = le_key(&q, &e);
                    if (k == 1) do_search();
                }
                if (e.a == FK_ESC && !(tab == 0 && q.len)) goto out;
                break;
            }
            (void)H;
        } while (be_wait(&e, 0) > 0);
    }
out:
    be_close();
    return 0;
}
