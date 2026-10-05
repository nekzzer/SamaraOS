/* edit: tabbed text editor. lines are pointers (borrowed from the file blob until touched),
   so a big file loads without a malloc per line */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "samara.h"
#include "dapps.h"

typedef struct { char *s; int n, cap; } Line;
typedef struct { int type, l, c, grp, cl0, cc0; char *t; int n; } Op;

enum { L_NONE, L_C, L_PY, L_SH, L_MD };
enum { H_NORM, H_KW, H_TYPE, H_STR, H_COM, H_NUM, H_PRE, H_FUNC };

typedef struct {
    Line *ln;
    int nl, cap;
    char path[1024];
    int dirty, lang, eol, crlf;
    int cl, cc, al, ac, top, left, want;
    Op *ops;
    int nops, capops, ptr, grp, typing, saved;
    uint8_t *st;
    int st_n, st_cap;
} Doc;

static Doc *docs[16];
static int ndocs, cur;
static int cw = 8, ch = 16, tabw = 4;
static int mx, my, dragging, sbdrag;
static char msg[200];
static uint32_t msg_until;
static int fb_mode;                 /* 0 none, 1 find, 2 replace, 3 goto */
static LineEd fb_find, fb_repl;
static int fb_foc, fb_case;

#define D docs[cur]

static void say(const char *s) {
    strncpy(msg, s, sizeof msg - 1);
    msg[sizeof msg - 1] = 0;
    msg_until = now_ms() + 4000;
}

static void own(Line *l, int extra) {
    if (!l->cap) {
        int c = l->n + extra + 16;
        char *p = malloc(c);
        memcpy(p, l->s, l->n);
        l->s = p;
        l->cap = c;
    } else if (l->n + extra + 1 > l->cap) {
        l->cap = (l->n + extra) * 2 + 16;
        l->s = realloc(l->s, l->cap);
    }
}

static void line_ins(Line *l, int at, const char *t, int n) {
    own(l, n);
    memmove(l->s + at + n, l->s + at, l->n - at);
    memcpy(l->s + at, t, n);
    l->n += n;
}

static void lines_room(Doc *d, int k) {
    if (d->nl + k + 1 > d->cap) {
        d->cap = (d->nl + k) * 3 / 2 + 64;
        d->ln = realloc(d->ln, d->cap * sizeof(Line));
    }
}

static void raw_ins(Doc *d, int l, int c, const char *t, int n, int *el, int *ec) {
    const char *e = memchr(t, '\n', n);
    int i, k = 0;
    if (!e) {
        line_ins(&d->ln[l], c, t, n);
        *el = l; *ec = c + n;
    } else {
        const char *p, *q;
        for (q = t; q < t + n; q++) if (*q == '\n') k++;
        lines_room(d, k);
        Line *L = &d->ln[l];
        int tn = L->n - c;
        char *tail = malloc(tn + 1);
        memcpy(tail, L->s + c, tn);
        own(L, 0);
        L->n = c;
        line_ins(L, c, t, e - t);
        memmove(d->ln + l + 1 + k, d->ln + l + 1, (d->nl - l - 1) * sizeof(Line));
        d->nl += k;
        p = e + 1;
        for (i = 1; i <= k; i++) {
            const char *e2 = i < k ? memchr(p, '\n', t + n - p) : 0;
            int sl = e2 ? e2 - p : t + n - p;
            Line *N = &d->ln[l + i];
            N->s = 0; N->n = 0; N->cap = 0;
            N->s = malloc(sl + 16); N->cap = sl + 16;
            memcpy(N->s, p, sl);
            N->n = sl;
            p = e2 + 1;
        }
        Line *Z = &d->ln[l + k];
        *el = l + k; *ec = Z->n;
        line_ins(Z, Z->n, tail, tn);
        free(tail);
    }
    if (d->st_n > l) d->st_n = l;
}

static char *get_text(Doc *d, int l1, int c1, int l2, int c2, int *len) {
    int i, n = 0;
    char *o;
    if (l1 == l2) n = c2 - c1;
    else {
        n = d->ln[l1].n - c1 + 1 + c2;
        for (i = l1 + 1; i < l2; i++) n += d->ln[i].n + 1;
    }
    o = malloc(n + 1);
    if (l1 == l2) memcpy(o, d->ln[l1].s + c1, n);
    else {
        char *p = o;
        memcpy(p, d->ln[l1].s + c1, d->ln[l1].n - c1); p += d->ln[l1].n - c1; *p++ = '\n';
        for (i = l1 + 1; i < l2; i++) { memcpy(p, d->ln[i].s, d->ln[i].n); p += d->ln[i].n; *p++ = '\n'; }
        memcpy(p, d->ln[l2].s, c2);
    }
    o[n] = 0;
    *len = n;
    return o;
}

static void raw_del(Doc *d, int l1, int c1, int l2, int c2) {
    if (l1 == l2) {
        Line *L = &d->ln[l1];
        own(L, 0);
        memmove(L->s + c1, L->s + c2, L->n - c2);
        L->n -= c2 - c1;
    } else {
        Line *A = &d->ln[l1], *B = &d->ln[l2];
        int i;
        own(A, B->n - c2);
        A->n = c1;
        memcpy(A->s + c1, B->s + c2, B->n - c2);
        A->n += B->n - c2;
        for (i = l1 + 1; i <= l2; i++) if (d->ln[i].cap) free(d->ln[i].s);
        memmove(d->ln + l1 + 1, d->ln + l2 + 1, (d->nl - l2 - 1) * sizeof(Line));
        d->nl -= l2 - l1;
    }
    if (d->st_n > l1) d->st_n = l1;
}

static void pos_after(int l, int c, const char *t, int n, int *el, int *ec) {
    int i, last = 0;
    *el = l;
    for (i = 0; i < n; i++) if (t[i] == '\n') { (*el)++; last = i + 1; }
    *ec = *el == l ? c + n : n - last;
}

static void push_op(Doc *d, int type, int l, int c, char *t, int n) {
    int i;
    if (d->saved > d->ptr) d->saved = -2;
    for (i = d->ptr; i < d->nops; i++) free(d->ops[i].t);
    d->nops = d->ptr;
    if (type == 1 && d->typing == 2 && d->ptr > 0) {
        Op *o = &d->ops[d->ptr - 1];
        if (o->type == 1 && o->l == l && !memchr(t, '\n', n) && !memchr(o->t, '\n', o->n) && o->n < 400) {
            if (c + n == o->c) {                 /* backspace */
                o->t = realloc(o->t, o->n + n + 1);
                memmove(o->t + n, o->t, o->n);
                memcpy(o->t, t, n);
                o->n += n; o->c = c;
                free(t);
                d->dirty = 1;
                return;
            }
            if (c == o->c) {                     /* del key */
                o->t = realloc(o->t, o->n + n + 1);
                memcpy(o->t + o->n, t, n);
                o->n += n;
                free(t);
                d->dirty = 1;
                return;
            }
        }
    }
    if (type == 0 && d->typing == 1 && d->ptr > 0) {
        Op *o = &d->ops[d->ptr - 1];
        int el, ec;
        if (o->type == 0 && !memchr(t, '\n', n)) {
            pos_after(o->l, o->c, o->t, o->n, &el, &ec);
            if (el == l && ec == c && o->n < 200) {
                o->t = realloc(o->t, o->n + n + 1);
                memcpy(o->t + o->n, t, n);
                o->n += n;
                free(t);
                if (d->saved == d->ptr) d->saved = -2;
                d->dirty = 1;
                return;
            }
        }
    }
    if (d->nops == d->capops) {
        d->capops = d->capops ? d->capops * 2 : 64;
        d->ops = realloc(d->ops, d->capops * sizeof(Op));
    }
    Op *o = &d->ops[d->nops++];
    o->type = type; o->l = l; o->c = c; o->t = t; o->n = n;
    o->grp = d->grp; o->cl0 = d->cl; o->cc0 = d->cc;
    d->ptr = d->nops;
    d->dirty = 1;
}

