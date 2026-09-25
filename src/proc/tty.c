#include "proc/tty.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/task.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "boot/pit.h"
#include "core/io.h"

#define IN_SZ   2048
#define OUT_SZ  16384
#define LINE_SZ 512

static char     in_buf[IN_SZ];
static int      in_head, in_tail;           /* ready for read() */
static char     line[LINE_SZ];
static int      line_len;                   /* canonical edit buffer */
static int      eof_pending;
static char     out_buf[OUT_SZ];
static volatile int out_head, out_tail;
static ktermios_t tio;
static int      fg_pgrp;

/* ---------------- rings ---------------- */

static int in_count(void) { return (in_head - in_tail + IN_SZ) % IN_SZ; }

static void in_push(char c) {
    int n = (in_head + 1) % IN_SZ;
    if (n == in_tail) return;
    in_buf[in_head] = c;
    in_head = n;
}

static void in_push_str(const char* s) { while (*s) in_push(*s++); }

static bool out_push(char c) {
    int n = (out_head + 1) % OUT_SZ;
    if (n == out_tail) return false;
    out_buf[out_head] = c;
    out_head = n;
    return true;
}

static void echo(char c) { out_push(c); }
static void echo_str(const char* s) { while (*s) out_push(*s++); }

/* ---------------- setup ---------------- */

static void render_reset(void);

void tty_reset(void) {
    uint32_t f = irq_save();
    in_head = in_tail = 0;
    line_len = 0;
    eof_pending = 0;
    memset(&tio, 0, sizeof(tio));
    tio.c_iflag = TTY_ICRNL;
    tio.c_oflag = TTY_OPOST | TTY_ONLCR;
    tio.c_cflag = 0x00BF;                   /* B38400 | CS8 | CREAD */
    tio.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | 0x0020 /*ECHOK*/ | 0x8000 /*IEXTEN*/;
    tio.c_cc[VINTR] = 3;
    tio.c_cc[VQUIT] = 0x1C;
    tio.c_cc[VERASE] = 0x7F;
    tio.c_cc[VKILL] = 0x15;
    tio.c_cc[VEOF] = 4;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    render_reset();
    irq_restore(f);
}

void tty_init(void) {
    out_head = out_tail = 0;
    fg_pgrp = 0;
    tty_reset();
}

void tty_get_termios(ktermios_t* t) { *t = tio; }
void tty_set_termios(const ktermios_t* t) {
    uint32_t f = irq_save();
    bool was_canon = tio.c_lflag & TTY_ICANON;
    tio = *t;
    /* Leaving canonical mode hands any half-typed line to the reader. */
    if (was_canon && !(tio.c_lflag & TTY_ICANON)) {
        for (int i = 0; i < line_len; i++) in_push(line[i]);
        line_len = 0;
    }
    irq_restore(f);
}
int  tty_fg_pgrp(void) { return fg_pgrp; }
void tty_set_fg_pgrp(int pgrp) { fg_pgrp = pgrp; }
void tty_winsize(int* rows, int* cols) { *rows = VGA_HEIGHT; *cols = VGA_WIDTH; }

/* ---------------- process side ---------------- */

bool tty_readable(void) { return in_count() > 0 || eof_pending; }

int tty_read(char* buf, int n, bool nonblock) {
    if (n <= 0) return 0;
    uint32_t deadline = 0;
    for (;;) {
        uint32_t f = irq_save();
        bool canon = tio.c_lflag & TTY_ICANON;
        if (in_count() > 0) {
            int got = 0;
            while (got < n && in_count() > 0) {
                char c = in_buf[in_tail];
                in_tail = (in_tail + 1) % IN_SZ;
                buf[got++] = c;
                if (canon && c == '\n') break;
            }
            irq_restore(f);
            return got;
        }
        if (eof_pending) { eof_pending = 0; irq_restore(f); return 0; }
        uint8_t vmin = tio.c_cc[VMIN], vtime = tio.c_cc[VTIME];
        irq_restore(f);

        if (nonblock) return -11;                        /* EAGAIN */
        if (!canon && vmin == 0) {
            if (vtime == 0) return 0;
            if (!deadline) deadline = pit_uptime_ms() + vtime * 100u;
            else if ((int32_t)(pit_uptime_ms() - deadline) >= 0) return 0;
        }
        if (proc_interrupted()) return -4;               /* EINTR */
        task_yield();
    }
}

