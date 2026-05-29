#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "fs.h"
#include "heap.h"
#include "task.h"
#include "pit.h"
#include "io.h"
#include "mouse.h"
#include "desktop.h"
#include "font.h"
#include "gfx.h"
#include "gfx_term.h"
#include "wm.h"
#include "ata.h"
#include "doom.h"
#include "mediaplayer.h"
#include "paint.h"
#include "clock.h"
#include "sb16.h"
#include "wav.h"
#include "fat.h"
#include "snake.h"
#include "net.h"

#define LINE_MAX 256
#define HIST_MAX 16

static fs_node_t* cwd;
static int shell_request_exit = 0;
static int shell_exit_on_esc = 0;       /* if true, Esc inside readline exits shell */

/* WM-terminal state (defined further down, forward-declared so cmd_exit sees them) */
static bool g_in_wm_terminal;
static window_t* g_current_term_window;

/* ===================== readline with cursor + history ===================== */

static char history[HIST_MAX][LINE_MAX];
static int  hist_count = 0;

static void hist_push(const char* line) {
    if (!line[0]) return;
    if (hist_count > 0 && !strcmp(history[0], line)) return;
    int n = hist_count < HIST_MAX ? hist_count + 1 : HIST_MAX;
    for (int i = n - 1; i > 0; i--) memcpy(history[i], history[i-1], LINE_MAX);
    strncpy(history[0], line, LINE_MAX - 1);
    history[0][LINE_MAX - 1] = 0;
    if (hist_count < HIST_MAX) hist_count++;
}

static void prompt(void) {
    char path[256];
    fs_path(cwd, path, sizeof(path));
    vga_set_color(VGA_LGREEN, VGA_BLACK);
    vga_puts("user@samara");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    vga_putc(':');
    vga_set_color(VGA_LBLUE, VGA_BLACK);
    vga_puts(path);
    vga_set_color(VGA_LGREY, VGA_BLACK);
    vga_puts("$ ");
}

static void redraw_tail(char* buf, int len, int cur, int prompt_x, int prompt_y) {
    vga_set_cursor(prompt_x + cur, prompt_y);
    for (int i = cur; i < len; i++) vga_putc(buf[i]);
    vga_putc(' ');
    vga_set_cursor(prompt_x + cur, prompt_y);
}

static void readline(char* buf, int cap) {
    int len = 0, cur = 0;
    int hist_idx = 0;
    int prompt_x, prompt_y;
    vga_get_cursor(&prompt_x, &prompt_y);
    buf[0] = 0;

    while (1) {
        char c = kbd_getc();

        if (c == '\n') {
            buf[len] = 0;
            vga_set_cursor(prompt_x + len, prompt_y);
            vga_putc('\n');
            hist_push(buf);
            return;
        }
        if (c == 0x1B) {                 /* Esc */
            if (shell_exit_on_esc) {
                shell_request_exit = 1;
                buf[0] = 0;
                vga_putc('\n');
                return;
            }
            continue;
        }
        if (c == '\b') {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                len--; cur--;
                buf[len] = 0;
                redraw_tail(buf, len, cur, prompt_x, prompt_y);
            }
            continue;
        }
        if (c == (char)K_DEL) {
            if (cur < len) {
                memmove(buf + cur, buf + cur + 1, len - cur);
                len--;
                buf[len] = 0;
                redraw_tail(buf, len, cur, prompt_x, prompt_y);
            }
            continue;
        }
        if (c == (char)K_LEFT) {
            if (cur > 0) { cur--; vga_set_cursor(prompt_x + cur, prompt_y); }
            continue;
        }
        if (c == (char)K_RIGHT) {
            if (cur < len) { cur++; vga_set_cursor(prompt_x + cur, prompt_y); }
            continue;
        }
        if (c == (char)K_HOME) {
            cur = 0; vga_set_cursor(prompt_x, prompt_y);
            continue;
        }
        if (c == (char)K_END) {
            cur = len; vga_set_cursor(prompt_x + cur, prompt_y);
            continue;
        }
        if (c == (char)K_UP) {
            if (hist_idx < hist_count) {
                vga_set_cursor(prompt_x, prompt_y);
                for (int i = 0; i < len; i++) vga_putc(' ');
                hist_idx++;
                strncpy(buf, history[hist_idx - 1], cap - 1);
                buf[cap-1] = 0;
                len = strlen(buf);
                cur = len;
                vga_set_cursor(prompt_x, prompt_y);
                vga_puts(buf);
            }
            continue;
        }
        if (c == (char)K_DOWN) {
            if (hist_idx > 0) {
                vga_set_cursor(prompt_x, prompt_y);
                for (int i = 0; i < len; i++) vga_putc(' ');
                hist_idx--;
                if (hist_idx == 0) { buf[0] = 0; len = 0; }
                else { strncpy(buf, history[hist_idx - 1], cap - 1); buf[cap-1]=0; len = strlen(buf); }
                cur = len;
                vga_set_cursor(prompt_x, prompt_y);
                vga_puts(buf);
            }
            continue;
        }
        if (c == 3) {            /* Ctrl+C */
            vga_putc('\n');
            buf[0] = 0;
            return;
        }
        if (c == 12) {           /* Ctrl+L */
            vga_clear();
            prompt();
            vga_get_cursor(&prompt_x, &prompt_y);
            vga_puts(buf);
            vga_set_cursor(prompt_x + cur, prompt_y);
            continue;
        }
        if (c == 4) {            /* Ctrl+D = exit */
            shell_request_exit = 1;
            buf[0] = 0;
            vga_putc('\n');
            return;
        }
        if (c >= 0x20 && c < 0x7F && len < cap - 1) {
            if (cur < len) memmove(buf + cur + 1, buf + cur, len - cur);
            buf[cur] = c;
            len++; cur++;
            buf[len] = 0;
            vga_set_cursor(prompt_x + cur - 1, prompt_y);
            for (int i = cur - 1; i < len; i++) vga_putc(buf[i]);
            vga_set_cursor(prompt_x + cur, prompt_y);
        }
    }
}

static int split(char* line, char** argv, int max) {
    int n = 0;
    char* p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return n;
}

/* ===================== file commands ===================== */

static void cmd_clear(int argc, char** argv) { (void)argc; (void)argv; vga_clear(); }

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) { vga_puts(argv[i]); if (i < argc - 1) vga_putc(' '); }
    vga_putc('\n');
}

static void cmd_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    char p[256]; fs_path(cwd, p, sizeof(p));
    vga_puts(p); vga_putc('\n');
}

static void cmd_ls(int argc, char** argv) {
    fs_node_t* d = (argc > 1) ? fs_resolve(cwd, argv[1]) : cwd;
    if (!d) { vga_puts("ls: no such path\n"); return; }
    if (d->type != FS_DIR) { vga_puts(d->name); vga_putc('\n'); return; }
    for (fs_node_t* c = d->child; c; c = c->next) {
        if (c->type == FS_DIR) {
            vga_set_color(VGA_LBLUE, VGA_BLACK);
            vga_puts(c->name); vga_putc('/');
        } else {
            vga_set_color(VGA_LGREY, VGA_BLACK);
            vga_puts(c->name);
        }
        vga_set_color(VGA_LGREY, VGA_BLACK);
        vga_puts("  ");
    }
    vga_putc('\n');
}