// all edits go through these two
static void do_ins(Doc *d, int l, int c, const char *t, int n) {
    int el, ec;
    char *cp = malloc(n + 1);
    memcpy(cp, t, n);
    cp[n] = 0;
    push_op(d, 0, l, c, cp, n);
    raw_ins(d, l, c, t, n, &el, &ec);
    d->cl = el; d->cc = ec;
}

static void do_del(Doc *d, int l1, int c1, int l2, int c2) {
    int n;
    char *t = get_text(d, l1, c1, l2, c2, &n);
    push_op(d, 1, l1, c1, t, n);
    raw_del(d, l1, c1, l2, c2);
    d->cl = l1; d->cc = c1;
}

static void undo(Doc *d, int redo) {
    int g;
    if (redo ? d->ptr >= d->nops : d->ptr <= 0) { say(redo ? "nothing to redo" : "nothing to undo"); return; }
    g = d->ops[redo ? d->ptr : d->ptr - 1].grp;
    for (;;) {
        Op *o;
        int el, ec;
        if (redo) { if (d->ptr >= d->nops || d->ops[d->ptr].grp != g) break; o = &d->ops[d->ptr++]; }
        else { if (d->ptr <= 0 || d->ops[d->ptr - 1].grp != g) break; o = &d->ops[--d->ptr]; }
        if ((o->type == 0) != (redo != 0)) {       /* remove the text */
            pos_after(o->l, o->c, o->t, o->n, &el, &ec);
            raw_del(d, o->l, o->c, el, ec);
            d->cl = o->l; d->cc = o->c;
        } else {
            raw_ins(d, o->l, o->c, o->t, o->n, &el, &ec);
            d->cl = el; d->cc = ec;
            if (!redo) { d->cl = o->cl0; d->cc = o->cc0; }
        }
    }
    d->dirty = d->ptr != d->saved;
    d->al = -1;
    d->typing = 0;
    d->st_n = 0;
}

static int detect_lang(const char *p) {
    const char *e = strrchr(p, '.'), *b = strrchr(p, '/');
    b = b ? b + 1 : p;
    if (!strcmp(b, "Makefile") || !strcmp(b, ".bashrc") || !strcmp(b, ".profile")) return L_SH;
    if (!e) return L_NONE;
    if (!strcasecmp(e, ".c") || !strcasecmp(e, ".h") || !strcasecmp(e, ".cpp") || !strcasecmp(e, ".cc") || !strcasecmp(e, ".hpp") ||
        !strcasecmp(e, ".js") || !strcasecmp(e, ".java") || !strcasecmp(e, ".go") || !strcasecmp(e, ".rs")) return L_C;
    if (!strcasecmp(e, ".py")) return L_PY;
    if (!strcasecmp(e, ".sh") || !strcasecmp(e, ".bash") || !strcasecmp(e, ".mk") || !strcasecmp(e, ".conf") || !strcasecmp(e, ".rc")) return L_SH;
    if (!strcasecmp(e, ".md") || !strcasecmp(e, ".markdown")) return L_MD;
    return L_NONE;
}

static Doc *doc_new(void) {
    Doc *d = calloc(1, sizeof *d);
    d->cap = 64;
    d->ln = calloc(d->cap, sizeof(Line));
    d->nl = 1;
    d->al = -1;
    d->eol = 1;
    return d;
}

static void doc_free(Doc *d) {
    int i;
    for (i = 0; i < d->nl; i++) if (d->ln[i].cap) free(d->ln[i].s);
    for (i = 0; i < d->nops; i++) free(d->ops[i].t);
    free(d->ln); free(d->ops); free(d->st);
    free(d);
}

static void title_upd(void) {
    char t[1100];
    const char *b = D->path[0] ? strrchr(D->path, '/') : 0;
    snprintf(t, sizeof t, "%s%s - edit", D->dirty ? "* " : "", D->path[0] ? (b ? b + 1 : D->path) : "untitled");
    be_title(t);
}

static void progress(long long done, long long tot, const char *what) {
    int W = be_w(), H = be_h();
    be_fill(0, 0, W, H, c_bg);
    be_text(W / 2 - 60, H / 2 - 30, what, c_ink, F_BOLD);
    be_fill(W / 2 - 150, H / 2, 300, 8, c_rule);
    be_fill(W / 2 - 150, H / 2, (int)(300 * done / (tot ? tot : 1)), 8, c_acc);
    be_flip();
}

// 0 ok, -1 can't open. the blob is never freed (lines point into it)
static int load_file(Doc *d, const char *path) {
    struct stat st;
    int fd = open(path, O_RDONLY);
    long long got = 0;
    char *blob, *p, *end;
    int cnt = 0;
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) { close(fd); return -1; }
    blob = malloc(st.st_size + 2);
    if (!blob) { close(fd); return -1; }
    uint32_t last = 0;
    while (got < st.st_size) {
        long r = read(fd, blob + got, st.st_size - got > (1 << 20) ? (1 << 20) : st.st_size - got);
        if (r <= 0) break;
        got += r;
        if (st.st_size > (4 << 20) && now_ms() - last > 150) { progress(got, st.st_size, "Loading..."); last = now_ms(); }
    }
    close(fd);
    blob[got] = '\n';
    end = blob + got;
    for (p = blob; p < end; p++) if (*p == '\n') cnt++;
    d->eol = got == 0 || end[-1] == '\n';
    if (!d->eol) cnt++;
    if (!cnt) cnt = 1;
    free(d->ln);
    d->cap = cnt + 64;
    d->ln = calloc(d->cap, sizeof(Line));
    d->nl = 0;
    p = blob;
    while (d->nl < cnt) {
        char *e = memchr(p, '\n', end - p + 1);
        int n = e - p;
        if (e > p && e[-1] == '\r') { n--; d->crlf = 1; }
        d->ln[d->nl].s = p;
        d->ln[d->nl].n = n;
        d->ln[d->nl].cap = 0;
        d->nl++;
        p = e + 1;
        if (p > end) break;
        if (d->nl % 200000 == 0 && st.st_size > (4 << 20)) progress(p - blob, got, "Parsing...");
    }
    if (!d->nl) { d->nl = 1; d->ln[0].s = blob; d->ln[0].n = 0; }
    strncpy(d->path, path, sizeof d->path - 1);
    d->lang = detect_lang(path);
    d->dirty = 0; d->saved = d->ptr;
    return 0;
}

static int save_file(Doc *d, const char *path) {
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return -1;
    for (i = 0; i < d->nl; i++) {
        fwrite(d->ln[i].s, 1, d->ln[i].n, f);
        if (i < d->nl - 1 || d->eol) fputs(d->crlf ? "\r\n" : "\n", f);
    }
    if (fclose(f) != 0) return -1;
    strncpy(d->path, path, sizeof d->path - 1);
    d->lang = detect_lang(path);
    d->dirty = 0; d->saved = d->ptr; d->typing = 0;
    return 0;
}

/* ---------- highlighting. state = what a line starts inside of (block comment, triple quote, fence) ---------- */

