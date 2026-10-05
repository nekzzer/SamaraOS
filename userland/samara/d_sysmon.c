#include "dapps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <sys/statvfs.h>

#define HIST 120
#define MAXP 256

typedef struct { int pid; char name[32], st; unsigned long t, pt; long rss; float cpu; } Proc;

static Proc pr[MAXP];
static int np, sortby = 1, sel = -1, top, selpid;
static float cpuh[16][HIST], rxh[HIST], txh[HIST];
static int ncpu;
static unsigned long pj[17][2], prx, ptx;
static long mt, mf, st_, sf, mc;
static char msg[64];
static uint32_t msg_until;

static const char *sn[] = { "PID", "CPU", "MEM", "Name" };

static void push(float *h, float v) { memmove(h, h + 1, (HIST - 1) * sizeof(float)); h[HIST - 1] = v; }

static void rd_cpu(void) {
    FILE *f = fopen("/proc/stat", "r");
    char l[256];
    if (!f) return;
    while (fgets(l, sizeof l, f) && !strncmp(l, "cpu", 3)) {
        unsigned long v[8] = {0}, tot = 0, idle;
        int i, n;
        char *p = l + 3;
        if (*p == ' ') n = 16; else n = atoi(p);
        while (*p && *p != ' ') p++;
        sscanf(p, "%lu %lu %lu %lu %lu %lu %lu %lu", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7);
        for (i = 0; i < 8; i++) tot += v[i];
        idle = v[3] + v[4];
        {
            int slot = n;
            if (l[3] == ' ') slot = 16;
            else if (slot >= 16) continue;
            if (slot < 16 && slot + 1 > ncpu) ncpu = slot + 1;
            if (pj[slot][0] && tot > pj[slot][0]) {
                float u = 1.0f - (float)(idle - pj[slot][1]) / (float)(tot - pj[slot][0]);
                if (u < 0) u = 0;
                if (slot < 16) push(cpuh[slot], u);
            }
            pj[slot][0] = tot; pj[slot][1] = idle;
        }
    }
    fclose(f);
}

static void rd_mem(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    long v;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        if (sscanf(l, "MemTotal: %ld", &v) == 1) mt = v;
        else if (sscanf(l, "MemAvailable: %ld", &v) == 1) mf = v;
        else if (sscanf(l, "SwapTotal: %ld", &v) == 1) st_ = v;
        else if (sscanf(l, "SwapFree: %ld", &v) == 1) sf = v;
        else if (sscanf(l, "Cached: %ld", &v) == 1) mc = v;
    }
    fclose(f);
}

static void rd_net(float *rx, float *tx) {
    FILE *f = fopen("/proc/net/dev", "r");
    char l[256];
    unsigned long r = 0, t = 0;
    *rx = *tx = 0;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        char *c = strchr(l, ':');
        unsigned long a, b, x[8];
        if (!c || strstr(l, "lo:")) continue;
        if (sscanf(c + 1, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &a, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7, &b) == 10) { r += a; t += b; }
    }
    fclose(f);
    if (prx) { *rx = r - prx; *tx = t - ptx; }
    prx = r; ptx = t;
}

static unsigned long now_t;
static unsigned long tot_prev;