static void cmd_cd(int argc, char** argv) {
    if (argc < 2) { cwd = fs_root(); return; }
    fs_node_t* d = fs_resolve(cwd, argv[1]);
    if (!d) { vga_puts("cd: no such path\n"); return; }
    if (d->type != FS_DIR) { vga_puts("cd: not a directory\n"); return; }
    cwd = d;
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) { vga_puts("cat: missing file\n"); return; }
    fs_node_t* f = fs_resolve(cwd, argv[1]);
    if (!f) { vga_puts("cat: no such file\n"); return; }
    if (f->type != FS_FILE) { vga_puts("cat: not a file\n"); return; }
    if (f->data) for (size_t i = 0; i < f->size; i++) vga_putc(f->data[i]);
    if (f->size && f->data[f->size-1] != '\n') vga_putc('\n');
}

static void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { vga_puts("mkdir: missing name\n"); return; }
    if (!fs_create(cwd, argv[1], FS_DIR)) vga_puts("mkdir: failed\n");
}

static void cmd_touch(int argc, char** argv) {
    if (argc < 2) { vga_puts("touch: missing name\n"); return; }
    fs_node_t* f = fs_resolve(cwd, argv[1]);
    if (f) return;
    if (!fs_create(cwd, argv[1], FS_FILE)) vga_puts("touch: failed\n");
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) { vga_puts("rm: missing path\n"); return; }
    if (fs_unlink(cwd, argv[1]) != 0) vga_puts("rm: failed\n");
}

static void cmd_cp(int argc, char** argv) {
    if (argc < 3) { vga_puts("cp: usage: cp <src> <dst>\n"); return; }
    fs_node_t* src = fs_resolve(cwd, argv[1]);
    if (!src || src->type != FS_FILE) { vga_puts("cp: no source file\n"); return; }
    fs_node_t* dst = fs_resolve(cwd, argv[2]);
    if (!dst) dst = fs_create(cwd, argv[2], FS_FILE);
    if (!dst || dst->type != FS_FILE) { vga_puts("cp: bad destination\n"); return; }
    if (src->data) fs_write(dst, src->data, src->size);
    else fs_write(dst, "", 0);
}

static void cmd_mv(int argc, char** argv) {
    if (argc < 3) { vga_puts("mv: usage: mv <src> <dst>\n"); return; }
    fs_node_t* src = fs_resolve(cwd, argv[1]);
    if (!src) { vga_puts("mv: no source\n"); return; }
    if (src->type != FS_FILE) { vga_puts("mv: only files supported\n"); return; }
    fs_node_t* dst = fs_create(cwd, argv[2], FS_FILE);
    if (!dst) { vga_puts("mv: cannot create destination\n"); return; }
    if (src->data) fs_write(dst, src->data, src->size);
    fs_unlink(cwd, argv[1]);
}

static void cmd_head(int argc, char** argv) {
    int n = 10, fi = 1;
    if (argc >= 4 && !strcmp(argv[1], "-n")) { n = atoi(argv[2]); fi = 3; }
    if (fi >= argc) { vga_puts("head: missing file\n"); return; }
    fs_node_t* f = fs_resolve(cwd, argv[fi]);
    if (!f || f->type != FS_FILE || !f->data) return;
    int lines = 0;
    for (size_t i = 0; i < f->size && lines < n; i++) {
        vga_putc(f->data[i]);
        if (f->data[i] == '\n') lines++;
    }
    if (f->size && f->data[f->size-1] != '\n') vga_putc('\n');
}

static void cmd_tail(int argc, char** argv) {
    int n = 10, fi = 1;
    if (argc >= 4 && !strcmp(argv[1], "-n")) { n = atoi(argv[2]); fi = 3; }
    if (fi >= argc) { vga_puts("tail: missing file\n"); return; }
    fs_node_t* f = fs_resolve(cwd, argv[fi]);
    if (!f || f->type != FS_FILE || !f->data) return;
    int total = 0;
    for (size_t i = 0; i < f->size; i++) if (f->data[i] == '\n') total++;
    int skip = total - n;
    if (skip < 0) skip = 0;
    int line = 0;
    for (size_t i = 0; i < f->size; i++) {
        if (line >= skip) vga_putc(f->data[i]);
        if (f->data[i] == '\n') line++;
    }
    if (f->size && f->data[f->size-1] != '\n') vga_putc('\n');
}

static void cmd_wc(int argc, char** argv) {
    if (argc < 2) { vga_puts("wc: missing file\n"); return; }
    fs_node_t* f = fs_resolve(cwd, argv[1]);
    if (!f || f->type != FS_FILE) { vga_puts("wc: not a file\n"); return; }
    int lines = 0, words = 0, bytes = (int)f->size;
    bool in = false;
    if (f->data) for (size_t i = 0; i < f->size; i++) {
        char c = f->data[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n') {
            if (in) { words++; in = false; }
        } else in = true;
    }
    if (in) words++;
    vga_printf(" %d %d %d %s\n", lines, words, bytes, argv[1]);
}

static void cmd_grep(int argc, char** argv) {
    if (argc < 3) { vga_puts("grep: usage: grep <pattern> <file>\n"); return; }
    fs_node_t* f = fs_resolve(cwd, argv[2]);
    if (!f || f->type != FS_FILE || !f->data) { vga_puts("grep: no such file\n"); return; }
    int ln = 1;
    size_t line_start = 0;
    for (size_t i = 0; i <= f->size; i++) {
        if (i == f->size || f->data[i] == '\n') {
            char buf[256];
            size_t L = i - line_start;
            if (L > 255) L = 255;
            memcpy(buf, f->data + line_start, L);
            buf[L] = 0;
            if (strstr(buf, argv[1])) {
                char tmp[16]; itoa(ln, tmp, 10);
                vga_set_color(VGA_LGREEN, VGA_BLACK); vga_puts(tmp);
                vga_set_color(VGA_LGREY, VGA_BLACK); vga_putc(':');
                vga_puts(buf); vga_putc('\n');
            }
            ln++;
            line_start = i + 1;
        }
    }
}

static const char* g_find_pattern;
static void find_recurse(fs_node_t* n) {
    if (!n) return;
    if (strstr(n->name, g_find_pattern)) {
        char p[256]; fs_path(n, p, sizeof(p));
        vga_puts(p); vga_putc('\n');
    }
    if (n->type == FS_DIR)
        for (fs_node_t* c = n->child; c; c = c->next) find_recurse(c);
}
static void cmd_find(int argc, char** argv) {
    if (argc < 2) { vga_puts("find: usage: find <name>\n"); return; }
    g_find_pattern = argv[1];
    find_recurse(cwd);
}

/* ===================== system ===================== */

static void cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    uint32_t ms = pit_uptime_ms();
    vga_printf("up %u ms (%u s)  ticks=%u\n", ms, ms / 1000, pit_ticks());
}

static void cmd_mem(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_printf("heap: %u / %u bytes used\n", (uint32_t)heap_used(), (uint32_t)heap_total());
}

static void cmd_df(int argc, char** argv) {
    (void)argc; (void)argv;
    int t = (int)(heap_total() / 1024);
    int u = (int)(heap_used()  / 1024);
    int f = t - u;
    int p = t > 0 ? (u * 100) / t : 0;
    vga_puts("Filesystem  KB-total  KB-used  KB-free  Use%  Mount\n");
    vga_printf("ramfs       %d      %d     %d    %d%%   /\n", t, u, f, p);
}

static void cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts(" id  state  ticks  name\n");
    task_dump(vga_puts);
}