static const char *kw_c = " if else for while do switch case default break continue return goto sizeof typedef struct union enum static extern const volatile inline register new delete class public private protected namespace using template this try catch throw function var let async await import export from package func fn mut impl pub use match in of null true false NULL nil ";
static const char *ty_c = " int char short long float double void unsigned signed bool size_t ssize_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t FILE string u8 u16 u32 u64 i32 i64 usize str ";
static const char *kw_py = " and as assert break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield True False None async await self print ";
static const char *kw_sh = " if then else elif fi for while until do done case esac in function select time export local readonly return exit break continue echo cd set unset shift source alias test eval exec trap ";

static int word_in(const char *list, const char *s, int n) {
    char w[40];
    if (n >= 36) return 0;
    w[0] = ' ';
    memcpy(w + 1, s, n);
    w[n + 1] = ' ';
    w[n + 2] = 0;
    return strstr(list, w) != 0;
}

static int idc(int c) { return isalnum(c) || c == '_' || c >= 0x80; }

// fills cls[0..n) (or just walks if cls == NULL), returns state at the end of the line
static int hl_line(int lang, const char *s, int n, int st, uint8_t *cls) {
    int i = 0, j;
    if (n > 4096) n = 4096;
    if (cls) memset(cls, H_NORM, n);
#define SETC(a, b, k) do { if (cls) for (int q_ = (a); q_ < (b); q_++) cls[q_] = (k); } while (0)
    if (lang == L_MD) {
        if (n >= 3 && !memcmp(s, "```", 3)) { SETC(0, n, H_STR); return !st; }
        if (st) { SETC(0, n, H_STR); return st; }
        if (n && s[0] == '#') { SETC(0, n, H_FUNC); return 0; }
        if (n && s[0] == '>') { SETC(0, n, H_COM); return 0; }
        for (i = 0; i < n; i++) {
            if (s[i] == '`') {
                for (j = i + 1; j < n && s[j] != '`'; j++) ;
                if (j < n) { SETC(i, j + 1, H_STR); i = j; }
            }
        }
        i = 0;
        while (i < n && s[i] == ' ') i++;
        if (i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < n && s[i + 1] == ' ') SETC(i, i + 1, H_KW);
        else if (i < n && isdigit(s[i])) {
            for (j = i; j < n && isdigit(s[j]); j++) ;
            if (j < n && s[j] == '.') SETC(i, j + 1, H_KW);
        }
        return 0;
    }
    if (lang == L_NONE) return 0;
    while (i < n) {
        int c = (unsigned char)s[i];
        if (st == 1 && lang == L_C) {             /* in block comment */
            for (j = i; j + 1 < n && !(s[j] == '*' && s[j + 1] == '/'); j++) ;
            if (j + 1 < n) { SETC(i, j + 2, H_COM); i = j + 2; st = 0; continue; }
            SETC(i, n, H_COM); return 1;
        }
        if (st >= 1 && lang == L_PY) {            /* in triple quote: 1 = """, 2 = ''' */
            char q = st == 1 ? '"' : '\'';
            for (j = i; j + 2 < n + 0 && !(s[j] == q && s[j + 1] == q && s[j + 2] == q); j++) ;
            if (j + 2 < n) { SETC(i, j + 3, H_STR); i = j + 3; st = 0; continue; }
            SETC(i, n, H_STR); return st;
        }
        if (lang == L_C && c == '/' && i + 1 < n && s[i + 1] == '/') { SETC(i, n, H_COM); return 0; }
        if (lang == L_C && c == '/' && i + 1 < n && s[i + 1] == '*') { SETC(i, i + 2, H_COM); i += 2; st = 1; continue; }
        if ((lang == L_PY || lang == L_SH) && c == '#' && (i == 0 || lang == L_PY || s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == ';')) {
            SETC(i, n, H_COM); return 0;
        }
        if (lang == L_C && c == '#') {
            int k = i;
            for (j = 0; j < i; j++) if (s[j] != ' ' && s[j] != '\t') { k = -1; break; }
            if (k >= 0) {
                j = i + 1;
                while (j < n && (s[j] == ' ' || isalpha(s[j]))) j++;
                SETC(i, j, H_PRE);
                i = j;
                continue;
            }
        }
        if (lang == L_PY && c == '@' ) { j = i + 1; while (j < n && idc(s[j])) j++; SETC(i, j, H_PRE); i = j; continue; }
        if (lang == L_SH && c == '$') {
            j = i + 1;
            if (j < n && s[j] == '{') { while (j < n && s[j] != '}') j++; if (j < n) j++; }
            else if (j < n && (idc(s[j]))) { while (j < n && idc(s[j])) j++; }
            else if (j < n) j++;
            SETC(i, j, H_PRE); i = j; continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && lang != L_PY)) {
            if (lang == L_PY && i + 2 < n + 0 && s[i + 1] == c && s[i + 2] == c && c != '`') {
                char q = c;
                for (j = i + 3; j + 2 < n + 0 && !(s[j] == q && s[j + 1] == q && s[j + 2] == q); j++) ;
                if (j + 2 < n) { SETC(i, j + 3, H_STR); i = j + 3; continue; }
                SETC(i, n, H_STR); return c == '"' ? 1 : 2;
            }
            if (lang == L_C && c == '\'' && i > 0 && idc(s[i - 1])) { i++; continue; }
            for (j = i + 1; j < n && s[j] != c; j++) if (s[j] == '\\' && lang != L_SH) j++;
            if (j >= n) j = n - 1;
            SETC(i, j + 1, H_STR);
            i = j + 1;
            continue;
        }
        if (isdigit(c) && (i == 0 || !idc((unsigned char)s[i - 1]))) {
            j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.')) j++;
            SETC(i, j, H_NUM); i = j; continue;
        }
        if (isalpha(c) || c == '_') {
            j = i;
            while (j < n && idc((unsigned char)s[j])) j++;
            if (lang == L_C && word_in(kw_c, s + i, j - i)) SETC(i, j, H_KW);
            else if (lang == L_C && (word_in(ty_c, s + i, j - i) || (j - i > 2 && !memcmp(s + j - 2, "_t", 2)))) SETC(i, j, H_TYPE);
            else if (lang == L_PY && word_in(kw_py, s + i, j - i)) SETC(i, j, H_KW);
            else if (lang == L_SH && word_in(kw_sh, s + i, j - i)) SETC(i, j, H_KW);
            else if (lang != L_SH && j < n && s[j] == '(') SETC(i, j, H_FUNC);
            else if (lang == L_PY && i >= 4 && !memcmp(s + i - 4, "def ", 4)) SETC(i, j, H_FUNC);
            else if (lang == L_PY && i >= 6 && !memcmp(s + i - 6, "class ", 6)) SETC(i, j, H_TYPE);
            i = j; continue;
        }
        i++;
    }
    return st;
}

static int state_at(Doc *d, int l) {
    int i;
    if (d->lang == L_NONE) return 0;
    if (d->st_cap < d->nl + 2) {
        d->st_cap = d->nl + 1024;
        d->st = realloc(d->st, d->st_cap);
        d->st_n = d->st_n < 0 ? 0 : d->st_n;
    }
    if (d->st_n == 0) d->st[0] = 0;
    for (i = d->st_n; i < l; i++) d->st[i + 1] = hl_line(d->lang, d->ln[i].s, d->ln[i].n, d->st[i], 0);
    if (l > d->st_n) d->st_n = l;
    return d->st[l];
}

/* ---------- editing commands ---------- */

