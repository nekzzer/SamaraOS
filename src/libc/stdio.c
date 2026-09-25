#include "../core/types.h"
#include "../core/io.h"
#include "../core/heap.h"
#include "../core/string.h"
#include <stdarg.h>

/* ---- FILE shim: in-memory only ---- */

#define MAX_RAM_FILES 8
#define RAM_FILE_CAP  (16 * 1024)
#define MAX_OPEN_FILES 16

typedef struct {
    char    name[32];
    uint8_t* data;
    size_t  size;
    int     used;
} ramfile_t;

static ramfile_t ramfs[MAX_RAM_FILES];

/* Set by doom_wad_init(): the WAD blob lives in kernel heap. */
static const uint8_t* g_wad_buf  = 0;
static size_t         g_wad_size = 0;
static const char*    g_wad_name = "doom1.wad";

void libc_set_wad(const uint8_t* buf, size_t sz, const char* name) {
    g_wad_buf  = buf;
    g_wad_size = sz;
    if (name) g_wad_name = name;
}

typedef struct _IO_FILE {
    int      kind;     /* 0=closed 1=mem-read 2=mem-write 3=stdout 4=stderr */
    const uint8_t* rd;
    uint8_t*       wr;
    size_t   pos, size, cap;
    int      eof, err;
    int      ungot;
    char     name[32];
    int      ramfs_slot; /* -1 if not backed by ramfs */
} FILE;

static FILE pool[MAX_OPEN_FILES];

static FILE g_stdout = { .kind = 3, .ungot = -1, .ramfs_slot = -1 };
static FILE g_stderr = { .kind = 4, .ungot = -1, .ramfs_slot = -1 };
static FILE g_stdin  = { .kind = 0, .ungot = -1, .ramfs_slot = -1 };
FILE* stdout = &g_stdout;
FILE* stderr = &g_stderr;
FILE* stdin  = &g_stdin;

int errno = 0;

static FILE* alloc_file(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (pool[i].kind == 0) {
            for (size_t k = 0; k < sizeof(pool[i]); k++) ((uint8_t*)&pool[i])[k] = 0;
            pool[i].ungot = -1;
            pool[i].ramfs_slot = -1;
            return &pool[i];
        }
    }
    return NULL;
}

static int eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Strip directory prefix from path */
static const char* basename(const char* p) {
    const char* b = p;
    for (const char* s = p; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static int find_ramfs(const char* name) {
    for (int i = 0; i < MAX_RAM_FILES; i++) {
        if (ramfs[i].used && eq_ci(ramfs[i].name, name)) return i;
    }
    return -1;
}

static int alloc_ramfs(const char* name) {
    int idx = find_ramfs(name);
    if (idx >= 0) return idx;
    for (int i = 0; i < MAX_RAM_FILES; i++) {
        if (!ramfs[i].used) {
            ramfs[i].used = 1;
            for (int k = 0; k < 31 && name[k]; k++) ramfs[i].name[k] = name[k];
            if (!ramfs[i].data) {
                ramfs[i].data = (uint8_t*)kmalloc(RAM_FILE_CAP);
            }
            ramfs[i].size = 0;
            return i;
        }
    }
    return -1;
}

FILE* fopen(const char* path, const char* mode) {
    if (!path || !mode) return NULL;
    const char* fname = basename(path);
    int writing = (mode[0] == 'w' || mode[0] == 'a');

    if (!writing) {
        /* try WAD blob */
        if (g_wad_buf && eq_ci(fname, g_wad_name)) {
            FILE* f = alloc_file();
            if (!f) return NULL;
            f->kind = 1; f->rd = g_wad_buf; f->size = g_wad_size; f->pos = 0;
            for (int i = 0; i < 31 && fname[i]; i++) f->name[i] = fname[i];
            return f;
        }
        /* try ramfs */
        int idx = find_ramfs(fname);
        if (idx < 0) return NULL;
        FILE* f = alloc_file();
        if (!f) return NULL;
        f->kind = 1; f->rd = ramfs[idx].data; f->size = ramfs[idx].size;
        f->pos = 0; f->ramfs_slot = idx;
        for (int i = 0; i < 31 && fname[i]; i++) f->name[i] = fname[i];
        return f;
    }

    /* writing: ramfs slot */
    int idx = alloc_ramfs(fname);
    if (idx < 0) return NULL;
    FILE* f = alloc_file();
    if (!f) return NULL;
    f->kind = 2; f->wr = ramfs[idx].data; f->size = 0; f->cap = RAM_FILE_CAP;
    f->pos = 0; f->ramfs_slot = idx;
    for (int i = 0; i < 31 && fname[i]; i++) f->name[i] = fname[i];
    return f;
}

int fclose(FILE* fp) {
    if (!fp || fp->kind == 0) return -1;
    if (fp->kind == 2 && fp->ramfs_slot >= 0) {
        ramfs[fp->ramfs_slot].size = fp->size;
    }
    if (fp == stdout || fp == stderr || fp == stdin) return 0;
    fp->kind = 0;
    return 0;
}

static void dbg_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) {}
    outb(0x3F8, c);
}
static void dbg_str(const char* s) {
    while (*s) dbg_putc(*s++);
}
static void dbg_hex(uint32_t v) {
    const char* h = "0123456789abcdef";
    char buf[9]; for (int i = 0; i < 8; i++) buf[7-i] = h[(v >> (i*4)) & 0xf];
    buf[8] = 0; dbg_str(buf);
}