int tty_write(const char* buf, int n) {
    uint32_t waited_from = 0;
    for (int i = 0; i < n; i++) {
        while (!out_push(buf[i])) {
            /* Nobody draining (console detached)? Drop instead of hanging. */
            if (!waited_from) waited_from = pit_uptime_ms();
            else if (pit_uptime_ms() - waited_from > 2000) return n;
            if (proc_interrupted()) return i ? i : -4;
            task_yield();
        }
    }
    return n;
}

/* ---------------- keyboard side ---------------- */

static void send_sig(int sig) {
    if (fg_pgrp) proc_signal_group(fg_pgrp, sig);
}

static const char* key_seq(uint8_t k) {
    switch (k) {
        case K_UP:    return "\x1b[A";
        case K_DOWN:  return "\x1b[B";
        case K_RIGHT: return "\x1b[C";
        case K_LEFT:  return "\x1b[D";
        case K_HOME:  return "\x1b[1~";                 /* terminfo "linux" */
        case K_END:   return "\x1b[4~";
        case K_DEL:   return "\x1b[3~";
        case K_PGUP:  return "\x1b[5~";
        case K_PGDN:  return "\x1b[6~";
    }
    if (k >= K_F1 && k < K_F1 + 5) {                   /* linux: F1..F5 = ESC [ [ A..E */
        static char f[5] = "\x1b[[?";
        f[3] = (char)('A' + (k - K_F1));
        return f;
    }
    return NULL;
}

void tty_key(char ch) {
    uint8_t c = (uint8_t)ch;
    uint32_t f = irq_save();
    bool canon = tio.c_lflag & TTY_ICANON;
    bool echo_on = tio.c_lflag & TTY_ECHO;

    const char* seq = key_seq(c);
    if (seq) {
        if (!canon) in_push_str(seq);
        irq_restore(f);
        return;
    }
    if (c == '\b') c = 0x7F;                     /* Backspace key -> DEL (VERASE) */
    if (c == '\n') c = '\r';                     /* Enter sends CR, like a real terminal */
    if (c == '\r' && (tio.c_iflag & TTY_ICRNL)) c = '\n';

    if ((tio.c_lflag & TTY_ISIG) && (c == tio.c_cc[VINTR] || c == tio.c_cc[VQUIT])) {
        line_len = 0;
        if (echo_on) echo_str(c == 3 ? "^C\n" : "^\\\n");
        irq_restore(f);
        send_sig(c == tio.c_cc[VINTR] ? 2 : 3);
        return;
    }

    if (!canon) {
        in_push((char)c);
        if (echo_on) echo((char)c);
        irq_restore(f);
        return;
    }

    if (c == tio.c_cc[VERASE] || c == 0x7F) {
        if (line_len > 0) {
            line_len--;
            if (echo_on) echo_str("\b \b");
        }
    } else if (c == tio.c_cc[VKILL]) {
        while (line_len > 0) { line_len--; if (echo_on) echo_str("\b \b"); }
    } else if (c == tio.c_cc[VEOF]) {
        if (line_len == 0) eof_pending = 1;
        for (int i = 0; i < line_len; i++) in_push(line[i]);
        line_len = 0;
    } else if (c == '\n') {
        for (int i = 0; i < line_len; i++) in_push(line[i]);
        in_push('\n');
        line_len = 0;
        if (echo_on || (tio.c_lflag & 0x40 /*ECHONL*/)) echo('\n');
    } else if (c >= 0x20 && c < 0x7F && line_len < LINE_SZ - 1) {
        line[line_len++] = (char)c;
        if (echo_on) echo((char)c);
    }
    irq_restore(f);
}