static int vcol(Line *l, int bc) {
    int v = 0, i = 0;
    while (i < bc && i < l->n) {
        if (l->s[i] == '\t') v += tabw - v % tabw;
        else v++;
        i += u8_len(l->s + i);
    }
    return v;
}

static int bcol(Line *l, int vc) {
    int v = 0, i = 0;
    while (i < l->n) {
        int wd = l->s[i] == '\t' ? tabw - v % tabw : 1;
        if (vc <= v) break;
        if (vc < v + wd) { if (vc - v >= (wd + 1) / 2) i += u8_len(l->s + i); break; }
        v += wd;
        i += u8_len(l->s + i);
    }
    return i > l->n ? l->n : i;
}

static void sel_order(Doc *d, int *l1, int *c1, int *l2, int *c2) {
    if (d->al < d->cl || (d->al == d->cl && d->ac <= d->cc)) { *l1 = d->al; *c1 = d->ac; *l2 = d->cl; *c2 = d->cc; }
    else { *l1 = d->cl; *c1 = d->cc; *l2 = d->al; *c2 = d->ac; }
}

static int del_sel(Doc *d) {
    int l1, c1, l2, c2;
    if (d->al < 0 || (d->al == d->cl && d->ac == d->cc)) { d->al = -1; return 0; }
    sel_order(d, &l1, &c1, &l2, &c2);
    d->al = -1;
    do_del(d, l1, c1, l2, c2);
    return 1;
}

static void type_text(Doc *d, const char *t, int n, int merge) {
    if (d->al >= 0) { d->grp++; d->typing = 0; del_sel(d); }
    else if (!merge || d->typing != 1) d->grp++;
    do_ins(d, d->cl, d->cc, t, n);
    d->typing = merge;
    d->want = -1;
}

static void copy_sel(Doc *d, int cut) {
    int l1, c1, l2, c2, n;
    char *t;
    if (d->al < 0 || (d->al == d->cl && d->ac == d->cc)) return;
    sel_order(d, &l1, &c1, &l2, &c2);
    t = get_text(d, l1, c1, l2, c2, &n);
    sm_clip_set(t, n);
    free(t);
    if (n > 65536) say("clipboard cut at 64k");
    if (cut) { d->grp++; d->typing = 0; del_sel(d); }
}

static void paste(Doc *d) {
    char *b = malloc(65537);
    int n = sm_clip_get(b, 65536), i, j = 0;
    for (i = 0; i < n; i++) if (b[i] != '\r') b[j++] = b[i];
    if (j) { d->typing = 0; type_text(d, b, j, 0); }
    free(b);
}

static void go_left(Doc *d) {
    if (d->cc > 0) { d->cc--; while (d->cc > 0 && (d->ln[d->cl].s[d->cc] & 0xC0) == 0x80) d->cc--; }
    else if (d->cl > 0) { d->cl--; d->cc = d->ln[d->cl].n; }
}

static void go_right(Doc *d) {
    Line *l = &d->ln[d->cl];
    if (d->cc < l->n) d->cc += u8_len(l->s + d->cc);
    else if (d->cl < d->nl - 1) { d->cl++; d->cc = 0; }
    if (d->cc > d->ln[d->cl].n) d->cc = d->ln[d->cl].n;
}

static void word_left(Doc *d) {
    if (d->cc == 0) { go_left(d); return; }
    Line *l = &d->ln[d->cl];
    while (d->cc > 0 && !idc((unsigned char)l->s[d->cc - 1])) d->cc--;
    while (d->cc > 0 && idc((unsigned char)l->s[d->cc - 1])) d->cc--;
}

static void word_right(Doc *d) {
    Line *l = &d->ln[d->cl];
    if (d->cc >= l->n) { go_right(d); return; }
    while (d->cc < l->n && !idc((unsigned char)l->s[d->cc])) d->cc++;
    while (d->cc < l->n && idc((unsigned char)l->s[d->cc])) d->cc++;
}

static void vert(Doc *d, int dl) {
    int nl = d->cl + dl;
    if (d->want < 0) d->want = vcol(&d->ln[d->cl], d->cc);
    if (nl < 0) { d->cl = 0; d->cc = 0; d->want = -1; return; }
    if (nl >= d->nl) { d->cl = d->nl - 1; d->cc = d->ln[d->cl].n; d->want = -1; return; }
    d->cl = nl;
    d->cc = bcol(&d->ln[nl], d->want);
}

static void enter_key(Doc *d) {
    char b[300];
    int n = 1, i = 0;
    Line *l = &d->ln[d->cl];
    b[0] = '\n';
    while (i < l->n && i < d->cc && (l->s[i] == ' ' || l->s[i] == '\t') && n < 200) b[n++] = l->s[i++];
    {
        int e = d->cc;
        while (e > 0 && l->s[e - 1] == ' ') e--;
        if (e > 0 && ((d->lang == L_PY && l->s[e - 1] == ':') || (d->lang == L_C && l->s[e - 1] == '{') || (d->lang == L_SH && !strncmp(l->s + (e > 4 ? e - 4 : 0), "then", 4))))
            { memcpy(b + n, "    ", 4); n += 4; }
    }
    d->typing = 0;
    type_text(d, b, n, 0);
}

static void backspace(Doc *d, int fwd) {
    if (d->al >= 0 && !(d->al == d->cl && d->ac == d->cc)) { d->grp++; d->typing = 0; del_sel(d); return; }
    d->al = -1;
    if (d->typing != 2) d->grp++;
    d->want = -1;
    if (!fwd) {
        if (d->cc > 0) {
            int c = d->cc - 1;
            while (c > 0 && (d->ln[d->cl].s[c] & 0xC0) == 0x80) c--;
            do_del(d, d->cl, c, d->cl, d->cc);
        } else if (d->cl > 0) do_del(d, d->cl - 1, d->ln[d->cl - 1].n, d->cl, 0);
        else return;
    } else {
        Line *l = &d->ln[d->cl];
        if (d->cc < l->n) do_del(d, d->cl, d->cc, d->cl, d->cc + u8_len(l->s + d->cc));
        else if (d->cl < d->nl - 1) do_del(d, d->cl, d->cc, d->cl + 1, 0);
        else return;
    }
    d->typing = 2;
}

static void indent(Doc *d, int back) {
    int l1, c1, l2, c2, i;
    if (d->al < 0 || d->al == d->cl) {
        if (!back) {
            char sp[8];
            int v = vcol(&d->ln[d->cl], d->cc), n = tabw - v % tabw;
            memset(sp, ' ', n);
            d->typing = 0;
            type_text(d, sp, n, 0);
            return;
        }
        l1 = l2 = d->cl;
    } else { sel_order(d, &l1, &c1, &l2, &c2); if (c2 == 0 && l2 > l1) l2--; }
    d->grp++;
    d->typing = 0;
    for (i = l1; i <= l2; i++) {
        Line *l = &d->ln[i];
        if (!back) do_ins(d, i, 0, "    ", 4);
        else {
            int k = 0;
            while (k < 4 && k < l->n && l->s[k] == ' ') k++;
            if (!k && l->n && l->s[0] == '\t') k = 1;
            if (k) do_del(d, i, 0, i, k);
        }
    }
    if (d->al >= 0) {
        d->al = l1; d->ac = 0;
        d->cl = l2; d->cc = d->ln[l2].n;
    } else if (back) { if (d->cc > d->ln[d->cl].n) d->cc = d->ln[d->cl].n; }
}

