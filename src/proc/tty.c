#include "proc/tty.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/task.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"
#include "gfx/gfx_term.h"
#include "gfx/termfont.h"
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
static int      reader_pid;                 /* last process blocked in read() */
static bool     fullscreen;                 /* raw mode + cursor addressing seen */

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
    fullscreen = false;
    reader_pid = 0;
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
    if (was_canon != !!(tio.c_lflag & TTY_ICANON)) fullscreen = false;
    /* Leaving canonical mode hands any half-typed line to the reader. */
    if (was_canon && !(tio.c_lflag & TTY_ICANON)) {
        for (int i = 0; i < line_len; i++) in_push(line[i]);
        line_len = 0;
    }
    irq_restore(f);
}
int  tty_fg_pgrp(void) { return fg_pgrp; }
void tty_set_fg_pgrp(int pgrp) { fg_pgrp = pgrp; }
void tty_winsize(int* rows, int* cols) { *rows = vga_rows(); *cols = vga_cols(); }

/* ---------------- process side ---------------- */

bool tty_readable(void) { return in_count() > 0 || eof_pending; }

int tty_read(char* buf, int n, bool nonblock) {
    if (n <= 0) return 0;
    proc_t* me = proc_current();
    if (me && me->tty_detached) return 0;          /* background job: EOF */
    if (me) reader_pid = me->pid;
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
    proc_t* me = proc_current();
    if (me && me->tty_detached) return n;          /* background job: dropped */
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

static void send_sig(int sig) {
    if (fg_pgrp) proc_signal_group(fg_pgrp, sig);
}

/* Mouse wheel over the console, dz > 0 = towards the user (scroll down).
   Full-screen programs get keys, like xterm's alternateScroll: nano its
   own scroll-without-moving-the-cursor (Alt+Up/Down, ESC [1;3A/B), the
   rest plain arrows. Returns false for line-mode output, where the
   console should move its scrollback instead. */
bool tty_wheel(int dz) {
    uint32_t f = irq_save();
    if ((tio.c_lflag & TTY_ICANON) || !fullscreen || !dz) { irq_restore(f); return false; }
    proc_t* r = reader_pid ? proc_by_pid(reader_pid) : NULL;
    bool nano = r && !strcmp(r->name, "nano");
    const char* seq = nano ? (dz < 0 ? "\x1b[1;3A" : "\x1b[1;3B")
                           : (dz < 0 ? "\x1b[A" : "\x1b[B");
    int n = (dz < 0 ? -dz : dz) * 3;
    if (n > 30) n = 30;
    for (int i = 0; i < n; i++) in_push_str(seq);
    irq_restore(f);
    return true;
}

bool tty_fullscreen(void) { return fullscreen && !(tio.c_lflag & TTY_ICANON); }

/* ---------------- keyboard side ---------------- */

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

    /* Programs speak UTF-8; the keyboard gives CP866 for non-ASCII. */
    char u8[4];
    int nu = 1;
    u8[0] = (char)c;
    if (c >= 0x80) {
        uint32_t cp = cp866_to_uni(c);
        if (cp < 0x800) { u8[0] = (char)(0xC0 | (cp >> 6)); u8[1] = (char)(0x80 | (cp & 0x3F)); nu = 2; }
        else { u8[0] = (char)(0xE0 | (cp >> 12)); u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
               u8[2] = (char)(0x80 | (cp & 0x3F)); nu = 3; }
    }

    if (!canon) {
        for (int i = 0; i < nu; i++) { in_push(u8[i]); if (echo_on) echo(u8[i]); }
        irq_restore(f);
        return;
    }

    if (c == tio.c_cc[VERASE] || c == 0x7F) {
        if (line_len > 0) {                      /* a whole UTF-8 character */
            while (line_len > 1 && ((uint8_t)line[line_len - 1] & 0xC0) == 0x80) line_len--;
            line_len--;
            if (echo_on) echo_str("\b \b");
        }
    } else if (c == tio.c_cc[VKILL]) {
        while (line_len > 0) {
            while (line_len > 1 && ((uint8_t)line[line_len - 1] & 0xC0) == 0x80) line_len--;
            line_len--;
            if (echo_on) echo_str("\b \b");
        }
    } else if (c == tio.c_cc[VEOF]) {
        if (line_len == 0) eof_pending = 1;
        for (int i = 0; i < line_len; i++) in_push(line[i]);
        line_len = 0;
    } else if (c == '\n') {
        for (int i = 0; i < line_len; i++) in_push(line[i]);
        in_push('\n');
        line_len = 0;
        if (echo_on || (tio.c_lflag & 0x40 /*ECHONL*/)) echo('\n');
    } else if ((c >= 0x20 && c != 0x7F) && line_len < LINE_SZ - nu) {
        for (int i = 0; i < nu; i++) { line[line_len++] = u8[i]; if (echo_on) echo(u8[i]); }
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
/* 24-bit colour from SGR 38;2 / 48;2 or a 256-colour index >= 16;
   -1 = use the 16-colour fg/bg. */
static int32_t fg_rgb = -1, bg_rgb = -1;
static bool par_priv, par_q;             /* CSI with a private marker; '?' (DEC modes) */
static uint8_t sgr_at;                    /* TF_BOLD | TF_UNDERLINE | ... */
static int  utf_need, utf_n;              /* UTF-8 decoding */
static uint32_t utf_cp;
static uint8_t  utf_raw[4];
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
    fg_rgb = bg_rgb = -1;
    sgr_at = 0;
    utf_need = 0;
    g0_gfx = g1_gfx = shift_out = false;
    reg_top = 0; reg_bot = vga_rows() - 1;
    need_color = true;
}

/* The console grid changed size: full-height scroll region again, and
   tell the foreground job (curses programs redraw on SIGWINCH). */
void tty_resized(void) {
    uint32_t f = irq_save();
    reg_top = 0;
    reg_bot = vga_rows() - 1;
    irq_restore(f);
    send_sig(28);                               /* SIGWINCH */
}


static const uint8_t ansi2vga[8] = {
    VGA_BLACK, VGA_RED, VGA_GREEN, VGA_BROWN, VGA_BLUE, VGA_MAGENTA, VGA_CYAN, VGA_LGREY
};

/* xterm's 256-colour palette above the 16 base colours */
static uint32_t xterm256(int n) {
    if (n >= 232) { uint32_t v = (uint32_t)(8 + (n - 232) * 10); return (v << 16) | (v << 8) | v; }
    n -= 16;
    static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
    return ((uint32_t)lv[(n / 36) % 6] << 16) | ((uint32_t)lv[(n / 6) % 6] << 8) | lv[n % 6];
}

static void apply_color(void) {
    int f = fg, b = bg;
    if (bold && f < 8) f += 8;
    if (fg_rgb < 0 && bg_rgb < 0) {
        if (reverse) { int t = f; f = b; b = t; }
        vga_set_color((uint8_t)f, (uint8_t)b);
        vga_set_attr(sgr_at);
        return;
    }
    vga_set_attr(sgr_at);
    uint32_t F = fg_rgb >= 0 ? (uint32_t)fg_rgb : gfx_term_color(f);
    uint32_t B = bg_rgb >= 0 ? (uint32_t)bg_rgb : gfx_term_color(b);
    if (reverse) { uint32_t t = F; F = B; B = t; }
    vga_set_rgb(F, B);
}

/* 38/48 ; 5;n | 2;r;g;b  starting at par[i]; returns params consumed after i */
static int ext_color(int i, bool is_fg) {
    if (i + 1 >= npar) return 0;
    if (par[i + 1] == 5 && i + 2 < npar) {
        int n = par[i + 2] & 255;
        if (n < 16) {
            int v = n < 8 ? ansi2vga[n] : ansi2vga[n - 8] + 8;
            if (is_fg) { fg = v; fg_rgb = -1; } else { bg = v; bg_rgb = -1; }
        } else if (is_fg) fg_rgb = (int32_t)xterm256(n);
        else              bg_rgb = (int32_t)xterm256(n);
        return 2;
    }
    if (par[i + 1] == 2 && i + 4 < npar) {
        int32_t c = ((par[i + 2] & 255) << 16) | ((par[i + 3] & 255) << 8) | (par[i + 4] & 255);
        if (is_fg) fg_rgb = c; else bg_rgb = c;
        return 4;
    }
    return 0;
}

static void sgr(void) {
    if (npar == 0) { par[0] = 0; npar = 1; }
    for (int i = 0; i < npar; i++) {
        int p = par[i];
        if (p == 0) { fg = VGA_LGREY; bg = VGA_BLACK; bold = reverse = false; fg_rgb = bg_rgb = -1; sgr_at = 0; }
        else if (p == 1) { bold = true; sgr_at |= TF_BOLD; }
        else if (p == 2) sgr_at |= TF_DIM;
        else if (p == 22) { bold = false; sgr_at &= ~(TF_BOLD | TF_DIM); }
        else if (p == 3) sgr_at |= TF_ITALIC;
        else if (p == 23) sgr_at &= ~TF_ITALIC;
        else if (p == 4 || p == 21) sgr_at |= TF_UNDERLINE;
        else if (p == 24) sgr_at &= ~TF_UNDERLINE;
        else if (p == 9) sgr_at |= TF_STRIKE;
        else if (p == 29) sgr_at &= ~TF_STRIKE;
        else if (p == 7) reverse = true;
        else if (p == 27) reverse = false;
        else if (p >= 30 && p <= 37) { fg = ansi2vga[p - 30]; fg_rgb = -1; }
        else if (p == 38) i += ext_color(i, true);
        else if (p == 39) { fg = VGA_LGREY; fg_rgb = -1; }
        else if (p >= 40 && p <= 47) { bg = ansi2vga[p - 40]; bg_rgb = -1; }
        else if (p == 48) i += ext_color(i, false);
        else if (p == 49) { bg = VGA_BLACK; bg_rgb = -1; }
        else if (p >= 90 && p <= 97) { fg = ansi2vga[p - 90] + 8; fg_rgb = -1; }
        else if (p >= 100 && p <= 107) { bg = ansi2vga[p - 100] + 8; bg_rgb = -1; }
    }
    apply_color();
}

static int P(int i, int def) { return (i < npar && par[i] > 0) ? par[i] : def; }

static void cursor(int* x, int* y) {
    vga_get_cursor(x, y);
    if (*y >= vga_rows()) *y = vga_rows() - 1;
}

/* Line feed: scrolls the region when leaving its bottom line. */
static void line_feed(bool cr) {
    int x, y;
    cursor(&x, &y);
    if (y == reg_bot) vga_scroll_region(reg_top, reg_bot, 1);
    else if (y < vga_rows() - 1) y++;
    vga_set_cursor(cr ? 0 : (x >= vga_cols() ? vga_cols() - 1 : x), y);
}

static void reverse_index(void) {
    int x, y;
    cursor(&x, &y);
    if (y == reg_top) vga_scroll_region(reg_top, reg_bot, -1);
    else if (y > 0) y--;
    vga_set_cursor(x >= vga_cols() ? vga_cols() - 1 : x, y);
}

static void csi(char final) {
    int x, y;
    cursor(&x, &y);
    /* Absolute cursor moves / scroll regions from a raw-mode program mean
       a full-screen UI (nano, vi, less, top): the wheel sends it keys. */
    if ((final == 'H' || final == 'f' || final == 'r') && !(tio.c_lflag & TTY_ICANON))
        fullscreen = true;
    if (x >= vga_cols()) x = vga_cols() - 1;
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
            int x0 = mode == 0 ? x : 0, x1 = mode == 1 ? x + 1 : vga_cols();
            vga_erase(x0, y, x1);
            break;
        }
        case 'J': {
            int mode = npar ? par[0] : 0;
            if (mode == 2 || mode == 3) {
                for (int r = 0; r < vga_rows(); r++) vga_erase(0, r, vga_cols());
            } else if (mode == 0) {
                vga_erase(x, y, vga_cols());
                for (int r = y + 1; r < vga_rows(); r++) vga_erase(0, r, vga_cols());
            } else {
                for (int r = 0; r < y; r++) vga_erase(0, r, vga_cols());
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
            int t = P(0, 1) - 1, b = P(1, vga_rows()) - 1;
            if (b >= vga_rows()) b = vga_rows() - 1;
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

/* DEC private modes: cursor visibility, the alternate screen. */
static void dec_mode(bool set) {
    for (int i = 0; i < (npar ? npar : 1); i++) {
        int m = par[i];
        if (m == 25) vga_cursor_visible(set);
        else if (m == 1049 || m == 47 || m == 1047) vga_alt_screen(set);
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

static void emit(uint32_t cp) {
    int x, y;
    cursor(&x, &y);
    if (x >= vga_cols()) line_feed(true);                    /* deferred autowrap */
    vga_putu(cp);
}

/* A broken UTF-8 sequence: show its bytes as CP866, like before UTF-8. */
static void utf_flush(void) {
    for (int i = 0; i < utf_n; i++) emit(cp866_to_uni(utf_raw[i]));
    utf_need = utf_n = 0;
}

static void render(char c) {
    if (utf_need && (st != ST_NORMAL || ((uint8_t)c & 0xC0) != 0x80)) utf_flush();
    switch (st) {
        case ST_ESC:
            st = ST_NORMAL;
            if (c == '[') { st = ST_CSI; npar = 0; par_priv = par_q = false; par[0] = 0; return; }
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
            if (c == '?' || c == '>' || c == '=' || c == '!') { par_priv = true; par_q = c == '?'; return; }
            if (c >= 0x40 && c <= 0x7E) {
                if (!par_priv) csi(c);
                else if (par_q && (c == 'h' || c == 'l')) dec_mode(c == 'h');
                st = ST_NORMAL;
            }
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
            if (x >= vga_cols()) x = vga_cols() - 1;
            if (x > 0) vga_set_cursor(x - 1, y);
            return;
        case '\t':
            cursor(&x, &y);
            x = (x + 8) & ~7;
            vga_set_cursor(x >= vga_cols() ? vga_cols() - 1 : x, y);
            return;
    }
    uint8_t u = (uint8_t)c;
    if (utf_need) {                                          /* continuation byte */
        utf_cp = (utf_cp << 6) | (u & 0x3F);
        utf_raw[utf_n++] = u;
        if (--utf_need == 0) { utf_n = 0; emit(utf_cp); }
        return;
    }
    if (u >= 0xC2 && u <= 0xF4) {                            /* UTF-8 lead byte */
        utf_need = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
        utf_cp = u & (0x3F >> utf_need);
        utf_raw[0] = u;
        utf_n = 1;
        return;
    }
    if (u >= 0x80) { emit(cp866_to_uni(u)); return; }        /* stray byte: CP866 */
    if (acs) { emit(cp866_to_uni((uint8_t)acs_map(c))); return; }
    (void)x; (void)y;
    emit(u);
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