static void cmd_mouse(int argc, char** argv) {
    (void)argc; (void)argv;
    int x, y; uint8_t b; mouse_get(&x, &y, &b);
    vga_printf("mouse x=%d y=%d btn=0x%x\n", x, y, b);
}

static void cmd_uname(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "-a"))
        vga_puts("SamaraOS 0.4 i686 samara graphics\n");
    else vga_puts("SamaraOS\n");
}

static void cmd_whoami(int argc, char** argv) { (void)argc; (void)argv; vga_puts("user\n"); }

static void cmd_about(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    vga_puts("SamaraOS 0.4\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    vga_puts("Hobby OS in C: multitasking, fs, PS/2, VBE/13h graphics, terminal\n");
    vga_puts("Type 'help' for the full command list.\n");
}

static void cmd_history(int argc, char** argv) {
    (void)argc; (void)argv;
    for (int i = hist_count - 1; i >= 0; i--) {
        vga_printf(" %d  %s\n", hist_count - i, history[i]);
    }
}

static void cmd_exit(int argc, char** argv) {
    (void)argc; (void)argv;
    if (g_in_wm_terminal && g_current_term_window && g_current_term_window->open) {
        wm_close(g_current_term_window);
        g_in_wm_terminal = false;
    } else {
        shell_request_exit = 1;
    }
}

static void cmd_sleep(int argc, char** argv) {
    if (argc < 2) { vga_puts("sleep: ms\n"); return; }
    int ms = atoi(argv[1]);
    uint32_t start = pit_uptime_ms();
    while ((int)(pit_uptime_ms() - start) < ms) __asm__ volatile ("hlt");
}

static void cmd_shutdown(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts("shutdown...\n");
    outw(0x604,  0x2000);  /* QEMU >= 2.0 */
    outw(0xB004, 0x2000);  /* older QEMU/Bochs */
    outw(0x4004, 0x3400);  /* virtualbox */
    __asm__ volatile ("cli; hlt");
}

static void cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts("rebooting...\n");
    while (inb(0x64) & 0x02) {}
    outb(0x64, 0xFE);
    __asm__ volatile ("cli; hlt");
}

static void cmd_beep(int argc, char** argv) {
    int freq = 1000;
    if (argc > 1) freq = atoi(argv[1]);
    if (freq < 50) freq = 50;
    if (freq > 10000) freq = 10000;
    uint32_t div = 1193180U / (uint32_t)freq;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 3);
    uint32_t start = pit_uptime_ms();
    while (pit_uptime_ms() - start < 200) __asm__ volatile ("hlt");
    outb(0x61, tmp & 0xFC);
}

/* ----- date via CMOS RTC ----- */
static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xF)); }

static void cmd_date(int argc, char** argv) {
    (void)argc; (void)argv;
    /* wait for not-updating */
    do { outb(0x70, 0x0A); } while (inb(0x71) & 0x80);

    outb(0x70, 0x00); uint8_t s  = inb(0x71);
    outb(0x70, 0x02); uint8_t m  = inb(0x71);
    outb(0x70, 0x04); uint8_t h  = inb(0x71);
    outb(0x70, 0x07); uint8_t d  = inb(0x71);
    outb(0x70, 0x08); uint8_t mo = inb(0x71);
    outb(0x70, 0x09); uint8_t y  = inb(0x71);
    outb(0x70, 0x0B); uint8_t b  = inb(0x71);

    if (!(b & 0x04)) {
        s = bcd2bin(s);  m = bcd2bin(m);  h = bcd2bin(h);
        d = bcd2bin(d);  mo = bcd2bin(mo); y = bcd2bin(y);
    }

    char buf[32];
    char tmp[8];
    int p = 0;
    #define APPEND2(v) do { \
        if ((v) < 10) buf[p++] = '0';                     \
        itoa((v), tmp, 10);                               \
        for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];   \
    } while (0)
    buf[p++]='2'; buf[p++]='0'; APPEND2(y);
    buf[p++]='-'; APPEND2(mo);
    buf[p++]='-'; APPEND2(d);
    buf[p++]=' '; APPEND2(h);
    buf[p++]=':'; APPEND2(m);
    buf[p++]=':'; APPEND2(s);
    buf[p++] = '\n'; buf[p] = 0;
    #undef APPEND2
    vga_puts(buf);
}

/* ----- calc: simple recursive descent + - * / % ----- */
static const char* calc_p;
static int calc_expr(void);
static void skip_ws(void) { while (*calc_p == ' ' || *calc_p == '\t') calc_p++; }
static int calc_factor(void) {
    skip_ws();
    if (*calc_p == '(') { calc_p++; int v = calc_expr(); skip_ws(); if (*calc_p == ')') calc_p++; return v; }
    int sign = 1;
    if (*calc_p == '-') { sign = -1; calc_p++; }
    else if (*calc_p == '+') calc_p++;
    int v = 0;
    if (*calc_p < '0' || *calc_p > '9') return 0;
    while (*calc_p >= '0' && *calc_p <= '9') { v = v * 10 + (*calc_p - '0'); calc_p++; }
    return sign * v;
}
static int calc_term(void) {
    int v = calc_factor();
    while (1) {
        skip_ws();
        char op = *calc_p;
        if (op != '*' && op != '/' && op != '%') break;
        calc_p++;
        int r = calc_factor();
        if (op == '*') v *= r;
        else if (op == '/') v = r ? v / r : 0;
        else                v = r ? v % r : 0;
    }
    return v;
}
static int calc_expr(void) {
    int v = calc_term();
    while (1) {
        skip_ws();
        char op = *calc_p;
        if (op != '+' && op != '-') break;
        calc_p++;
        int r = calc_term();
        if (op == '+') v += r; else v -= r;
    }
    return v;
}
static void cmd_calc(int argc, char** argv) {
    if (argc < 2) { vga_puts("calc: usage: calc <expr>\n"); return; }
    char buf[256] = "";
    for (int i = 1; i < argc; i++) {
        if (strlen(buf) + strlen(argv[i]) + 1 >= sizeof(buf)) break;
        strcat(buf, argv[i]);
        if (i < argc - 1) strcat(buf, " ");
    }
    calc_p = buf;
    int r = calc_expr();
    vga_printf("= %d\n", r);
}

/* ===================== help ===================== */

static void cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts(
        "files:   ls cd cat pwd mkdir touch rm cp mv head tail wc grep find\n"
        "edit:    nano <file>          (^S save, ^X/^Q exit)\n"
        "system:  uptime mem df ps mouse uname whoami date about history\n"
        "shell:   help clear echo calc sleep beep history exit reboot shutdown\n"
        "gfx:     desktop              terminal-on-desktop graphical mode\n"
        "         neofetch\n"
        "apps:    player (music)  paint  clock  bounce  doom  snake\n"
        "         doom-mini (in-tree mini engine TITLEPIC)  play (mini E1M1)\n"
        "audio:   playwav <file>  stopwav  sbinfo   (try: playwav welcome.wav)\n"
        "fat:     fatmount [drv]  fatls  fatload <name> [ram]  playfat <name>\n"
        "net:     ifconfig  ping <ip> [count]  wget <http://ip[:port]/path> [file]\n"
        "Line ed: Left/Right Home/End Up/Down history Del ^C ^L ^D\n"
    );
}

/* ===================== nano ===================== */

#define E_BUF 8192

static char ebuf[E_BUF];
static int  elen, ecur, etop, edesired_col, edirty;
static fs_node_t* efile;
static char ename[64];
static char emsg[64];
static int  emsg_ticks;