static void dup_line(Doc *d) {
    Line *l = &d->ln[d->cl];
    char *b = malloc(l->n + 2);
    b[0] = '\n';
    memcpy(b + 1, l->s, l->n);
    d->grp++; d->typing = 0;
    int cl = d->cl, cc = d->cc;
    do_ins(d, d->cl, l->n, b, l->n + 1);
    d->cl = cl + 1; d->cc = cc;
    free(b);
}

static void goto_line(Doc *d, int l) {
    if (l < 1) l = 1;
    if (l > d->nl) l = d->nl;
    d->cl = l - 1; d->cc = 0; d->al = -1; d->want = -1;
}

static void select_all(Doc *d) {
    d->al = 0; d->ac = 0;
    d->cl = d->nl - 1; d->cc = d->ln[d->cl].n;
}

static void select_word(Doc *d) {
    Line *l = &d->ln[d->cl];
    int a = d->cc, b = d->cc;
    if (a < l->n && idc((unsigned char)l->s[a])) {
        while (a > 0 && idc((unsigned char)l->s[a - 1])) a--;
        while (b < l->n && idc((unsigned char)l->s[b])) b++;
    } else if (b < l->n) b += u8_len(l->s + b);
    d->al = d->cl; d->ac = a; d->cc = b;
}

/* ---------- find / replace ---------- */

static int find_in(const char *s, int n, const char *q, int ql, int from, int cs) {
    int i;
    for (i = from; i + ql <= n; i++) {
        if (cs ? !memcmp(s + i, q, ql) : !strncasecmp(s + i, q, ql)) return i;
    }
    return -1;
}

static int find_next(Doc *d, int back) {
    char *q = fb_find.buf;
    int ql = fb_find.len, l1, c1, l2, c2, i, p, steps;
    if (!ql) return 0;
    if (d->al >= 0) sel_order(d, &l1, &c1, &l2, &c2); else { l1 = l2 = d->cl; c1 = c2 = d->cc; }
    if (!back) {
        i = l2; p = c2;
        for (steps = 0; steps <= d->nl; steps++) {
            Line *l = &d->ln[i];
            int f = find_in(l->s, l->n, q, ql, p, fb_case);
            if (f >= 0) { d->al = i; d->ac = f; d->cl = i; d->cc = f + ql; d->want = -1; return 1; }
            i = (i + 1) % d->nl; p = 0;
            if (i == 0 && steps < d->nl) say("wrapped around");
        }
    } else {
        i = l1; p = c1;
        for (steps = 0; steps <= d->nl; steps++) {
            Line *l = &d->ln[i];
            int f, last = -1, from = 0;
            while ((f = find_in(l->s, l->n, q, ql, from, fb_case)) >= 0 && f < p) { last = f; from = f + 1; }
            if (last >= 0) { d->al = i; d->ac = last; d->cl = i; d->cc = last + ql; d->want = -1; return 1; }
            i = (i + d->nl - 1) % d->nl; p = d->ln[i].n + 1;
        }
    }
    say("not found");
    return 0;
}

static void replace_one(Doc *d) {
    int l1, c1, l2, c2;
    if (d->al >= 0) {
        sel_order(d, &l1, &c1, &l2, &c2);
        if (l1 == l2 && c2 - c1 == fb_find.len && (fb_case ? !memcmp(d->ln[l1].s + c1, fb_find.buf, c2 - c1) : !strncasecmp(d->ln[l1].s + c1, fb_find.buf, c2 - c1))) {
            d->grp++; d->typing = 0;
            del_sel(d);
            if (fb_repl.len) do_ins(d, d->cl, d->cc, fb_repl.buf, fb_repl.len);
        }
    }
    find_next(d, 0);
}

static void replace_all(Doc *d) {
    int i, cnt = 0, ql = fb_find.len;
    if (!ql) return;
    d->grp++; d->typing = 0; d->al = -1;
    for (i = 0; i < d->nl; i++) {
        Line *l = &d->ln[i];
        int f, from = 0, pos[256], np = 0, k;
        while (np < 256 && (f = find_in(l->s, l->n, fb_find.buf, ql, from, fb_case)) >= 0) { pos[np++] = f; from = f + ql; }
        for (k = np - 1; k >= 0; k--) {
            do_del(d, i, pos[k], i, pos[k] + ql);
            if (fb_repl.len) do_ins(d, i, pos[k], fb_repl.buf, fb_repl.len);
            cnt++;
        }
    }
    {
        char m[64];
        sprintf(m, "replaced %d", cnt);
        say(m);
    }
    if (d->cl >= d->nl) d->cl = d->nl - 1;
    if (d->cc > d->ln[d->cl].n) d->cc = d->ln[d->cl].n;
}

/* ---------- drawing ---------- */

static uint32_t hcol[8];
static int follow = 1, blink_on = 1;
static uint32_t last_key;
static int TH, SBH = 24;

static void pal(void) {
    int light = c_bg == 0xF4F4F2;
    static const uint32_t dk[8] = { 0xD4D4D4, 0xC586C0, 0x4EC9B0, 0xCE9178, 0x6A9955, 0xB5CEA8, 0x9CDCFE, 0xDCDCAA };
    static const uint32_t lt[8] = { 0x24262B, 0x8A2BB5, 0x1F7F7A, 0xA3441E, 0x5E8A47, 0x2E7D32, 0x2B6CB0, 0x7A5C00 };
    memcpy(hcol, light ? lt : dk, sizeof hcol);
}

static int fb_h(void) { return fb_mode == 2 ? 70 : fb_mode ? 38 : 0; }

typedef struct { int fx, fy, fw, prev, next, aa, by, rx, ry, rw, rep, all; } FbL;

static void fb_lay(FbL *f) {
    int W = be_w(), top = be_h() - SBH - fb_h();
    f->fx = 84; f->fy = top + 5; f->fw = W - 84 - 270;
    if (f->fw > 360) f->fw = 360;
    f->prev = f->fx + f->fw + 10; f->next = f->prev + 64; f->aa = f->next + 64;
    f->rx = f->fx; f->ry = top + 37; f->rw = f->fw;
    f->rep = f->prev; f->all = f->next;
    f->by = top + 5;
}

static void tab_geo(int i, int *x, int *w) {
    int W = be_w();
    int tw = (W - 40) / (ndocs ? ndocs : 1);
    if (tw > 190) tw = 190;
    *x = 4 + i * tw;
    *w = tw - 2;
}

static int digits(int n) { int k = 1; while (n >= 10) { n /= 10; k++; } return k; }