/* ---------------- ANSI renderer (console side) ----------------
   Implements what terminfo "linux" (TERM=linux) asks of a terminal, so
   curses programs (nano, vi, top, less) draw correctly: scroll regions,
   insert/delete lines and characters, reverse index, deferred wrap. */

enum { ST_NORMAL, ST_ESC, ST_CSI, ST_SKIP1, ST_OSC, ST_OSC_ESC, ST_G0, ST_G1 };
static int  st = ST_NORMAL;
static int  par[16], npar;
static bool par_priv;
static int  fg = VGA_LGREY, bg = VGA_BLACK;
static bool bold, reverse, acs;
static bool g0_gfx, g1_gfx, shift_out;     /* charsets: ESC ( 0, ESC ) 0, ^N/^O */
static int  saved_x, saved_y;
static bool need_color;
static int  reg_top, reg_bot = VGA_HEIGHT - 1;      /* scroll region, inclusive */

static void render_reset(void) {
    st = ST_NORMAL;
    fg = VGA_LGREY; bg = VGA_BLACK;
    bold = reverse = acs = false;
    g0_gfx = g1_gfx = shift_out = false;
    reg_top = 0; reg_bot = VGA_HEIGHT - 1;
    need_color = true;
}

static const uint8_t ansi2vga[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN, VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LGREY
};

static void apply_color(void) {
    int f = fg, b = bg;
    if (bold && f < 8) f += 8;
    if (reverse) { int t = f; f = b; b = t; }
    vga_set_color((uint8_t)f, (uint8_t)b);
}

static void sgr(void) {
    if (npar == 0) { par[0] = 0; npar = 1; }
    for (int i = 0; i < npar; i++) {
        int p = par[i];
        if (p == 0) { fg = VGA_LGREY; bg = VGA_BLACK; bold = reverse = false; }
        else if (p == 1) bold = true;
        else if (p == 2 || p == 22) bold = false;
        else if (p == 7) reverse = true;
        else if (p == 27) reverse = false;
        else if (p >= 30 && p <= 37) fg = ansi2vga[p - 30];
        else if (p == 38 && i + 2 < npar && par[i + 1] == 5) { fg = par[i + 2] & 15; i += 2; }
        else if (p == 39) fg = VGA_LGREY;
        else if (p >= 40 && p <= 47) bg = ansi2vga[p - 40];
        else if (p == 48 && i + 2 < npar && par[i + 1] == 5) { bg = par[i + 2] & 7; i += 2; }
        else if (p == 49) bg = VGA_BLACK;
        else if (p >= 90 && p <= 97) fg = ansi2vga[p - 90] + 8;
        else if (p >= 100 && p <= 107) bg = ansi2vga[p - 100] + 8;
    }
    apply_color();
}

static int P(int i, int def) { return (i < npar && par[i] > 0) ? par[i] : def; }

static void cursor(int* x, int* y) {
    vga_get_cursor(x, y);
    if (*y >= VGA_HEIGHT) *y = VGA_HEIGHT - 1;
}

/* Line feed: scrolls the region when leaving its bottom line. */
static void line_feed(bool cr) {
    int x, y;
    cursor(&x, &y);
    if (y == reg_bot) vga_scroll_region(reg_top, reg_bot, 1);
    else if (y < VGA_HEIGHT - 1) y++;
    vga_set_cursor(cr ? 0 : (x >= VGA_WIDTH ? VGA_WIDTH - 1 : x), y);
}

static void reverse_index(void) {
    int x, y;
    cursor(&x, &y);
    if (y == reg_top) vga_scroll_region(reg_top, reg_bot, -1);
    else if (y > 0) y--;
    vga_set_cursor(x >= VGA_WIDTH ? VGA_WIDTH - 1 : x, y);
}