static int e_line_of(int off) {
    int n = 0;
    for (int i = 0; i < off; i++) if (ebuf[i] == '\n') n++;
    return n;
}
static int e_col_of(int off) {
    int c = 0;
    for (int i = off - 1; i >= 0 && ebuf[i] != '\n'; i--) c++;
    return c;
}
static int e_offset_of(int line, int col) {
    int off = 0, l = 0;
    while (off < elen && l < line) { if (ebuf[off++] == '\n') l++; }
    if (l < line) return elen;
    int c = 0;
    while (off < elen && ebuf[off] != '\n' && c < col) { off++; c++; }
    return off;
}
static int e_total_lines(void) {
    int n = 1;
    for (int i = 0; i < elen; i++) if (ebuf[i] == '\n') n++;
    return n;
}
static void e_set_msg(const char* m) {
    int i = 0;
    while (m[i] && i < (int)sizeof(emsg) - 1) { emsg[i] = m[i]; i++; }
    emsg[i] = 0;
    emsg_ticks = 1;
}

static void e_render(void) {
    int cur_line = e_line_of(ecur);
    int cur_col  = e_col_of(ecur);
    int rows = VGA_HEIGHT - 2;

    if (cur_line < etop) etop = cur_line;
    if (cur_line >= etop + rows) etop = cur_line - rows + 1;
    if (etop < 0) etop = 0;

    vga_set_color(VGA_BLACK, VGA_LCYAN);
    vga_set_cursor(0, 0);
    char hdr[VGA_WIDTH + 1];
    for (int i = 0; i < VGA_WIDTH; i++) hdr[i] = ' ';
    const char* p = " SamaraOS nano 0.2   ";
    int hp = 0;
    for (; p[hp]; hp++) hdr[hp] = p[hp];
    for (int i = 0; ename[i] && hp < VGA_WIDTH; i++, hp++) hdr[hp] = ename[i];
    if (edirty && hp < VGA_WIDTH - 4) { hdr[hp++] = ' '; hdr[hp++] = '*'; }
    for (int i = 0; i < VGA_WIDTH; i++) vga_putc(hdr[i]);

    vga_set_color(VGA_LGREY, VGA_BLACK);
    int off = 0, line = 0;
    while (off < elen && line < etop) { if (ebuf[off++] == '\n') line++; }

    for (int row = 1; row <= rows; row++) {
        vga_set_cursor(0, row);
        for (int i = 0; i < VGA_WIDTH; i++) vga_putc(' ');
        vga_set_cursor(0, row);
        if (off >= elen && line + (row - 1) > 0) {
            vga_set_color(VGA_DGREY, VGA_BLACK);
            vga_putc('~');
            vga_set_color(VGA_LGREY, VGA_BLACK);
            continue;
        }
        int col = 0;
        while (off < elen && ebuf[off] != '\n') {
            if (col < VGA_WIDTH) vga_putc(ebuf[off]);
            off++; col++;
        }
        if (off < elen && ebuf[off] == '\n') off++;
    }

    vga_set_cursor(0, VGA_HEIGHT - 1);
    vga_set_color(VGA_BLACK, VGA_LGREY);
    char st[VGA_WIDTH + 1];
    for (int i = 0; i < VGA_WIDTH; i++) st[i] = ' ';
    const char* sc = " ^S Save   ^X Exit   ^Q Quit ";
    int sp = 0;
    for (; sc[sp]; sp++) st[sp] = sc[sp];

    char info[64]; int ip = 0; char tmp[16];
    info[ip++]=' '; info[ip++]='L'; info[ip++]=':';
    itoa(cur_line + 1, tmp, 10); for (int i = 0; tmp[i]; i++) info[ip++] = tmp[i];
    info[ip++]=' '; info[ip++]='C'; info[ip++]=':';
    itoa(cur_col + 1, tmp, 10);  for (int i = 0; tmp[i]; i++) info[ip++] = tmp[i];
    info[ip++]=' '; info[ip++]='B'; info[ip++]=':';
    itoa(elen, tmp, 10);         for (int i = 0; tmp[i]; i++) info[ip++] = tmp[i];
    info[ip++]=' '; info[ip] = 0;
    int istart = VGA_WIDTH - ip;
    if (istart < sp + 1) istart = sp + 1;
    for (int i = 0; info[i] && istart + i < VGA_WIDTH; i++) st[istart + i] = info[i];

    if (emsg_ticks > 0) {
        int ml = (int)strlen(emsg);
        int mstart = sp + 2;
        if (mstart + ml > istart - 1) ml = istart - 1 - mstart;
        for (int i = 0; i < ml; i++) st[mstart + i] = emsg[i];
        emsg_ticks = 0;
    }
    for (int i = 0; i < VGA_WIDTH; i++) vga_putc(st[i]);
    vga_set_color(VGA_LGREY, VGA_BLACK);

    int sx = cur_col;
    int sy = (cur_line - etop) + 1;
    if (sx >= VGA_WIDTH) sx = VGA_WIDTH - 1;
    if (sy < 1) sy = 1;
    if (sy > rows) sy = rows;
    vga_set_cursor(sx, sy);
}

static void e_insert(char c) {
    if (elen >= E_BUF - 1) return;
    if (ecur < elen) memmove(ebuf + ecur + 1, ebuf + ecur, elen - ecur);
    ebuf[ecur] = c;
    elen++; ecur++;
    edirty = 1;
    edesired_col = e_col_of(ecur);
}
static void e_backspace(void) {
    if (ecur == 0) return;
    if (ecur < elen) memmove(ebuf + ecur - 1, ebuf + ecur, elen - ecur);
    elen--; ecur--;
    edirty = 1;
    edesired_col = e_col_of(ecur);
}
static void e_delete(void) {
    if (ecur >= elen) return;
    memmove(ebuf + ecur, ebuf + ecur + 1, elen - ecur - 1);
    elen--; edirty = 1;
}

static void cmd_nano(int argc, char** argv) {
    if (argc < 2) { vga_puts("nano: missing file\n"); return; }
    efile = fs_resolve(cwd, argv[1]);
    if (!efile) efile = fs_create(cwd, argv[1], FS_FILE);
    if (!efile || efile->type != FS_FILE) { vga_puts("nano: cannot open\n"); return; }

    strncpy(ename, argv[1], sizeof(ename) - 1);
    ename[sizeof(ename) - 1] = 0;
    elen = 0;
    if (efile->data) {
        elen = (int)efile->size;
        if (elen > E_BUF - 1) elen = E_BUF - 1;
        memcpy(ebuf, efile->data, elen);
    }
    ebuf[elen] = 0;
    ecur = 0; etop = 0; edesired_col = 0; edirty = 0;
    emsg[0] = 0; emsg_ticks = 0;

    vga_clear();
    e_render();

    while (1) {
        char c = kbd_getc();
        if (c == 17) { vga_clear(); return; }                /* ^Q */
        if (c == 24) {                                        /* ^X */
            if (edirty) { e_set_msg("Unsaved! ^X again or ^S"); edirty = 2; e_render(); continue; }
            if (edirty == 2) { vga_clear(); return; }
            vga_clear(); return;
        }
        if (c == 19) { fs_write(efile, ebuf, elen); edirty = 0; e_set_msg("Saved"); e_render(); continue; }
        if (edirty == 2) edirty = 1;

        if (c == (char)K_LEFT) {
            if (ecur > 0) { ecur--; edesired_col = e_col_of(ecur); }
        } else if (c == (char)K_RIGHT) {
            if (ecur < elen) { ecur++; edesired_col = e_col_of(ecur); }
        } else if (c == (char)K_UP) {
            int line = e_line_of(ecur);
            if (line > 0) ecur = e_offset_of(line - 1, edesired_col);
        } else if (c == (char)K_DOWN) {
            int line = e_line_of(ecur);
            if (line < e_total_lines() - 1) ecur = e_offset_of(line + 1, edesired_col);
        } else if (c == (char)K_HOME) {
            ecur = e_offset_of(e_line_of(ecur), 0); edesired_col = 0;
        } else if (c == (char)K_END) {
            int line = e_line_of(ecur);
            int off  = e_offset_of(line, 0);
            while (off < elen && ebuf[off] != '\n') off++;
            ecur = off; edesired_col = e_col_of(ecur);
        } else if (c == (char)K_PGUP) {
            int line = e_line_of(ecur) - (VGA_HEIGHT - 4);
            if (line < 0) line = 0;
            ecur = e_offset_of(line, edesired_col);
        } else if (c == (char)K_PGDN) {
            int line = e_line_of(ecur) + (VGA_HEIGHT - 4);
            int total = e_total_lines();
            if (line > total - 1) line = total - 1;
            ecur = e_offset_of(line, edesired_col);
        } else if (c == (char)K_DEL) {
            e_delete();
        } else if (c == '\b') {
            e_backspace();
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
            e_insert(c);
        }
        e_render();
    }
}