static void draw(void) {
    Doc *d = D;
    int W = be_w(), H = be_h(), i, r;
    int fbh = fb_h();
    TH = fh_reg + 12;
    int ty = TH, th = H - TH - SBH - fbh;
    int rows = th / ch;
    int gw = digits(d->nl) * cw + 22;
    int cols = (W - gw - 14) / cw;
    char tmp[1200];
    if (rows < 1) rows = 1;
    if (cols > 240) cols = 240;
    if (follow) {
        int vc = vcol(&d->ln[d->cl], d->cc);
        if (d->cl < d->top) d->top = d->cl;
        if (d->cl >= d->top + rows) d->top = d->cl - rows + 1;
        if (vc < d->left) d->left = vc > 4 ? vc - 4 : 0;
        if (vc >= d->left + cols - 1) d->left = vc - cols + 5;
        follow = 0;
    }
    if (d->top > d->nl - 1) d->top = d->nl - 1;
    if (d->top < 0) d->top = 0;
    if (d->left < 0) d->left = 0;
    be_fill(0, 0, W, H, c_well);
    {
        int sl1 = 0, sc1 = 0, sl2 = 0, sc2 = 0, hs = 0;
        int st = state_at(d, d->top);
        uint8_t cls[4100];
        if (d->al >= 0 && !(d->al == d->cl && d->ac == d->cc)) { sel_order(d, &sl1, &sc1, &sl2, &sc2); hs = 1; }
        for (r = 0; r < rows && d->top + r < d->nl; r++) {
            int li = d->top + r, y = ty + r * ch;
            Line *l = &d->ln[li];
            int n = l->n, k, v = 0, nc = 0, tx = gw + 4;
            static char out[1100];
            static int off[300], ccl[300];
            int ob = 0;
            if (li == d->cl && !hs) be_fill(gw, y, W - gw, ch, c_bg2);
            if ((fb_mode == 1 || fb_mode == 2) && fb_find.len) {
                int f, from = 0, cnt = 0;
                while (cnt++ < 50 && (f = find_in(l->s, n, fb_find.buf, fb_find.len, from, fb_case)) >= 0) {
                    int a = vcol(l, f) - d->left, b = vcol(l, f + fb_find.len) - d->left;
                    if (b > 0 && a < cols) {
                        if (a < 0) a = 0;
                        if (b > cols) b = cols;
                        be_fill(tx + a * cw, y, (b - a) * cw, ch, c_bg == 0xF4F4F2 ? 0xF2D98A : 0x5A4A1A);
                    }
                    from = f + fb_find.len;
                }
            }
            if (hs && li >= sl1 && li <= sl2) {
                int a = li == sl1 ? vcol(l, sc1) : 0;
                int b = li == sl2 ? vcol(l, sc2) : vcol(l, n) + 1;
                a -= d->left; b -= d->left;
                if (a < 0) a = 0;
                if (b > cols) b = cols;
                if (b > a) be_fill(tx + a * cw, y, (b - a) * cw, ch, c_bg == 0xF4F4F2 ? 0xBBD6F5 : 0x2F4A6E);
            }
            st = hl_line(d->lang, l->s, n, st, cls);
            for (k = 0; k < n && v < d->left + cols; ) {
                int wd, u = (unsigned char)l->s[k], cl = k < 4096 ? cls[k] : 0, len = 1;
                if (u == '\t') wd = tabw - v % tabw;
                else wd = 1;
                if (u >= 0xC0) len = u8_len(l->s + k);
                if (k + len > n) len = n - k;
                if (v + wd > d->left) {
                    int c0;
                    for (c0 = v; c0 < v + wd; c0++) {
                        if (c0 < d->left || c0 >= d->left + cols || nc >= 290) continue;
                        off[nc] = ob; ccl[nc] = cl;
                        if (u == '\t') out[ob++] = ' ';
                        else if (u < 32 || u == 127) out[ob++] = '.';
                        else if (u < 0x80) out[ob++] = u;
                        else if ((u == 0xD0 || u == 0xD1 || u == 0xC2) && len == 2 && (l->s[k + 1] & 0xC0) == 0x80) { out[ob++] = u; out[ob++] = l->s[k + 1]; }
                        else out[ob++] = '?';
                        nc++;
                    }
                }
                v += wd;
                k += len;
            }
            off[nc] = ob;
            for (k = 0; k < nc;) {
                int e = k + 1, all_sp = 1, j;
                while (e < nc && ccl[e] == ccl[k]) e++;
                for (j = off[k]; j < off[e]; j++) if (out[j] != ' ') { all_sp = 0; break; }
                if (!all_sp) {
                    memcpy(tmp, out + off[k], off[e] - off[k]);
                    tmp[off[e] - off[k]] = 0;
                    be_text(tx + k * cw, y, tmp, hcol[ccl[k]], F_MONO);
                }
                k = e;
            }
            if (li == d->cl && (blink_on || now_ms() - last_key < 600)) {
                int cx = vcol(l, d->cc) - d->left;
                if (cx >= 0 && cx <= cols) be_fill(tx + cx * cw, y, 2, ch, c_acc);
            }
        }
        be_fill(0, ty, gw, th, c_bg2);
        for (r = 0; r < rows && d->top + r < d->nl; r++) {
            char nb[16];
            int li = d->top + r;
            sprintf(nb, "%d", li + 1);
            be_text(gw - 10 - be_text_w(nb, F_MONO), ty + r * ch, nb, li == d->cl ? c_ink : c_faint, F_MONO);
        }
        be_fill(gw - 1, ty, 1, th, c_rule);
    }
    if (d->nl > rows) {
        int sh = th * rows / d->nl, sy;
        if (sh < 24) sh = 24;
        sy = ty + (th - sh) * d->top / (d->nl - rows);
        be_fill(W - 9, ty, 9, th, c_bg2);
        be_fill(W - 8, sy, 6, sh, c_faint);
    }
    be_fill(0, 0, W, TH, c_bar);
    for (i = 0; i < ndocs; i++) {
        int x, w;
        char t[300], b[300];
        const char *p = docs[i]->path[0] ? strrchr(docs[i]->path, '/') : 0;
        tab_geo(i, &x, &w);
        be_fill(x, 3, w, TH - 3, i == cur ? c_bg : c_bar);
        if (i == cur) be_fill(x, 3, w, 2, c_acc);
        snprintf(b, sizeof b, "%s%s", docs[i]->dirty ? "\xE2\x97\x8F " : "", docs[i]->path[0] ? (p ? p + 1 : docs[i]->path) : "untitled");
        fit(b, w - 30, F_REG, t, sizeof t);
        txt(x + 8, 3, TH - 3, t, i == cur ? c_ink : c_dim, F_REG);
        txt(x + w - 18, 3, TH - 3, "x", c_faint, F_REG);
    }
    {
        int x, w;
        tab_geo(ndocs, &x, &w);
        txt(x + 6, 3, TH - 3, "+", c_dim, F_BOLD);
    }
    if (fbh) {
        FbL f;
        fb_lay(&f);
        be_fill(0, H - SBH - fbh, W, fbh, c_bar);
        be_fill(0, H - SBH - fbh, W, 1, c_rule);
        txt(10, f.fy, 28, fb_mode == 3 ? "Line:" : "Find:", c_dim, F_REG);
        {
            LineEd *e = &fb_find;
            le_draw(e, f.fx, f.fy, f.fw, 28, fb_foc == 0);
        }
        if (fb_mode != 3) {
            button(f.prev, f.by, 58, "Prev", 0, mx, my);
            button(f.next, f.by, 58, "Next", 0, mx, my);
            button(f.aa, f.by, 40, "Aa", fb_case, mx, my);
        }
        if (fb_mode == 2) {
            txt(10, f.ry - 5, 28, "Replace:", c_dim, F_REG);
            le_draw(&fb_repl, f.rx, f.ry - 5, f.rw, 28, fb_foc == 1);
            button(f.rep, f.ry - 5, 58, "One", 0, mx, my);
            button(f.all, f.ry - 5, 58, "All", 0, mx, my);
        }
    }
    be_fill(0, H - SBH, W, SBH, c_bar);
    {
        char s[300], t[200];
        const char *ln[] = { "plain", "C/JS", "Python", "shell", "Markdown" };
        if (now_ms() < msg_until) fit(msg, W / 2, F_SMALL, s, sizeof s);
        else fit(d->path[0] ? d->path : "untitled", W / 2, F_SMALL, s, sizeof s);
        txt(10, H - SBH, SBH, s, now_ms() < msg_until ? c_acc : c_dim, F_SMALL);
        snprintf(t, sizeof t, "Ln %d, Col %d    %s    UTF-8    %s", d->cl + 1, vcol(&d->ln[d->cl], d->cc) + 1, ln[d->lang], d->crlf ? "CRLF" : "LF");
        txt(W - 10 - be_text_w(t, F_SMALL), H - SBH, SBH, t, c_dim, F_SMALL);
    }
    be_flip();
}

