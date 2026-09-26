/* samarafetch - SamaraOS logo + system info in 24-bit colour gradients.
 *
 *   samarafetch          logo and info (shown at ssh/telnet login, /etc/profile)
 *   samarafetch --ps1    a gradient PS1 for busybox sh (/etc/shrc)
 *
 * The SamaraOS console is CP866 (TERM=linux); anything else - an ssh or
 * telnet client - gets UTF-8. Both show true colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

static int utf8;

typedef struct { int r, g, b; } rgb_t;

static rgb_t at(const rgb_t *st, int k, int i, int n) {
    if (n <= 1 || k == 1) return st[0];
    int pos = i * (k - 1) * 1000 / (n - 1), seg = pos / 1000, t = pos % 1000;
    if (seg >= k - 1) return st[k - 1];
    rgb_t a = st[seg], b = st[seg + 1];
    rgb_t c = { a.r + (b.r - a.r) * t / 1000, a.g + (b.g - a.g) * t / 1000, a.b + (b.b - a.b) * t / 1000 };
    return c;
}

static void fg(rgb_t c) { printf("\033[38;2;%d;%d;%dm", c.r, c.g, c.b); }
static void reset(void) { fputs("\033[0m", stdout); }

/* one CP866 half-block byte as the terminal wants it */
static void block(unsigned char c) {
    if (!utf8) { putchar(c); return; }
    fputs(c == 0xDB ? "\xe2\x96\x88" : c == 0xDF ? "\xe2\x96\x80" : c == 0xDC ? "\xe2\x96\x84" : " ", stdout);
}

static const char *const logo[] = {
    "\xdc\xdf\xdf\xdf \xdc\xdf\xdf\xdc \xdb\xdc \xdc\xdb \xdc\xdf\xdf\xdc \xdb\xdf\xdf\xdc \xdc\xdf\xdf\xdc    \xdc\xdf\xdf\xdc \xdc\xdf\xdf\xdf",
    " \xdf\xdf\xdc \xdb\xdf\xdf\xdb \xdb \xdf \xdb \xdb\xdf\xdf\xdb \xdb\xdf\xdb  \xdb\xdf\xdf\xdb    \xdb  \xdb  \xdf\xdf\xdc",
    "\xdf\xdf\xdf  \xdf  \xdf \xdf   \xdf \xdf  \xdf \xdf  \xdf \xdf  \xdf     \xdf\xdf  \xdf\xdf\xdf",
};
static const rgb_t RAINBOW[] = { {0xFF,0x5E,0x8A}, {0xFF,0x8A,0x5B}, {0xFF,0xC8,0x57}, {0x9B,0xE3,0x8B}, {0x5C,0xC8,0xF0} };
static const rgb_t SUNSET[]  = { {0xFF,0x6A,0x88}, {0xFF,0x9A,0x5A}, {0xFF,0xD3,0x6E} };
static const rgb_t KEY = {0xFF,0x9A,0x5A}, DIM = {0x7A,0x7F,0x8C}, VAL = {0xE8,0xE6,0xE3};

static void grad_text(const char *s, const rgb_t *st, int k) {
    int n = (int)strlen(s);
    for (int i = 0; i < n; i++) { fg(at(st, k, i, n)); putchar(s[i]); }
}

static int read_first(const char *path, char *buf, int cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n < 0 ? 0 : n] = 0;
    return n > 0;
}

static long meminfo(const char *key) {
    char buf[4096], *p;
    if (!read_first("/proc/meminfo", buf, sizeof buf) || !(p = strstr(buf, key))) return -1;
    return strtol(p + strlen(key), NULL, 10);
}

static void row(const char *key, const char *val) {
    fputs("  ", stdout);
    fg(KEY); printf("%-9s", key);
    fg(VAL); fputs(val, stdout);
    reset(); putchar('\n');
}

static void ps1(void) {
    const char *user = getenv("USER");
    char host[64] = "samara";
    gethostname(host, sizeof host);
    char who[128];
    snprintf(who, sizeof who, "%s@%s", user && *user ? user : "root", host);
    int n = (int)strlen(who);
    for (int i = 0; i < n; i++) {
        rgb_t c = at(SUNSET, 3, i, n);
        printf("\\[\033[38;2;%d;%d;%dm\\]%c", c.r, c.g, c.b, who[i]);
    }
    printf("\\[\033[38;2;122;127;140m\\]:\\[\033[38;2;143;184;240m\\]\\w"
           "\\[\033[38;2;255;211;110m\\] \\$ \\[\033[0m\\]");
}

int main(int argc, char **argv) {
    const char *term = getenv("TERM");
    utf8 = !(term && !strcmp(term, "linux"));
    if (argc > 1 && !strcmp(argv[1], "--ps1")) { ps1(); return 0; }

    putchar('\n');
    const int w = 44;
    for (int r = 0; r < 3; r++) {
        fputs("  ", stdout);
        for (int i = 0; logo[r][i]; i++) {
            unsigned char c = (unsigned char)logo[r][i];
            if (c == ' ') { putchar(' '); continue; }
            fg(at(RAINBOW, 5, i, w));
            block(c);
        }
        reset(); putchar('\n');
    }
    putchar('\n');

    struct utsname u;
    uname(&u);
    const char *user = getenv("USER");
    char host[64] = "samara", buf[256];
    gethostname(host, sizeof host);
    snprintf(buf, sizeof buf, "%s@%s", user && *user ? user : "root", host);
    fputs("  ", stdout); grad_text(buf, SUNSET, 3); reset(); putchar('\n');
    fputs("  ", stdout); fg(DIM);
    for (size_t i = 0; i < strlen(buf); i++) fputs(utf8 ? "\xe2\x94\x80" : "\xc4", stdout);
    reset(); putchar('\n');

    row("OS", "SamaraOS 0.5 (i686)");
    snprintf(buf, sizeof buf, "%s %s", u.sysname, u.release);
    row("Kernel", buf);
    double up = 0;
    if (read_first("/proc/uptime", buf, sizeof buf)) up = atof(buf);
    int s = (int)up;
    if (s >= 3600) snprintf(buf, sizeof buf, "%dh %dm", s / 3600, s / 60 % 60);
    else snprintf(buf, sizeof buf, "%dm %ds", s / 60, s % 60);
    row("Uptime", buf);
    char cpu[4096];
    if (read_first("/proc/cpuinfo", cpu, sizeof cpu)) {
        char *p = strstr(cpu, "model name");
        if (p && (p = strchr(p, ':'))) {
            p++;
            while (*p == ' ') p++;
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            row("CPU", p);
        }
    }
    long tot = meminfo("MemTotal:"), avail = meminfo("MemAvailable:");
    if (avail < 0) avail = meminfo("MemFree:");
    if (tot > 0) {
        snprintf(buf, sizeof buf, "%ld MiB / %ld MiB", (tot - avail) / 1024, tot / 1024);
        row("Memory", buf);
    }
    const char *sh = getenv("SHELL");
    row("Shell", sh ? sh : "/bin/sh");
    row("Terminal", term ? term : "?");

    /* palette */
    fputs("\n  ", stdout);
    for (int i = 0; i < 8; i++) printf("\033[4%dm   ", i);
    reset(); fputs("\n  ", stdout);
    for (int i = 0; i < 8; i++) printf("\033[10%dm   ", i);
    reset(); fputs("\n\n", stdout);
    return 0;
}