/* ===================== neofetch ===================== */

static void cmd_neofetch(int argc, char** argv) {
    (void)argc; (void)argv;
    const char* logo[] = {
        "    ____                                  ",
        "   / __/__ _ __ _  ___ _ _________ _      ",
        "  _\\ \\/ _ `/  '  \\/ _ `// __/ _ `/      ",
        " /___/\\_,_/_/_/_/\\_,_(_)__/\\_,_/        ",
        "                                          ",
    };
    int sx, sy; vga_get_cursor(&sx, &sy);
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    for (int i = 0; i < 5; i++) { vga_puts(logo[i]); vga_putc('\n'); }
    vga_set_color(VGA_LGREY, VGA_BLACK);
    int ex, ey; vga_get_cursor(&ex, &ey);
    int info_y = sy;
    int info_x = 46;

    #define INFO(label, value) do {                                    \
        vga_set_cursor(info_x, info_y++);                              \
        vga_set_color(VGA_LMAGENTA, VGA_BLACK); vga_puts(label);       \
        vga_set_color(VGA_LGREY, VGA_BLACK);    vga_puts(value);       \
    } while (0)

    INFO("OS:     ", "SamaraOS 0.4");
    INFO("Kernel: ", "samara-kernel 0.4");
    char buf[32]; utoa(pit_uptime_ms() / 1000, buf, 10); strcat(buf, " s");
    INFO("Uptime: ", buf);
    char hbuf[64]; char tmp[16];
    utoa((uint32_t)heap_used(), tmp, 10); strcpy(hbuf, tmp); strcat(hbuf, " / ");
    utoa((uint32_t)heap_total(), tmp, 10); strcat(hbuf, tmp); strcat(hbuf, " B");
    INFO("Heap:   ", hbuf);
    char tbuf[16]; itoa(task_count(), tbuf, 10);
    INFO("Tasks:  ", tbuf);
    INFO("Shell:  ", "samara-sh");
    INFO("CPU:    ", "i686 (qemu)");
    INFO("Term:   ", "VGA / VBE 1024x768");
    vga_set_cursor(0, ey > info_y ? ey : info_y);
    vga_putc('\n');
    #undef INFO
}

/* ===================== forward decl ===================== */
static void execute(char* line);

/* ===================== terminal-as-window (used by WM) ===================== */

static char term_buf[LINE_MAX];
static int  term_len = 0, term_cur = 0, term_hist_idx = 0;
static int  term_prompt_x = 0, term_prompt_y = 0;
/* g_in_wm_terminal and g_current_term_window declared near top of file */

static void term_redraw_tail(void) {
    vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    for (int i = term_cur; i < term_len; i++) vga_putc(term_buf[i]);
    vga_putc(' ');
    vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
}

static void term_show_prompt(void) {
    prompt();
    vga_get_cursor(&term_prompt_x, &term_prompt_y);
    term_buf[0] = 0; term_len = 0; term_cur = 0; term_hist_idx = 0;
}