/* ---------- tabs, files, events ---------- */

static void tab_open(const char *path) {
    Doc *d;
    int i;
    if (ndocs >= 16) { say("too many tabs"); return; }
    if (path) {
        for (i = 0; i < ndocs; i++) if (!strcmp(docs[i]->path, path)) { cur = i; return; }
    }
    d = doc_new();
    if (path && load_file(d, path) < 0) {
        /* new file with that name */
        strncpy(d->path, path, sizeof d->path - 1);
        d->lang = detect_lang(path);
        if (access(path, F_OK) == 0) { say("can't read file"); doc_free(d); return; }
        say("new file");
    }
    if (ndocs == 1 && !docs[0]->path[0] && !docs[0]->dirty && docs[0]->nl == 1 && !docs[0]->ln[0].n) {
        doc_free(docs[0]);
        docs[0] = d;
        cur = 0;
        return;
    }
    docs[ndocs] = d;
    cur = ndocs++;
}

static int tab_close(int i) {
    if (docs[i]->dirty) {
        char q[300];
        const char *p = docs[i]->path[0] ? strrchr(docs[i]->path, '/') : 0;
        snprintf(q, sizeof q, "%s has unsaved changes", docs[i]->path[0] ? (p ? p + 1 : docs[i]->path) : "untitled");
        if (!ask_yn("Close without saving?", q)) return 0;
    }
    doc_free(docs[i]);
    memmove(docs + i, docs + i + 1, (ndocs - i - 1) * sizeof docs[0]);
    ndocs--;
    if (cur >= ndocs) cur = ndocs - 1;
    return 1;
}

static void do_save_as(void) {
    char p[1100];
    strcpy(p, D->path[0] ? D->path : "untitled.txt");
    if (!D->path[0]) { if (!getcwd(p, sizeof p)) strcpy(p, "/"); strcat(p, "/untitled.txt"); }
    if (!ask_file("Save as", p, 1)) return;
    if (save_file(D, p) < 0) say("save failed");
    else say("saved");
}

static void do_save(void) {
    if (!D->path[0]) { do_save_as(); return; }
    if (save_file(D, D->path) < 0) say("save failed");
    else say("saved");
}

static void do_open(void) {
    char p[1100];
    if (!getcwd(p, sizeof p)) strcpy(p, "/");
    strcat(p, "/");
    if (D->path[0]) strcpy(p, D->path);
    if (ask_file("Open file", p, 0)) tab_open(p);
}

static int quit_all(void) {
    int i, any = 0;
    for (i = 0; i < ndocs; i++) if (docs[i]->dirty) any = 1;
    if (!any) return 1;
    return ask_yn("Quit?", "some tabs have unsaved changes");
}

static void fb_open(int mode) {
    Doc *d = D;
    fb_mode = mode;
    fb_foc = 0;
    if (mode != 3 && d->al >= 0 && d->al == d->cl && d->cc > d->ac && d->cc - d->ac < 200) {
        char b[256];
        memcpy(b, d->ln[d->cl].s + d->ac, d->cc - d->ac);
        b[d->cc - d->ac] = 0;
        le_set(&fb_find, b);
    } else if (mode == 3) le_set(&fb_find, "");
    fb_find.all = 1;
}

static void text_pos(int px, int py, int *l, int *c) {
    Doc *d = D;
    int gw = digits(d->nl) * cw + 22;
    int li = d->top + (py - TH) / ch;
    int v = d->left + (px - gw - 4 + cw / 2) / cw;
    if (py < TH) li = d->top - 1;
    if (li < 0) li = 0;
    if (li >= d->nl) { li = d->nl - 1; *l = li; *c = d->ln[li].n; return; }
    if (v < 0) v = 0;
    *l = li;
    *c = bcol(&d->ln[li], v);
}

static int mod_hit(int x, int y, int bx, int by, int bw) { return x >= bx && x < bx + bw && y >= by && y < by + 28; }

static void on_down(FmEv *e) {
    Doc *d = D;
    int W = be_w(), H = be_h(), i;
    static uint32_t lt;
    static int lx, ly, clicks;
    if (e->b < TH) {
        for (i = 0; i <= ndocs; i++) {
            int x, w;
            tab_geo(i, &x, &w);
            if (e->a >= x && e->a < x + w) {
                if (i == ndocs) { tab_open(0); return; }
                if (e->a > x + w - 26) { tab_close(i); if (!ndocs) { tab_open(0); } return; }
                cur = i;
                follow = 1;
                return;
            }
        }
        return;
    }
    if (fb_mode && e->b >= H - SBH - fb_h() && e->b < H - SBH) {
        FbL f;
        fb_lay(&f);
        if (e->b < f.fy + 28 + 4) {
            if (e->a >= f.fx && e->a < f.fx + f.fw) { fb_foc = 0; le_click(&fb_find, f.fx, e->a); }
            if (fb_mode != 3) {
                if (mod_hit(e->a, e->b, f.prev, f.by, 58)) find_next(d, 1);
                if (mod_hit(e->a, e->b, f.next, f.by, 58)) find_next(d, 0);
                if (mod_hit(e->a, e->b, f.aa, f.by, 40)) fb_case = !fb_case;
            }
        } else if (fb_mode == 2) {
            if (e->a >= f.rx && e->a < f.rx + f.rw) { fb_foc = 1; le_click(&fb_repl, f.rx, e->a); }
            if (mod_hit(e->a, e->b, f.rep, f.ry - 5, 58)) replace_one(d);
            if (mod_hit(e->a, e->b, f.all, f.ry - 5, 58)) replace_all(d);
        }
        follow = 1;
        return;
    }
    if (e->b >= H - SBH) return;
    if (e->a >= W - 12 && d->nl > 1) {
        sbdrag = 1;
        d->top = (long long)(e->b - TH) * d->nl / (H - TH - SBH - fb_h());
        if (d->top >= d->nl) d->top = d->nl - 1;
        return;
    }
    {
        int l, c;
        text_pos(e->a, e->b, &l, &c);
        if (now_ms() - lt < 450 && abs(e->a - lx) < 4 && abs(e->b - ly) < 4) clicks++; else clicks = 1;
        lt = now_ms(); lx = e->a; ly = e->b;
        d->typing = 0;
        d->want = -1;
        if (e->mods & MOD_SHIFT) { if (d->al < 0) { d->al = d->cl; d->ac = d->cc; } d->cl = l; d->cc = c; }
        else {
            d->cl = l; d->cc = c; d->al = -1;
            if (clicks == 2) select_word(d);
            else if (clicks >= 3) { d->al = l; d->ac = 0; d->cc = d->ln[l].n; clicks = 0; }
            else { dragging = 1; }
        }
        if (clicks == 2) dragging = 1;
        last_key = now_ms();
    }
}

static void on_move(FmEv *e) {
    Doc *d = D;
    mx = e->a; my = e->b;
    if (sbdrag) {
        int H = be_h();
        d->top = (long long)(e->b - TH) * d->nl / (H - TH - SBH - fb_h());
        if (d->top < 0) d->top = 0;
        if (d->top >= d->nl) d->top = d->nl - 1;
        return;
    }
    if (dragging) {
        int l, c, H = be_h();
        if (e->b < TH + 2 && d->top > 0) d->top--;
        if (e->b > H - SBH - fb_h() - 2 && d->top < d->nl - 1) d->top++;
        text_pos(e->a, e->b < TH ? TH : e->b, &l, &c);
        if (d->al < 0) { d->al = d->cl; d->ac = d->cc; }
        d->cl = l; d->cc = c;
        follow = 0;
    }
}