void samara_trace(const char* tag) { dbg_str("[TR "); dbg_str(tag); dbg_str("]"); }
void samara_trace_n(const char* tag, uint32_t n) {
    dbg_str("[TR "); dbg_str(tag); dbg_putc('='); dbg_hex(n); dbg_str("]");
}

size_t fread(void* p, size_t sz, size_t n, FILE* fp) {
    if (!fp || fp->kind != 1) return 0;
    size_t want = sz * n;
    size_t avail = fp->size > fp->pos ? fp->size - fp->pos : 0;
    if (want > avail) { want = avail; fp->eof = 1; }
    uint8_t* d = (uint8_t*)p;
    for (size_t i = 0; i < want; i++) d[i] = fp->rd[fp->pos + i];
    fp->pos += want;
    return sz ? want / sz : 0;
}

size_t fwrite(const void* p, size_t sz, size_t n, FILE* fp) {
    if (!fp) return 0;
    size_t want = sz * n;
    if (fp->kind == 2) {
        if (fp->pos + want > fp->cap) want = fp->cap - fp->pos;
        const uint8_t* s = (const uint8_t*)p;
        for (size_t i = 0; i < want; i++) fp->wr[fp->pos + i] = s[i];
        fp->pos += want;
        if (fp->pos > fp->size) fp->size = fp->pos;
        return sz ? want / sz : 0;
    }
    if (fp->kind == 3 || fp->kind == 4) {
        const uint8_t* s = (const uint8_t*)p;
        for (size_t i = 0; i < want; i++) {
            while (!(inb(0x3F8 + 5) & 0x20)) {}
            outb(0x3F8, s[i]);
        }
        return sz ? want / sz : 0;
    }
    return 0;
}

int fseek(FILE* fp, long off, int whence) {
    if (!fp || fp->kind != 1) {
        /* allow seek on writes too */
        if (fp && fp->kind == 2) {
            long base = whence == 0 ? 0 : whence == 1 ? (long)fp->pos : (long)fp->size;
            long np = base + off;
            if (np < 0) return -1;
            fp->pos = (size_t)np;
            return 0;
        }
        return -1;
    }
    long base = whence == 0 ? 0 : whence == 1 ? (long)fp->pos : (long)fp->size;
    long np = base + off;
    if (np < 0 || (size_t)np > fp->size) return -1;
    fp->pos = (size_t)np;
    fp->eof = 0;
    return 0;
}

long ftell(FILE* fp) { return fp ? (long)fp->pos : -1; }
void rewind(FILE* fp) { if (fp) { fp->pos = 0; fp->eof = 0; } }
int  fflush(FILE* fp) { (void)fp; return 0; }
int  feof(FILE* fp) { return fp ? fp->eof : 1; }
int  ferror(FILE* fp) { return fp ? fp->err : 0; }
int  remove(const char* p) { (void)p; return 0; }
int  rename(const char* a, const char* b) { (void)a; (void)b; return 0; }
FILE* tmpfile(void) { return NULL; }