void wm_terminal_init(window_t* w) {
    vga_use_gfx_term(w->term_x, w->term_y);
    vga_clear();
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    vga_puts("SamaraOS Terminal\n");
    vga_set_color(VGA_DGREY, VGA_BLACK);
    vga_puts("'help' for commands. 'exit' or X closes this window. Esc exits desktop.\n\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
    g_in_wm_terminal = true;
    g_current_term_window = w;
    term_show_prompt();
}

void wm_terminal_handle_key(window_t* w, char c) {
    g_current_term_window = w;

    if (c == '\n') {
        term_buf[term_len] = 0;
        vga_set_cursor(term_prompt_x + term_len, term_prompt_y);
        vga_putc('\n');
        hist_push(term_buf);
        char copy[LINE_MAX];
        strncpy(copy, term_buf, LINE_MAX - 1); copy[LINE_MAX - 1] = 0;
        execute(copy);
        if (!w->open) { g_in_wm_terminal = false; return; }
        term_show_prompt();
        return;
    }
    if (c == '\b') {
        if (term_cur > 0) {
            memmove(term_buf + term_cur - 1, term_buf + term_cur, term_len - term_cur);
            term_len--; term_cur--; term_buf[term_len] = 0;
            term_redraw_tail();
        }
        return;
    }
    if (c == (char)K_DEL) {
        if (term_cur < term_len) {
            memmove(term_buf + term_cur, term_buf + term_cur + 1, term_len - term_cur);
            term_len--; term_buf[term_len] = 0;
            term_redraw_tail();
        }
        return;
    }
    if (c == (char)K_LEFT) {
        if (term_cur > 0) { term_cur--; vga_set_cursor(term_prompt_x + term_cur, term_prompt_y); }
        return;
    }
    if (c == (char)K_RIGHT) {
        if (term_cur < term_len) { term_cur++; vga_set_cursor(term_prompt_x + term_cur, term_prompt_y); }
        return;
    }
    if (c == (char)K_HOME) { term_cur = 0; vga_set_cursor(term_prompt_x, term_prompt_y); return; }
    if (c == (char)K_END)  { term_cur = term_len; vga_set_cursor(term_prompt_x + term_cur, term_prompt_y); return; }
    if (c == (char)K_UP) {
        if (term_hist_idx < hist_count) {
            vga_set_cursor(term_prompt_x, term_prompt_y);
            for (int i = 0; i < term_len; i++) vga_putc(' ');
            term_hist_idx++;
            strncpy(term_buf, history[term_hist_idx - 1], LINE_MAX - 1);
            term_buf[LINE_MAX - 1] = 0;
            term_len = strlen(term_buf); term_cur = term_len;
            vga_set_cursor(term_prompt_x, term_prompt_y);
            vga_puts(term_buf);
        }
        return;
    }
    if (c == (char)K_DOWN) {
        if (term_hist_idx > 0) {
            vga_set_cursor(term_prompt_x, term_prompt_y);
            for (int i = 0; i < term_len; i++) vga_putc(' ');
            term_hist_idx--;
            if (term_hist_idx == 0) { term_buf[0] = 0; term_len = 0; }
            else { strncpy(term_buf, history[term_hist_idx - 1], LINE_MAX - 1);
                   term_buf[LINE_MAX - 1] = 0; term_len = strlen(term_buf); }
            term_cur = term_len;
            vga_set_cursor(term_prompt_x, term_prompt_y);
            vga_puts(term_buf);
        }
        return;
    }
    if (c == 3) { vga_putc('\n'); term_show_prompt(); return; }                  /* ^C */
    if (c == 4) { wm_close(w); g_in_wm_terminal = false; return; }               /* ^D */
    if (c == 12) { vga_clear(); term_show_prompt(); return; }                     /* ^L */
    if (c >= 0x20 && c < 0x7F && term_len < LINE_MAX - 1) {
        if (term_cur < term_len) memmove(term_buf + term_cur + 1, term_buf + term_cur, term_len - term_cur);
        term_buf[term_cur] = c;
        term_len++; term_cur++; term_buf[term_len] = 0;
        vga_set_cursor(term_prompt_x + term_cur - 1, term_prompt_y);
        for (int i = term_cur - 1; i < term_len; i++) vga_putc(term_buf[i]);
        vga_set_cursor(term_prompt_x + term_cur, term_prompt_y);
    }
}

const char* wm_sysinfo_text(void) {
    static char buf[1024];
    int p = 0;
    char tmp[24];
    #define ADD(s) do { for (const char* _q = (s); *_q; _q++) buf[p++] = *_q; } while (0)
    ADD("System Information\n\n");
    ADD("OS:      SamaraOS 0.5\n");
    ADD("Kernel:  samara-kernel 0.5\n");
    ADD("CPU:     i686\n");
    ADD("Display: ");
    itoa(gfx_w(), tmp, 10); ADD(tmp); ADD(" x ");
    itoa(gfx_h(), tmp, 10); ADD(tmp); ADD(" 32bpp\n");
    ADD("Uptime:  ");
    utoa(pit_uptime_ms() / 1000, tmp, 10); ADD(tmp); ADD(" s\n");
    ADD("Heap:    ");
    utoa((uint32_t)heap_used(),  tmp, 10); ADD(tmp); ADD(" / ");
    utoa((uint32_t)heap_total(), tmp, 10); ADD(tmp); ADD(" B\n");
    ADD("Tasks:   ");
    itoa(task_count(), tmp, 10); ADD(tmp); ADD("\n");
    buf[p] = 0;
    #undef ADD
    return buf;
}

/* ===================== sample App: bouncing ball ===================== */
/* Demonstrates the wm_open_app API used for porting future apps (e.g. DOOM):
   provide on_paint to draw the window's client area, on_key for input. */

typedef struct {
    int x, y, vx, vy;
    uint32_t last_ms;
} bounce_state_t;

static bounce_state_t bounce_st;

static void bounce_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    bounce_state_t* s = (bounce_state_t*)w->user;

    uint32_t now = pit_uptime_ms();
    uint32_t dt = now - s->last_ms;
    s->last_ms = now;
    int steps = (int)(dt / 16);
    if (steps < 1) steps = 1;
    if (steps > 6) steps = 6;
    int r = 12;
    for (int i = 0; i < steps; i++) {
        s->x += s->vx;
        s->y += s->vy;
        if (s->x < r)        { s->x = r;        s->vx = -s->vx; }
        if (s->y < r)        { s->y = r;        s->vy = -s->vy; }
        if (s->x > cw - r)   { s->x = cw - r;   s->vx = -s->vx; }
        if (s->y > ch - r)   { s->y = ch - r;   s->vy = -s->vy; }
    }

    gfx_rect_fill(cx, cy, cw, ch, RGB(0x10, 0x18, 0x30));
    int bx = cx + s->x, by = cy + s->y;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                gfx_pixel(bx + dx, by + dy, RGB(0xFF, 0x80, 0x40));
    gfx_string(cx + 6, cy + 6,  "Sample App  (paint+key callbacks)", RGB(0xFF,0xFF,0xFF), 0, false);
    gfx_string(cx + 6, cy + 22, "Arrows kick ball, Esc closes",     RGB(0x90,0xA0,0xC0), 0, false);
}

static void bounce_key(window_t* w, char c) {
    bounce_state_t* s = (bounce_state_t*)w->user;
    if (c == (char)K_LEFT)  s->vx -= 1;
    if (c == (char)K_RIGHT) s->vx += 1;
    if (c == (char)K_UP)    s->vy -= 1;
    if (c == (char)K_DOWN)  s->vy += 1;
    if (c == 0x1B) wm_close(w);
}

static void cmd_bounce(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) { vga_puts("bounce: enter 'desktop' first\n"); return; }
    bounce_st.x = 100; bounce_st.y = 60;
    bounce_st.vx = 2;  bounce_st.vy = 2;
    bounce_st.last_ms = pit_uptime_ms();
    wm_open_app(120, 120, 380, 240, "Bounce", bounce_paint, bounce_key, &bounce_st);
}

static void cmd_disk(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!ata_present() && !ata_init()) {
        vga_puts("ata: no disk found on primary master\n");
        return;
    }
    uint32_t s = ata_total_sectors();
    vga_printf("ata: primary master  %u sectors  (~%u KiB / ~%u MiB)\n",
               s, (s * 512U) / 1024U, (s * 512U) / (1024U * 1024U));
}

static void cmd_doom_mini(int argc, char** argv) {
    (void)argc; (void)argv;
    if (doom_load_from_disk() != 0) {
        vga_puts("doom-mini: "); vga_puts(doom_status()); vga_putc('\n');
        return;
    }
    vga_puts("doom-mini: "); vga_puts(doom_status()); vga_putc('\n');
    if (!gfx_ready()) { vga_puts("doom-mini: enter 'desktop' first to view TITLEPIC\n"); return; }
    doom_open_window();
}

static void cmd_play(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) { vga_puts("play: enter 'desktop' first\n"); return; }
    int r = doom_play_e1m1();
    if (r != 0) {
        vga_puts("play: ");
        vga_puts(doom_status());
        vga_putc('\n');
    }
}

/* ---- WAV / FAT helpers ---- */

static const char* wav_err_str(int r) {
    switch (r) {
        case  0: return "ok";
        case -1: return "bad args";
        case -2: return "missing RIFF tag (not a WAV file)";
        case -3: return "missing WAVE tag";
        case -4: return "chunk runs off end (truncated file?)";
        case -5: return "not PCM (compressed: try saving as 'PCM WAV')";
        case -6: return "data before fmt chunk";
        case -7: return "no data chunk found";
        case -10: return "SB16 not initialised";
        case -11: return "fmt chunk has zero frame size";
        case -12: return "SB16 play failed";
        default:  return "unknown error";
    }
}

static void fat_list_visit(const char* name, uint32_t size, void* user) {
    (void)user;
    vga_puts("  ");
    vga_puts(name);
    int pad = 13 - (int)strlen(name);
    for (int i = 0; i < pad; i++) vga_putc(' ');
    vga_printf(" %u bytes\n", size);
}

static void cmd_fatmount(int argc, char** argv) {
    int idx = 2;       /* secondary master by default */
    if (argc > 1) idx = atoi(argv[1]);
    if (fat_mount(idx)) {
        vga_printf("fat: mounted drive %d\n", idx);
    } else {
        vga_printf("fat: mount drive %d failed (%s)\n", idx, fat_status());
    }
}