static void csi(char final) {
    int x, y;
    cursor(&x, &y);
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    switch (final) {
        case 'm': sgr(); break;
        case 'A': vga_set_cursor(x, y - P(0, 1)); break;
        case 'B': case 'e': vga_set_cursor(x, y + P(0, 1)); break;
        case 'C': case 'a': vga_set_cursor(x + P(0, 1), y); break;
        case 'D': vga_set_cursor(x - P(0, 1), y); break;
        case 'E': vga_set_cursor(0, y + P(0, 1)); break;
        case 'F': vga_set_cursor(0, y - P(0, 1)); break;
        case 'G': case '`': vga_set_cursor(P(0, 1) - 1, y); break;
        case 'd': vga_set_cursor(x, P(0, 1) - 1); break;
        case 'H': case 'f': vga_set_cursor(P(1, 1) - 1, P(0, 1) - 1); break;
        case 'K': {
            int mode = npar ? par[0] : 0;
            int x0 = mode == 0 ? x : 0, x1 = mode == 1 ? x + 1 : VGA_WIDTH;
            vga_erase(x0, y, x1);
            break;
        }
        case 'J': {
            int mode = npar ? par[0] : 0;
            if (mode == 2 || mode == 3) {
                for (int r = 0; r < VGA_HEIGHT; r++) vga_erase(0, r, VGA_WIDTH);
            } else if (mode == 0) {
                vga_erase(x, y, VGA_WIDTH);
                for (int r = y + 1; r < VGA_HEIGHT; r++) vga_erase(0, r, VGA_WIDTH);
            } else {
                for (int r = 0; r < y; r++) vga_erase(0, r, VGA_WIDTH);
                vga_erase(0, y, x + 1);
            }
            break;
        }
        case 'X': vga_erase(x, y, x + P(0, 1)); break;
        case '@': vga_shift_chars(x, y, P(0, 1)); break;
        case 'P': vga_shift_chars(x, y, -P(0, 1)); break;
        case 'L':                                           /* insert lines */
            if (y >= reg_top && y <= reg_bot) vga_scroll_region(y, reg_bot, -P(0, 1));
            vga_set_cursor(0, y);
            break;
        case 'M':                                           /* delete lines */
            if (y >= reg_top && y <= reg_bot) vga_scroll_region(y, reg_bot, P(0, 1));
            vga_set_cursor(0, y);
            break;
        case 'S': vga_scroll_region(reg_top, reg_bot, P(0, 1)); break;
        case 'T': vga_scroll_region(reg_top, reg_bot, -P(0, 1)); break;
        case 'r': {                                         /* DECSTBM */
            int t = P(0, 1) - 1, b = P(1, VGA_HEIGHT) - 1;
            if (b >= VGA_HEIGHT) b = VGA_HEIGHT - 1;
            if (t < b) { reg_top = t; reg_bot = b; }
            vga_set_cursor(0, 0);
            break;
        }
        case 'n':
            if (P(0, 0) == 6) {
                char rep[24], num[12];
                int p = 0;
                rep[p++] = 0x1b; rep[p++] = '[';
                itoa(y + 1, num, 10); for (char* q = num; *q; q++) rep[p++] = *q;
                rep[p++] = ';';
                itoa(x + 1, num, 10); for (char* q = num; *q; q++) rep[p++] = *q;
                rep[p++] = 'R'; rep[p] = 0;
                uint32_t f = irq_save();
                in_push_str(rep);
                irq_restore(f);
            }
            break;
        case 's': saved_x = x; saved_y = y; break;
        case 'u': vga_set_cursor(saved_x, saved_y); break;
        default: break;                                     /* h/l/c/...: ignored */
    }
}

/* VT100 special graphics (ESC(0 / smacs) -> CP437 box drawing. */
static char acs_map(char c) {
    switch (c) {
        case 'j': return (char)0xD9; case 'k': return (char)0xBF; case 'l': return (char)0xDA;
        case 'm': return (char)0xC0; case 'n': return (char)0xC5; case 'q': return (char)0xC4;
        case 't': return (char)0xC3; case 'u': return (char)0xB4; case 'v': return (char)0xC1;
        case 'w': return (char)0xC2; case 'x': return (char)0xB3; case 'a': return (char)0xB1;
        case '`': return (char)0x04; case 'f': return (char)0xF8; case 'g': return (char)0xF1;
        case '~': return (char)0xF9; case '0': return (char)0xDB; case 'h': return (char)0xB0;
    }
    return c;
}