static void rd_procs(unsigned long dtot) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    Proc old[MAXP];
    int on = np, i;
    memcpy(old, pr, sizeof(Proc) * np);
    np = 0;
    if (!d) return;
    while ((e = readdir(d)) && np < MAXP) {
        char p[64], b[512];
        FILE *f;
        Proc *q = &pr[np];
        unsigned long ut, stt;
        int pid = atoi(e->d_name), ppid;
        char *c1, *c2;
        if (pid <= 0) continue;
        snprintf(p, sizeof p, "/proc/%d/stat", pid);
        f = fopen(p, "r");
        if (!f) continue;
        i = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        b[i > 0 ? i : 0] = 0;
        c1 = strchr(b, '('); c2 = strrchr(b, ')');
        if (!c1 || !c2) continue;
        memset(q, 0, sizeof *q);
        q->pid = pid;
        *c2 = 0;
        snprintf(q->name, sizeof q->name, "%s", c1 + 1);
        q->st = c2[2];
        if (sscanf(c2 + 4, "%d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ppid, &ut, &stt) < 3) { ut = stt = 0; }
        q->t = ut + stt;
        for (i = 0; i < on; i++) if (old[i].pid == pid) {
            if (dtot) q->cpu = (float)(q->t - old[i].t) * 100.0f / dtot;
            break;
        }
        snprintf(p, sizeof p, "/proc/%d/status", pid);
        f = fopen(p, "r");
        if (f) {
            char l[128];
            while (fgets(l, sizeof l, f)) if (sscanf(l, "VmRSS: %ld", &q->rss) == 1) break;
            fclose(f);
        }
        np++;
    }
    closedir(d);
}

static int cmp(const void *a, const void *b) {
    const Proc *x = a, *y = b;
    switch (sortby) {
    case 0: return x->pid - y->pid;
    case 1: return y->cpu > x->cpu ? 1 : y->cpu < x->cpu ? -1 : y->rss > x->rss ? 1 : -1;
    case 2: return y->rss > x->rss ? 1 : y->rss < x->rss ? -1 : 0;
    }
    return strcasecmp(x->name, y->name);
}

static void graph(int x, int y, int w, int h, float *v, int n, float mx, uint32_t col) {
    int i;
    be_fill(x, y, w, h, c_well);
    for (i = 0; i < n; i++) {
        float f = v[i] / mx;
        int bh, bx = x + i * w / n, bw = (i + 1) * w / n - i * w / n;
        if (f > 1) f = 1;
        bh = f * (h - 2);
        if (bh < 1 && v[i] > 0) bh = 1;
        if (bh) be_fill(bx, y + h - 1 - bh, bw ? bw : 1, bh, col);
    }
    frame(x, y, w, h, c_rule);
}

static void bar(int x, int y, int w, int h, float f, uint32_t col) {
    be_fill(x, y, w, h, c_well);
    if (f > 1) f = 1;
    be_fill(x, y, (int)(w * f), h, col);
    frame(x, y, w, h, c_rule);
}

static void hsize(char *o, double kb) {
    if (kb >= 1048576) sprintf(o, "%.1f GB", kb / 1048576);
    else if (kb >= 1024) sprintf(o, "%.1f MB", kb / 1024);
    else sprintf(o, "%.0f KB", kb);
}

static void rate(char *o, float b) {
    if (b >= 1048576) sprintf(o, "%.1f MB/s", b / 1048576);
    else if (b >= 1024) sprintf(o, "%.1f KB/s", b / 1024);
    else sprintf(o, "%.0f B/s", b);
}

static int mx, my, list_y;

static void draw(float cpu_tot) {
    int W = be_w(), H = be_h(), i, y, rh = fh_reg + 8, rend = 0;
    int lw = W * 3 / 5 - 12, rx = lw + 20, rw = W - rx - 12;
    char s[160], a[32], b[32];
    int cols = ncpu > 4 ? 4 : ncpu ? ncpu : 1, rows = (ncpu + cols - 1) / cols, gh = 56, cw_ = (lw - (cols - 1) * 8) / cols;
    be_fill(0, 0, W, H, c_bg);
    txt(12, 8, fh_reg, "CPU", c_dim, F_BOLD);
    sprintf(s, "%.0f%%", cpu_tot * 100);
    txt(12 + 50, 8, fh_reg, s, c_ink, F_REG);
    for (i = 0; i < ncpu; i++) {
        int gx = 12 + (i % cols) * (cw_ + 8), gy = 8 + fh_reg + 8 + (i / cols) * (gh + 6);
        graph(gx, gy, cw_, gh, cpuh[i], HIST, 1.0f, c_acc);
        sprintf(s, "cpu%d %.0f%%", i, cpuh[i][HIST - 1] * 100);
        txt(gx + 4, gy + 1, fh_small, s, c_ink, F_SMALL);
    }
    y = 8 + fh_reg + 8 + rows * (gh + 6) + 4;
    // right column
    txt(rx, 8, fh_reg, "Memory", c_dim, F_BOLD);
    hsize(a, mt - mf); hsize(b, mt);
    sprintf(s, "%s / %s", a, b);
    txt(rx + rw - be_text_w(s, F_REG), 8, fh_reg, s, c_ink, F_REG);
    bar(rx, 8 + fh_reg + 6, rw, 14, mt ? (float)(mt - mf) / mt : 0, c_acc);
    txt(rx, 8 + fh_reg + 26, fh_reg, "Swap", c_dim, F_BOLD);
    if (st_) { hsize(a, st_ - sf); hsize(b, st_); sprintf(s, "%s / %s", a, b); }
    else strcpy(s, "off");
    txt(rx + rw - be_text_w(s, F_REG), 8 + fh_reg + 26, fh_reg, s, c_ink, F_REG);
    bar(rx, 8 + 2 * fh_reg + 32, rw, 14, st_ ? (float)(st_ - sf) / st_ : 0, c_danger);
    {
        int ny = 8 + 2 * fh_reg + 56;
        float mxr = 4096;
        for (i = 0; i < HIST; i++) { if (rxh[i] > mxr) mxr = rxh[i]; if (txh[i] > mxr) mxr = txh[i]; }
        txt(rx, ny, fh_reg, "Network", c_dim, F_BOLD);
        rate(a, rxh[HIST - 1]); rate(b, txh[HIST - 1]);
        sprintf(s, "rx %s   tx %s", a, b);
        txt(rx + rw - be_text_w(s, F_REG), ny, fh_reg, s, c_ink, F_REG);
        graph(rx, ny + fh_reg + 6, rw, 36, rxh, HIST, mxr, c_ok);
        graph(rx, ny + fh_reg + 46, rw, 36, txh, HIST, mxr, c_acc);
        // disks
        {
            FILE *f = fopen("/proc/mounts", "r");
            char l[256], dev[64], mp[128], ty[32];
            int dy = ny + fh_reg + 94;
            txt(rx, dy, fh_reg, "Disks", c_dim, F_BOLD);
            dy += fh_reg + 6;
            while (f && fgets(l, sizeof l, f)) {
                struct statvfs sv;
                double tot, used;
                if (sscanf(l, "%63s %127s %31s", dev, mp, ty) < 3) continue;
                if (strncmp(dev, "/dev/", 5) || !strcmp(ty, "devtmpfs") || !strcmp(ty, "devpts")) continue;
                if (statvfs(mp, &sv) < 0 || !sv.f_blocks) continue;
                tot = (double)sv.f_blocks * sv.f_frsize / 1024;
                used = tot - (double)sv.f_bfree * sv.f_frsize / 1024;
                hsize(a, used); hsize(b, tot);
                sprintf(s, "%s  %s / %s", mp, a, b);
                txt(rx, dy, fh_reg, s, c_ink, F_REG);
                bar(rx, dy + fh_reg + 3, rw, 8, used / tot, c_acc);
                dy += fh_reg + 18;
                if (dy > H - 200) break;
            }
            if (f) fclose(f);
            rend = dy;
        }
    }
    // process list
    {
        int ly = y + 4 > rend + 6 ? y + 4 : rend + 6, hx[5] = { 12, 84, 164, 244, 0 };
        int vis, lines;
        list_y = ly;
        be_fill(0, ly, W, rh, c_bar);
        for (i = 0; i < 4; i++) {
            char h[16];
            sprintf(h, "%s%s", sn[i], i == sortby ? " v" : "");
            txt(hx[i], ly, rh, h, i == sortby ? c_ink : c_dim, F_BOLD);
        }
        txt(hx[4] ? hx[4] : W - 120, ly, rh, "State", c_dim, F_BOLD);
        ly += rh;
        vis = (H - ly - 26) / rh;
        lines = np < vis ? np : vis;
        if (top > np - vis) top = np - vis;
        if (top < 0) top = 0;
        for (i = 0; i < lines && top + i < np; i++) {
            Proc *q = &pr[top + i];
            int yy = ly + i * rh;
            if (q->pid == selpid) be_fill(0, yy, W, rh, c_selbg);
            else if (my >= yy && my < yy + rh && mx < W) be_fill(0, yy, W, rh, c_hover);
            sprintf(s, "%d", q->pid); txt(hx[0], yy, rh, s, c_dim, F_REG);
            sprintf(s, "%.1f%%", q->cpu); txt(hx[1], yy, rh, s, q->cpu > 50 ? c_danger : c_ink, F_REG);
            hsize(s, q->rss); txt(hx[2], yy, rh, s, c_ink, F_REG);
            txt(hx[3], yy, rh, q->name, c_ink, F_REG);
            s[0] = q->st; s[1] = 0; txt(W - 120, yy, rh, s, c_dim, F_REG);
        }
    }
    be_fill(0, H - 24, W, 24, c_bar);
    if (now_ms() < msg_until) txt(10, H - 24, 24, msg, c_acc, F_SMALL);
    else txt(10, H - 24, 24, "click header: sort   K / Del: kill (Shift: SIGKILL)   Q: quit", c_dim, F_SMALL);
    sprintf(s, "%d procs", np);
    txt(W - 10 - be_text_w(s, F_SMALL), H - 24, 24, s, c_dim, F_SMALL);
    be_flip();
}

static void kill_sel(int sig) {
    if (selpid <= 1) return;
    if (kill(selpid, sig) < 0) snprintf(msg, sizeof msg, "can't kill %d", selpid);
    else snprintf(msg, sizeof msg, "sent %s to %d", sig == 9 ? "KILL" : "TERM", selpid);
    msg_until = now_ms() + 3000;
}

int sysmon_main(int argc, char **argv) {
    FmEv e;
    uint32_t next = 0;
    float ct = 0;
    int W, ly;
    if (ui_open(980, 640, "sysmon") < 0) return 1;
    for (;;) {
        int r;
        uint32_t t = now_ms();
        if (t >= next) {
            float rx, tx;
            unsigned long k = pj[16][0];
            rd_cpu();
            rd_mem();
            rd_net(&rx, &tx);
            push(rxh, rx); push(txh, tx);
            // total cpu = avg of cores
            {
                int i;
                float s = 0;
                for (i = 0; i < ncpu; i++) s += cpuh[i][HIST - 1];
                ct = ncpu ? s / ncpu : 0;
            }
            // proc deltas: per-core ticks sum
            rd_procs(pj[16][0] > k && k ? (pj[16][0] - k) / (ncpu ? ncpu : 1) : 0);
            qsort(pr, np, sizeof(Proc), cmp);
            next = t + 1000;
        }
        draw(ct);
        r = be_wait(&e, 200);
        if (r < 0) break;
        if (r == 0) continue;
        do {
            W = be_w();
            switch (e.type) {
            case EV_CLOSE: goto out;
            case EV_MOVE: mx = e.a; my = e.b; break;
            case EV_WHEEL: top += e.a * 3; break;
            case EV_DOWN: {
                int rh = fh_reg + 8;
                ly = list_y;
                mx = e.a; my = e.b;
                if (e.b >= ly && e.b < ly + rh) {
                    int hx[4] = { 12, 84, 164, 244 }, i;
                    for (i = 3; i >= 0; i--) if (e.a >= hx[i] - 4) { sortby = i; qsort(pr, np, sizeof(Proc), cmp); break; }
                } else if (e.b >= ly + rh && e.b < be_h() - 24) {
                    int i = top + (e.b - ly - rh) / rh;
                    selpid = i < np ? pr[i].pid : 0;
                }
                break;
            }
            case EV_KEY:
                if (e.a == 'q' || e.a == 'Q' && !(e.mods & MOD_SHIFT)) goto out;
                if (e.a == 'k' || e.a == 'K' || e.a == FK_DEL) kill_sel(e.mods & MOD_SHIFT ? 9 : 15);
                if (e.a == FK_UP || e.a == FK_DOWN) {
                    int i, d = e.a == FK_UP ? -1 : 1;
                    for (i = 0; i < np; i++) if (pr[i].pid == selpid) break;
                    i += d;
                    if (i < 0) i = 0;
                    if (i < np) selpid = pr[i].pid;
                    if (i < top) top = i;
                }
                break;
            }
        } while (be_wait(&e, 0) > 0);
        (void)W;
    }
out:
    be_close();
    return 0;
}