static void cmd_fatls(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!fat_mounted()) { vga_puts("fat: not mounted (run: fatmount)\n"); return; }
    vga_puts("FAT root:\n");
    fat_list(fat_list_visit, NULL);
}

static void cmd_fatload(int argc, char** argv) {
    if (argc < 2) { vga_puts("fatload: usage: fatload <FAT-name> [ramfs-name]\n"); return; }
    if (!fat_mounted()) { vga_puts("fatload: not mounted\n"); return; }
    uint8_t* buf = NULL; uint32_t sz = 0;
    int r = fat_read_file(argv[1], &buf, &sz);
    if (r != 0) { vga_printf("fatload: error %d\n", r); return; }
    const char* dst = argc > 2 ? argv[2] : argv[1];
    fs_node_t* f = fs_resolve(cwd, dst);
    if (!f) f = fs_create(cwd, dst, FS_FILE);
    if (!f || f->type != FS_FILE) { vga_puts("fatload: cannot create ramfs file\n"); kfree(buf); return; }
    fs_write(f, (const char*)buf, sz);
    kfree(buf);
    vga_printf("fatload: %s -> %s (%u bytes)\n", argv[1], dst, sz);
}

/* Convenience: mount + find + play in one go, without copying to ramfs. */
static void cmd_playfat(int argc, char** argv) {
    if (argc < 2) { vga_puts("playfat: usage: playfat <FAT-name>\n"); return; }
    if (!sb16_present()) { vga_puts("playfat: SB16 not available\n"); return; }
    if (!fat_mounted()) {
        if (!fat_mount(2)) {
            vga_printf("playfat: auto-mount failed (%s)\n", fat_status());
            return;
        }
    }
    uint8_t* buf = NULL; uint32_t sz = 0;
    int r = fat_read_file(argv[1], &buf, &sz);
    if (r != 0) { vga_printf("playfat: not found (%d)\n", r); return; }
    vga_printf("playfat: %s, %u bytes, playing...\n", argv[1], sz);
    int pr = wav_play(buf, sz);
    if (pr != 0) vga_printf("playfat: %s (err %d)\n", wav_err_str(pr), pr);
    kfree(buf);
}

static void cmd_playwav(int argc, char** argv) {
    if (argc < 2) { vga_puts("playwav: usage: playwav <file>\n"); return; }
    if (!sb16_present()) {
        vga_puts("playwav: SB16 not available ("); vga_puts(sb16_status()); vga_puts(")\n");
        return;
    }
    fs_node_t* f = fs_resolve(cwd, argv[1]);
    if (!f || f->type != FS_FILE || !f->data) {
        vga_puts("playwav: no such file\n"); return;
    }
    vga_printf("playwav: %u bytes, playing...\n", (uint32_t)f->size);
    int r = wav_play((const uint8_t*)f->data, (uint32_t)f->size);
    if (r != 0) vga_printf("playwav: %s (err %d)\n", wav_err_str(r), r);
}

static void cmd_stopwav(int argc, char** argv) {
    (void)argc; (void)argv;
    sb16_stop();
    vga_puts("playwav: stopped\n");
}

static void cmd_sbinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts("sb16: ");
    vga_puts(sb16_status());
    vga_putc('\n');
}

static void cmd_player(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) { vga_puts("player: enter 'desktop' first\n"); return; }
    if (mediaplayer_open() != 0) vga_puts("player: failed to open\n");
}

static void cmd_paint(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) { vga_puts("paint: enter 'desktop' first\n"); return; }
    if (paint_open() != 0) vga_puts("paint: failed to open\n");
}

static void cmd_clock(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) { vga_puts("clock: enter 'desktop' first\n"); return; }
    if (clock_open() != 0) vga_puts("clock: failed to open\n");
}

extern int  samara_doom_launch(void);
extern const char* samara_doom_status(void);

static void cmd_doom(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) {
        vga_puts("doom: enter 'desktop' first\n");
        return;
    }
    int r = samara_doom_launch();
    vga_puts("doom: ");
    vga_puts(samara_doom_status());
    vga_putc('\n');
    (void)r;
}

static void cmd_snake(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!gfx_ready()) { vga_puts("snake: enter 'desktop' first\n"); return; }
    if (snake_open() != 0) vga_puts("snake: failed to open window\n");
}

/* ===================== networking ===================== */

/* Parse "a.b.c.d" -> uint32_t (host byte order). 1 on success. */
static int parse_ipv4(const char* s, uint32_t* out) {
    uint32_t v = 0;
    int parts = 0, n = 0, have = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            if (n > 255) return 0;
            have = 1;
        } else if (*s == '.') {
            if (!have) return 0;
            v = (v << 8) | (uint32_t)n;
            n = 0; have = 0; parts++;
            if (parts > 3) return 0;
        } else {
            return 0;
        }
        s++;
    }
    if (!have || parts != 3) return 0;
    v = (v << 8) | (uint32_t)n;
    *out = v;
    return 1;
}

static void print_ip(uint32_t ip) {
    vga_printf("%u.%u.%u.%u",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8)  & 0xFF,  ip        & 0xFF);
}

static void print_mac(const uint8_t* m) {
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char a = hex[m[i] >> 4], b = hex[m[i] & 0xF];
        vga_putc(a); vga_putc(b);
        if (i < 5) vga_putc(':');
    }
}

static int ensure_net(const char* who) {
    if (net_ready()) return 0;
    int r = net_init();
    if (r != 0) {
        vga_puts(who); vga_puts(": "); vga_puts(net_status()); vga_putc('\n');
        return -1;
    }
    return 0;
}

static void cmd_ifconfig(int argc, char** argv) {
    (void)argc; (void)argv;
    if (ensure_net("ifconfig") != 0) return;
    vga_puts("eth0  HWaddr "); print_mac(net_mac()); vga_putc('\n');
    vga_puts("      inet "); print_ip(net_ip());
    vga_puts("  mask 255.255.255.0  gw "); print_ip(net_gw()); vga_putc('\n');
    vga_puts("      status: "); vga_puts(net_status()); vga_putc('\n');
}

static void cmd_ping(int argc, char** argv) {
    if (argc < 2) { vga_puts("ping: usage: ping <ipv4> [count]\n"); return; }
    if (ensure_net("ping") != 0) return;
    uint32_t ip;
    if (!parse_ipv4(argv[1], &ip)) {
        vga_puts("ping: bad IP (numeric only, no DNS)\n");
        return;
    }
    int count = (argc > 2) ? atoi(argv[2]) : 4;
    if (count < 1) count = 1;
    if (count > 32) count = 32;
    vga_puts("PING "); print_ip(ip); vga_puts(" 32 bytes\n");
    int sent = 0, recv_ok = 0, rtt_sum = 0;
    for (int i = 0; i < count; i++) {
        int rtt = net_ping(ip, 1500);
        sent++;
        if (rtt >= 0) {
            recv_ok++;
            rtt_sum += rtt;
            vga_printf("  seq=%d time=%d ms\n", i + 1, rtt);
        } else {
            vga_printf("  seq=%d timeout\n", i + 1);
        }
        /* small delay between pings */
        uint32_t until = pit_uptime_ms() + 250;
        while (pit_uptime_ms() < until) net_poll();
    }
    vga_printf("--- stats: %d sent, %d recv, %d%% loss",
               sent, recv_ok, sent ? (100 - 100 * recv_ok / sent) : 0);
    if (recv_ok) vga_printf(", avg %d ms", rtt_sum / recv_ok);
    vga_putc('\n');
}