static void render(char c) {
    switch (st) {
        case ST_ESC:
            st = ST_NORMAL;
            if (c == '[') { st = ST_CSI; npar = 0; par_priv = false; par[0] = 0; return; }
            if (c == ']') { st = ST_OSC; return; }
            if (c == '(') { st = ST_G0; return; }
            if (c == ')') { st = ST_G1; return; }
            if (c == '#' || c == '%') { st = ST_SKIP1; return; }
            if (c == '7') { vga_get_cursor(&saved_x, &saved_y); }
            else if (c == '8') { vga_set_cursor(saved_x, saved_y); }
            else if (c == 'c') { render_reset(); apply_color(); vga_clear(); }
            else if (c == 'M') reverse_index();
            else if (c == 'D') line_feed(false);
            else if (c == 'E') line_feed(true);
            return;
        case ST_SKIP1:
            st = ST_NORMAL;
            return;
        case ST_G0: case ST_G1:
            if (st == ST_G0) g0_gfx = c == '0'; else g1_gfx = c == '0';
            acs = shift_out ? g1_gfx : g0_gfx;
            st = ST_NORMAL;
            return;
        case ST_OSC:                                        /* OSC ... BEL | ESC \ */
            if (c == '\a') st = ST_NORMAL;
            else if (c == 0x1b) st = ST_OSC_ESC;
            return;
        case ST_OSC_ESC:
            st = c == '\\' ? ST_NORMAL : ST_OSC;
            return;
        case ST_CSI:
            if (c >= '0' && c <= '9') {
                if (npar == 0) npar = 1;
                if (par[npar - 1] < 10000) par[npar - 1] = par[npar - 1] * 10 + (c - '0');
                return;
            }
            if (c == ';') {
                if (npar == 0) npar = 1;
                if (npar < 16) par[npar++] = 0;
                return;
            }
            if (c == '?' || c == '>' || c == '=' || c == '!') { par_priv = true; return; }
            if (c >= 0x40 && c <= 0x7E) { if (!par_priv) csi(c); st = ST_NORMAL; }
            return;
        default:
            break;
    }
    int x, y;
    switch (c) {
        case 0x1b: st = ST_ESC; return;
        case '\a': case 0: return;
        case 0x0E: shift_out = true;  acs = g1_gfx; return;   /* SO: G1 */
        case 0x0F: shift_out = false; acs = g0_gfx; return;   /* SI: G0 */
        case '\r': cursor(&x, &y); vga_set_cursor(0, y); return;
        case '\n': case '\v': case '\f':
            line_feed((tio.c_oflag & TTY_OPOST) && (tio.c_oflag & TTY_ONLCR));
            return;
        case '\b':                                          /* move left, never erase */
            cursor(&x, &y);
            if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
            if (x > 0) vga_set_cursor(x - 1, y);
            return;
        case '\t':
            cursor(&x, &y);
            x = (x + 8) & ~7;
            vga_set_cursor(x >= VGA_WIDTH ? VGA_WIDTH - 1 : x, y);
            return;
    }
    cursor(&x, &y);
    if (x >= VGA_WIDTH) line_feed(true);                    /* deferred autowrap */
    vga_putc(acs ? acs_map(c) : c);
}

bool tty_has_output(void) { return out_head != out_tail; }

bool tty_serial_mirror;             /* copy program output to COM1 (autotests) */

static void serial_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) {}
    outb(0x3F8, c);
}

void tty_pump(void) {
    if (need_color) { apply_color(); need_color = false; }
    int budget = 8192;                           /* keep the UI responsive */
    while (out_tail != out_head && budget--) {
        char c = out_buf[out_tail];
        out_tail = (out_tail + 1) % OUT_SZ;
        if (tty_serial_mirror) { if (c == '\n') serial_putc('\r'); serial_putc(c); }
        render(c);
    }
}