static void on_key(FmEv *e) {
    Doc *d = D;
    int c = e->a, sh = e->mods & MOD_SHIFT, ct = e->mods & MOD_CTRL, mv = 0;
    last_key = now_ms();
    follow = 1;
    if (ct && c >= 'A' && c <= 'Z') c += 32;
    if (ct) {
        switch (c) {
        case 's': if (sh) do_save_as(); else do_save(); return;
        case 'o': do_open(); return;
        case 'n': case 't': tab_open(0); return;
        case 'w': if (tab_close(cur) && !ndocs) tab_open(0); return;
        case 'q': if (quit_all()) exit(0); return;
        case 'f': fb_open(1); return;
        case 'h': case 'r': fb_open(2); return;
        case 'g': fb_open(3); return;
        case 'z': if (sh) undo(d, 1); else undo(d, 0); return;
        case 'y': undo(d, 1); return;
        case 'a': select_all(d); return;
        case 'c': copy_sel(d, 0); return;
        case 'x': copy_sel(d, 1); return;
        case 'v': paste(d); return;
        case 'd': dup_line(d); return;
        case FK_TAB: cur = (cur + (sh ? ndocs - 1 : 1)) % ndocs; return;
        case FK_PGDN: cur = (cur + 1) % ndocs; return;
        case FK_PGUP: cur = (cur + ndocs - 1) % ndocs; return;
        }
    }
    if (c == FK_ESC) {
        if (fb_mode) { fb_mode = 0; return; }
        d->al = -1;
        return;
    }
    switch (c) {
    case FK_UP: case FK_DOWN: case FK_LEFT: case FK_RIGHT: case FK_HOME: case FK_END: case FK_PGUP: case FK_PGDN: mv = 1; break;
    }
    if (mv) {
        int rows = (be_h() - TH - SBH - fb_h()) / ch, l1, c1, l2, c2;
        d->typing = 0;
        if (sh && d->al < 0) { d->al = d->cl; d->ac = d->cc; }
        else if (!sh && d->al >= 0) {
            sel_order(d, &l1, &c1, &l2, &c2);
            if (c == FK_LEFT) { d->cl = l1; d->cc = c1; d->al = -1; d->want = -1; return; }
            if (c == FK_RIGHT) { d->cl = l2; d->cc = c2; d->al = -1; d->want = -1; return; }
            d->al = -1;
        }
        if (c != FK_UP && c != FK_DOWN && c != FK_PGUP && c != FK_PGDN) d->want = -1;
        switch (c) {
        case FK_LEFT: if (ct) word_left(d); else go_left(d); break;
        case FK_RIGHT: if (ct) word_right(d); else go_right(d); break;
        case FK_UP: vert(d, -1); break;
        case FK_DOWN: vert(d, 1); break;
        case FK_PGUP: vert(d, -rows); d->top -= rows; if (d->top < 0) d->top = 0; break;
        case FK_PGDN: vert(d, rows); d->top += rows; break;
        case FK_HOME:
            if (ct) { d->cl = 0; d->cc = 0; }
            else {
                Line *l = &d->ln[d->cl];
                int k = 0;
                while (k < l->n && (l->s[k] == ' ' || l->s[k] == '\t')) k++;
                d->cc = d->cc == k ? 0 : k;
            }
            break;
        case FK_END:
            if (ct) { d->cl = d->nl - 1; }
            d->cc = d->ln[d->cl].n;
            break;
        }
        if (sh && d->al == d->cl && d->ac == d->cc) d->al = -1;
        return;
    }
    switch (c) {
    case FK_ENTER: enter_key(d); return;
    case FK_BACK: backspace(d, 0); return;
    case FK_DEL: backspace(d, 1); return;
    case FK_TAB: indent(d, sh); return;
    }
    if (c >= 32 && c < 0x200000 && !(e->mods & MOD_ALT)) {
        char t[4];
        int n = cp_utf8(c, t);
        type_text(d, t, n, 1);
    }
}

static void on_fb_key(FmEv *e) {
    LineEd *le = fb_foc ? &fb_repl : &fb_find;
    Doc *d = D;
    char old[512];
    int r;
    if (e->mods & MOD_CTRL) {
        int c = e->a;
        if (c == 'f') { fb_open(1); return; }
        if (c == 'h' || c == 'r') { fb_open(2); return; }
        if (c == 'g') { fb_open(3); return; }
        if (c == 's' || c == 'z' || c == 'y' || c == 'o' || c == 'w' || c == 'q' || c == 'n' || c == 't') { on_key(e); return; }
        if (c == FK_TAB) { on_key(e); return; }
    }
    if (e->a == FK_TAB && fb_mode == 2) { fb_foc ^= 1; return; }
    if (e->a == FK_DOWN || e->a == FK_UP) {
        if (fb_mode == 1) find_next(d, e->a == FK_UP);
        follow = 1;
        return;
    }
    strcpy(old, le->buf);
    r = le_key(le, e);
    if (r == 2) { fb_mode = 0; return; }
    if (r == 1) {
        if (fb_mode == 3) { goto_line(d, atoi(fb_find.buf)); fb_mode = 0; }
        else if (fb_foc == 1) replace_one(d);
        else find_next(d, (e->mods & MOD_SHIFT) != 0);
        follow = 1;
        return;
    }
    if (strcmp(old, le->buf) && !fb_foc && fb_mode != 3) {
        if (d->al >= 0) { int l1, c1, l2, c2; sel_order(d, &l1, &c1, &l2, &c2); d->cl = l1; d->cc = c1; d->al = -1; }
        find_next(d, 0);
        follow = 1;
    }
}

int edit_main(int argc, char **argv) {
    FmEv e;
    int i, blink = 0;
    char last_title[1200] = "";
    if (ui_open(900, 640, "edit") < 0) return 1;
    pal();
    cw = be_text_w("M", F_MONO) ? be_text_w("M", F_MONO) : 8;
    ch = fh_mono ? fh_mono : 16;
    for (i = 1; i < argc; i++) tab_open(argv[i]);
    if (!ndocs) tab_open(0);
    for (;;) {
        char t[1200];
        int r;
        title_upd();
        snprintf(t, sizeof t, "%d%d", D->dirty, cur);
        draw();
        r = be_wait(&e, 250);
        if (r < 0) break;
        if (r == 0) { blink = (now_ms() / 530) & 1; blink_on = blink; continue; }
        do {
            switch (e.type) {
            case EV_CLOSE: if (quit_all()) goto out; break;
            case EV_KEY: if (fb_mode && fb_foc >= 0 && !((e.mods & MOD_CTRL) && 0)) on_fb_key(&e); else on_key(&e); break;
            case EV_DOWN: if (e.c == 1) on_down(&e); mx = e.a; my = e.b; break;
            case EV_UP: dragging = sbdrag = 0; break;
            case EV_MOVE: on_move(&e); break;
            case EV_WHEEL: D->top += e.a * 3; if (D->top < 0) D->top = 0; if (D->top >= D->nl) D->top = D->nl - 1; break;
            }
        } while (be_wait(&e, 0) > 0);
        blink_on = 1;
        (void)last_title;
    }
out:
    be_close();
    return 0;
}