/* tiny URL parser for "http://a.b.c.d[:port][/path]" — no DNS. */
static int parse_http_url(const char* url, uint32_t* ip, uint16_t* port, const char** path) {
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    char host[64]; int hn = 0;
    while (*p && *p != ':' && *p != '/' && hn < (int)sizeof(host) - 1) host[hn++] = *p++;
    host[hn] = 0;
    if (!parse_ipv4(host, ip)) return 0;
    *port = 80;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v < 1 || v > 65535) return 0;
        *port = (uint16_t)v;
    }
    if (*p == 0) *path = "/";
    else *path = p;       /* p points at '/' */
    return 1;
}

#define WGET_BUF (32 * 1024)
static uint8_t wget_buf[WGET_BUF];

static void cmd_wget(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("wget: usage: wget <http://a.b.c.d[:port]/path> [outfile]\n");
        return;
    }
    if (ensure_net("wget") != 0) return;
    uint32_t ip; uint16_t port; const char* path;
    if (!parse_http_url(argv[1], &ip, &port, &path)) {
        vga_puts("wget: bad URL (need numeric IP)\n");
        return;
    }
    vga_puts("wget: GET "); print_ip(ip); vga_printf(":%u%s\n", port, path);
    int n = net_http_get(ip, port, NULL, path, wget_buf, WGET_BUF);
    if (n < 0) { vga_printf("wget: failed (%d) — %s\n", n, net_status()); return; }
    vga_printf("wget: got %d bytes\n", n);

    /* strip HTTP headers: find "\r\n\r\n" */
    int body_off = 0;
    for (int i = 0; i + 3 < n; i++) {
        if (wget_buf[i] == '\r' && wget_buf[i+1] == '\n' &&
            wget_buf[i+2] == '\r' && wget_buf[i+3] == '\n') {
            body_off = i + 4; break;
        }
    }
    int body_len = n - body_off;

    if (argc >= 3) {
        const char* outname = argv[2];
        fs_node_t* f = fs_resolve(cwd, outname);
        if (!f) f = fs_create(cwd, outname, FS_FILE);
        if (!f || f->type != FS_FILE) {
            vga_puts("wget: cannot create output file\n"); return;
        }
        fs_write(f, (const char*)(wget_buf + body_off), body_len);
        vga_printf("wget: wrote %d bytes to %s\n", body_len, outname);
    } else {
        /* print body (up to 4KB) for quick test */
        int show = body_len < 4096 ? body_len : 4096;
        for (int i = 0; i < show; i++) {
            char c = (char)wget_buf[body_off + i];
            if (c == '\r') continue;
            if (c == '\n' || (c >= 0x20 && c < 0x7F)) vga_putc(c);
            else vga_putc('.');
        }
        if (body_len > show) vga_printf("\n[...%d more bytes truncated]\n", body_len - show);
        else vga_putc('\n');
    }
}

/* ===================== desktop entry ===================== */

static void cmd_desktop(int argc, char** argv) {
    (void)argc; (void)argv;

    if (!desktop_init_graphics()) {
        vga_puts("desktop: no graphics available\n");
        return;
    }

    int prev_max_x, prev_max_y;
    mouse_get_range(&prev_max_x, &prev_max_y);
    mouse_set_text_cursor(false);
    mouse_set_range(gfx_w() - 1, gfx_h() - 1);
    mouse_set_pos(gfx_w() / 2, gfx_h() / 2);

    wm_init();
    /* open a Welcome info window and a Terminal so user sees something */
    static const char welcome[] =
        "Welcome to SamaraOS Desktop!\n"
        "\n"
        "* Drag windows by their title bar.\n"
        "* Click X (red) to close a window.\n"
        "* Open more from the Start menu.\n"
        "* Esc returns to text shell.\n";
    wm_open_info(40, 40, 440, 180, "Welcome", welcome);
    wm_open_terminal(80, 80);

    wm_run();

    /* cleanup */
    mediaplayer_force_stop();
    g_in_wm_terminal = false;
    g_current_term_window = NULL;
    vga_use_text();
    vga_set_text_mode_3();
    font_restore();
    mouse_set_text_cursor(true);
    mouse_set_range(prev_max_x, prev_max_y);
    vga_init();
    vga_set_color(VGA_LCYAN, VGA_BLACK);
    vga_puts("returned from desktop.\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);
}

/* ===================== dispatch ===================== */

typedef struct { const char* name; void (*fn)(int, char**); } cmd_t;
static cmd_t cmds[] = {
    {"help", cmd_help}, {"clear", cmd_clear}, {"echo", cmd_echo},
    {"pwd", cmd_pwd}, {"ls", cmd_ls}, {"cd", cmd_cd}, {"cat", cmd_cat},
    {"mkdir", cmd_mkdir}, {"touch", cmd_touch}, {"rm", cmd_rm},
    {"cp", cmd_cp}, {"mv", cmd_mv},
    {"head", cmd_head}, {"tail", cmd_tail}, {"wc", cmd_wc},
    {"grep", cmd_grep}, {"find", cmd_find},
    {"uptime", cmd_uptime}, {"mem", cmd_mem}, {"df", cmd_df},
    {"ps", cmd_ps}, {"mouse", cmd_mouse},
    {"uname", cmd_uname}, {"whoami", cmd_whoami},
    {"about", cmd_about}, {"date", cmd_date},
    {"history", cmd_history}, {"exit", cmd_exit},
    {"sleep", cmd_sleep}, {"shutdown", cmd_shutdown},
    {"reboot", cmd_reboot}, {"beep", cmd_beep}, {"calc", cmd_calc},
    {"nano", cmd_nano}, {"neofetch", cmd_neofetch},
    {"desktop", cmd_desktop}, {"bounce", cmd_bounce},
    {"disk", cmd_disk},
    {"doom", cmd_doom}, {"doom2", cmd_doom},
    {"doom-mini", cmd_doom_mini}, {"play", cmd_play},
    {"snake", cmd_snake},
    {"ifconfig", cmd_ifconfig}, {"ping", cmd_ping}, {"wget", cmd_wget},
    {"player", cmd_player}, {"music", cmd_player},
    {"paint", cmd_paint}, {"clock", cmd_clock},
    {"playwav", cmd_playwav}, {"stopwav", cmd_stopwav}, {"sbinfo", cmd_sbinfo},
    {"fatmount", cmd_fatmount}, {"fatls", cmd_fatls}, {"fatload", cmd_fatload},
    {"playfat", cmd_playfat},
    {NULL, NULL}
};

static void execute(char* line) {
    char* argv[16];
    int argc = split(line, argv, 16);
    if (!argc) return;
    for (cmd_t* c = cmds; c->name; c++) {
        if (!strcmp(c->name, argv[0])) { c->fn(argc, argv); return; }
    }
    vga_puts(argv[0]); vga_puts(": command not found (try 'help')\n");
}

void shell_run(void) {
    cwd = fs_resolve(fs_root(), "/home/user");
    if (!cwd) cwd = fs_root();

    vga_set_color(VGA_LCYAN, VGA_BLACK);
    vga_puts("\nSamaraOS shell. Type 'help' or 'desktop'.\n\n");
    vga_set_color(VGA_LGREY, VGA_BLACK);

    char line[LINE_MAX];
    while (!shell_request_exit) {
        prompt();
        readline(line, LINE_MAX);
        if (shell_request_exit) break;
        execute(line);
    }
    shell_request_exit = 0;
}