int fputc(int c, FILE* fp) {
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, fp) == 1) return c;
    return -1;
}
int fputs(const char* s, FILE* fp) {
    size_t n = 0; while (s[n]) n++;
    return (int)fwrite(s, 1, n, fp);
}
int fgetc(FILE* fp) {
    if (!fp || fp->kind != 1) return -1;
    if (fp->ungot >= 0) { int c = fp->ungot; fp->ungot = -1; return c; }
    if (fp->pos >= fp->size) { fp->eof = 1; return -1; }
    return fp->rd[fp->pos++];
}
char* fgets(char* s, int n, FILE* fp) {
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(fp);
        if (c < 0) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}
int ungetc(int c, FILE* fp) { if (!fp) return -1; fp->ungot = c; return c; }

int putchar(int c) { return fputc(c, stdout); }
int puts(const char* s) { fputs(s, stdout); fputc('\n', stdout); return 0; }
int getchar(void) { return -1; }
void perror(const char* s) { if (s) { fputs(s, stderr); fputs(": error\n", stderr); } }

/* ---- printf core ---- */

typedef struct {
    char*  buf;
    size_t cap;
    size_t pos;
    FILE*  fp;
    int    written;
} sink_t;

static void emit_char(sink_t* s, char c) {
    if (s->buf) {
        if (s->pos + 1 < s->cap) s->buf[s->pos] = c;
        s->pos++;
        s->written++;
    } else if (s->fp) {
        unsigned char ch = (unsigned char)c;
        fwrite(&ch, 1, 1, s->fp);
        s->written++;
    }
}

static void emit_str(sink_t* s, const char* str, int n) {
    for (int i = 0; i < n; i++) emit_char(s, str[i]);
}

static int fmt_long(char* buf, long v, int base, int upper, int sign) {
    /* writes RIGHT-justified; returns length; buf must be 32 bytes */
    char tmp[32];
    int n = 0;
    int neg = 0;
    unsigned long uv;
    if (sign && v < 0) { neg = 1; uv = (unsigned long)(-v); }
    else uv = (unsigned long)v;
    if (uv == 0) tmp[n++] = '0';
    else while (uv) {
        unsigned d = uv % base;
        tmp[n++] = (d < 10) ? ('0' + d) : ((upper ? 'A' : 'a') + d - 10);
        uv /= base;
    }
    int out = 0;
    if (neg) buf[out++] = '-';
    while (n) buf[out++] = tmp[--n];
    return out;
}

static int fmt_ulong(char* buf, unsigned long v, int base, int upper) {
    return fmt_long(buf, (long)v, base, upper, 0);
}

int vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap) {
    sink_t sk = { .buf = buf, .cap = cap, .pos = 0, .fp = 0, .written = 0 };
    while (*fmt) {
        if (*fmt != '%') { emit_char(&sk, *fmt++); continue; }
        fmt++;
        /* flags */
        int leftalign = 0, zeropad = 0, showsign = 0, blank = 0, althash = 0;
        for (;;) {
            if (*fmt == '-') { leftalign = 1; fmt++; }
            else if (*fmt == '0') { zeropad = 1; fmt++; }
            else if (*fmt == '+') { showsign = 1; fmt++; }
            else if (*fmt == ' ') { blank = 1; fmt++; }
            else if (*fmt == '#') { althash = 1; fmt++; }
            else break;
        }
        /* width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { leftalign = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        /* precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        /* length */
        int islong = 0, islonglong = 0, isshort = 0, issize = 0;
        if (*fmt == 'l') { islong = 1; fmt++; if (*fmt == 'l') { islonglong = 1; fmt++; } }
        else if (*fmt == 'h') { isshort = 1; fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { issize = 1; fmt++; }
        (void)islonglong; (void)isshort; (void)issize; (void)blank;

        char conv = *fmt; if (!conv) break;
        fmt++;

        char numbuf[40];
        int  numlen = 0;
        const char* prefix = "";
        int  prefixlen = 0;

        switch (conv) {
            case '%': emit_char(&sk, '%'); continue;
            case 'c': {
                int c = va_arg(ap, int);
                if (!leftalign) for (int i = 1; i < width; i++) emit_char(&sk, ' ');
                emit_char(&sk, (char)c);
                if (leftalign) for (int i = 1; i < width; i++) emit_char(&sk, ' ');
                continue;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                int len = 0; while (s[len] && (prec < 0 || len < prec)) len++;
                if (!leftalign) for (int i = len; i < width; i++) emit_char(&sk, ' ');
                emit_str(&sk, s, len);
                if (leftalign) for (int i = len; i < width; i++) emit_char(&sk, ' ');
                continue;
            }
            case 'd':
            case 'i': {
                long v = islong ? va_arg(ap, long) : va_arg(ap, int);
                if (showsign && v >= 0) { prefix = "+"; prefixlen = 1; }
                else if (v < 0) { prefix = "-"; prefixlen = 1; numlen = fmt_long(numbuf, v, 10, 0, 0); /* abs value path: */ }
                if (v < 0) {
                    /* hand-roll */
                    unsigned long uv = (unsigned long)(-v);
                    int n2 = 0;
                    char tmp[32];
                    if (uv == 0) tmp[n2++] = '0';
                    else while (uv) { tmp[n2++] = '0' + (uv % 10); uv /= 10; }
                    int out = 0;
                    while (n2) numbuf[out++] = tmp[--n2];
                    numlen = out;
                } else {
                    numlen = fmt_long(numbuf, v, 10, 0, 0);
                }
                break;
            }
            case 'u': {
                unsigned long v = islong ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                numlen = fmt_ulong(numbuf, v, 10, 0);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long v = islong ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                numlen = fmt_ulong(numbuf, v, 16, conv == 'X');
                if (althash && v) { prefix = (conv == 'X') ? "0X" : "0x"; prefixlen = 2; }
                break;
            }
            case 'o': {
                unsigned long v = islong ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                numlen = fmt_ulong(numbuf, v, 8, 0);
                if (althash) { prefix = "0"; prefixlen = 1; }
                break;
            }
            case 'p': {
                unsigned long v = (unsigned long)va_arg(ap, void*);
                prefix = "0x"; prefixlen = 2;
                numlen = fmt_ulong(numbuf, v, 16, 0);
                break;
            }
            case 'f':
            case 'g':
            case 'e':
            case 'E':
            case 'G': {
                /* very crude: read double, print integer part with optional .000000 */
                double d = va_arg(ap, double);
                if (d < 0) { prefix = "-"; prefixlen = 1; d = -d; }
                else if (showsign) { prefix = "+"; prefixlen = 1; }
                long ip = (long)d;
                double fp = d - (double)ip;
                int p = prec < 0 ? 6 : prec;
                int out = 0;
                /* int part */
                {
                    char tmp[32]; int n2 = 0;
                    if (ip == 0) tmp[n2++] = '0';
                    else { unsigned long u = (unsigned long)ip; while (u) { tmp[n2++] = '0' + u % 10; u /= 10; } }
                    while (n2) numbuf[out++] = tmp[--n2];
                }
                if (p > 0) {
                    numbuf[out++] = '.';
                    for (int k = 0; k < p && out < (int)sizeof(numbuf) - 1; k++) {
                        fp *= 10;
                        int dig = (int)fp;
                        fp -= dig;
                        numbuf[out++] = '0' + (dig < 0 ? 0 : dig > 9 ? 9 : dig);
                    }
                }
                numlen = out;
                break;
            }
            case 'n': {
                int* dst = va_arg(ap, int*);
                if (dst) *dst = sk.written;
                continue;
            }
            default:
                emit_char(&sk, '%'); emit_char(&sk, conv); continue;
        }

        /* compose with width/zero-pad */
        int total = numlen + prefixlen;
        int pad = width - total;
        if (prec > numlen && (conv == 'd' || conv == 'i' || conv == 'u' ||
                              conv == 'x' || conv == 'X' || conv == 'o')) {
            int extra = prec - numlen;
            pad -= extra;
            if (!leftalign && !zeropad) for (int i = 0; i < pad; i++) emit_char(&sk, ' ');
            emit_str(&sk, prefix, prefixlen);
            for (int i = 0; i < extra; i++) emit_char(&sk, '0');
            emit_str(&sk, numbuf, numlen);
            if (leftalign) for (int i = 0; i < pad; i++) emit_char(&sk, ' ');
        } else if (zeropad && !leftalign && prec < 0) {
            emit_str(&sk, prefix, prefixlen);
            for (int i = 0; i < pad; i++) emit_char(&sk, '0');
            emit_str(&sk, numbuf, numlen);
        } else if (leftalign) {
            emit_str(&sk, prefix, prefixlen);
            emit_str(&sk, numbuf, numlen);
            for (int i = 0; i < pad; i++) emit_char(&sk, ' ');
        } else {
            for (int i = 0; i < pad; i++) emit_char(&sk, ' ');
            emit_str(&sk, prefix, prefixlen);
            emit_str(&sk, numbuf, numlen);
        }
    }
    if (sk.buf && sk.cap > 0) {
        sk.buf[sk.pos < sk.cap ? sk.pos : sk.cap - 1] = 0;
    }
    return sk.written;
}

int snprintf(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* buf, const char* fmt, va_list ap) {
    return vsnprintf(buf, 0x7fffffff, fmt, ap);
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE* fp, const char* fmt, va_list ap) {
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    int w = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
    fwrite(tmp, 1, w, fp);
    return w;
}

int fprintf(FILE* fp, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- sscanf — minimal: %d %x %s ---- */
int vsscanf(const char* str, const char* fmt, va_list ap) {
    int matched = 0;
    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*str == ' ' || *str == '\t' || *str == '\n') str++;
            fmt++; continue;
        }
        if (*fmt != '%') { if (*str != *fmt) return matched; str++; fmt++; continue; }
        fmt++;
        int suppress = 0; if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0; while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + *fmt - '0'; fmt++; }
        if (*fmt == 'l' || *fmt == 'h') fmt++;
        char conv = *fmt; if (!conv) break; fmt++;

        while (*str == ' ' || *str == '\t' || *str == '\n') str++;

        if (conv == 'd' || conv == 'i') {
            int sign = 1; if (*str == '-') { sign = -1; str++; } else if (*str == '+') str++;
            if (!(*str >= '0' && *str <= '9')) return matched;
            int v = 0;
            while (*str >= '0' && *str <= '9') { v = v * 10 + (*str - '0'); str++; }
            v *= sign;
            if (!suppress) { int* p = va_arg(ap, int*); *p = v; }
            matched++;
        } else if (conv == 'x' || conv == 'X') {
            if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
            unsigned int v = 0; int got = 0;
            while (1) {
                int d;
                if (*str >= '0' && *str <= '9') d = *str - '0';
                else if (*str >= 'a' && *str <= 'f') d = *str - 'a' + 10;
                else if (*str >= 'A' && *str <= 'F') d = *str - 'A' + 10;
                else break;
                v = v * 16 + d; str++; got = 1;
            }
            if (!got) return matched;
            if (!suppress) { unsigned int* p = va_arg(ap, unsigned int*); *p = v; }
            matched++;
        } else if (conv == 'o') {
            unsigned int v = 0; int got = 0;
            while (*str >= '0' && *str <= '7') { v = v * 8 + (*str - '0'); str++; got = 1; }
            if (!got) return matched;
            if (!suppress) { unsigned int* p = va_arg(ap, unsigned int*); *p = v; }
            matched++;
        } else if (conv == 's') {
            char* p = suppress ? 0 : va_arg(ap, char*);
            int n = 0;
            while (*str && *str != ' ' && *str != '\t' && *str != '\n' && (width == 0 || n < width)) {
                if (p) p[n] = *str;
                str++; n++;
            }
            if (p) p[n] = 0;
            if (n) matched++;
        } else if (conv == 'c') {
            char* p = suppress ? 0 : va_arg(ap, char*);
            if (!*str) return matched;
            if (p) *p = *str;
            str++;
            matched++;
        } else {
            return matched;
        }
    }
    return matched;
}

int sscanf(const char* str, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- assert / panic ---- */
void __libc_assert_fail(const char* expr, const char* file, int line) {
    fprintf(stderr, "assert failed: %s at %s:%d\n", expr, file, line);
    extern void doomgen_panic(const char* msg) __attribute__((noreturn));
    doomgen_panic("assertion failure");
}
