/* SamaraOS Web Browser.
 *
 * Pages are fetched by the userland `curl` (DNS, HTTPS via mbedTLS,
 * redirects): the browser spawns it in the background and polls it from the
 * WM tick, so the desktop never freezes while a page loads. The result is
 * decoded (UTF-8 / windows-1251 -> CP866, the OS charset), parsed by a small
 * HTML tokenizer and laid out with the Golos UI fonts: headings, paragraphs,
 * lists, links, <pre>, rules and simple GET/POST forms (text fields and
 * submit buttons), which is enough for search boxes.
 *
 * No JavaScript and no CSS. Google search needs JavaScript nowadays, so
 * searches sent to google.com/search go to DuckDuckGo's HTML version. */

#include "apps/browser.h"
#include "gui/wm.h"
#include "gui/theme.h"
#include "gfx/gfx.h"
#include "gfx/uifont.h"
#include "core/string.h"
#include "core/heap.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "fs/fs.h"
#include "proc/proc.h"
#include "boot/pit.h"

/* ---------- tunables ---------- */
#define RESP_CAP     (2 * 1024 * 1024)   /* page bytes kept */
#define POOL_CAP     (1024 * 1024)       /* laid-out text */
#define URLPOOL_CAP  (512 * 1024)        /* link targets */
#define MAX_RUNS     24000
#define MAX_LINKS    6000
#define MAX_FIELDS   96
#define MAX_FORMS    24
#define URL_MAX      1024
#define FIELD_MAX    256
#define HIST_MAX     24

#define TB_H         48                  /* toolbar */
#define SB_H         26                  /* status bar */
#define PAD          20                  /* page margin */
#define BTN          32
#define F_MONO       UIF_MONO
#define FIELD_H      30

#define PAGE_FILE    "/tmp/.browser-page"
#define META_FILE    "/tmp/.browser-meta"
#define ERR_FILE     "/tmp/.browser-err"
#define CURL         "/usr/bin/curl"
#define USER_AGENT   "Dillo/3.0.5"        /* gets Google's classic no-JS page */
#define SEARCH_URL   "https://html.duckduckgo.com/html/?q="
#define HOME_URL     "about:home"

/* ---------- colours (theme family) ---------- */
#define COL_PAPER    RGB(0xFB, 0xFA, 0xF7)
#define COL_TEXT     C_INK
#define COL_LINK     RGB(0x1F, 0x5C, 0xB8)
#define COL_LINK_HOV RGB(0xC2, 0x6A, 0x12)

/* ---------- document model ---------- */
enum { RK_TEXT, RK_RULE, RK_BULLET, RK_FIELD, RK_BUTTON };
enum { FT_TEXT, FT_PASSWORD, FT_HIDDEN, FT_SUBMIT };

typedef struct {
    int      x, y;           /* document pixels */
    uint16_t w, h;
    int      off;            /* text in pool */
    uint16_t len;
    uint8_t  font, kind;
    uint32_t fg;
    int16_t  link, field;
} run_t;

typedef struct {
    int     form;
    uint8_t type;
    char    name[64];
    char    value[FIELD_MAX];
    int     len, cur;
} field_t;

typedef struct { int action; bool post; } form_t;   /* action: url_pool offset */

static uint8_t* resp;
static int      resp_len;
static char*    pool;
static int      pool_used;
static char*    url_pool;
static int      url_used;
static run_t*   runs;
static int      n_runs;
static int      link_off[MAX_LINKS];
static int      n_links;
static field_t  fields[MAX_FIELDS];
static int      n_fields, n_fields_prev;
static bool     keep_fields;
static form_t   forms[MAX_FORMS];
static int      n_forms;
static int      doc_h, scroll_y;
static char     page_title[64];
static char     refresh_url[URL_MAX];
static bool     plain_text;

/* ---------- browser state ---------- */
static window_t* g_win;
static char     cur_url[URL_MAX];     /* the page being shown */
static char     base_url[URL_MAX];    /* for relative links (after redirects) */
static char     edit[URL_MAX];        /* address bar text */
static int      edit_len, edit_cur;
static bool     edit_all;             /* whole address selected: typing replaces it */
enum { FOCUS_NONE = -2, FOCUS_URL = -1 };  /* >= 0: a form field */
static int      focus = FOCUS_URL;
static char     back_stack[HIST_MAX][URL_MAX], fwd_stack[HIST_MAX][URL_MAX];
static int      n_back, n_fwd;
static char     status[200];
static int      hover_link = -1, hover_btn = -1, hover_run = -1;
static bool     click_consumed;
static uint32_t now_ms;
static int      laid_w;
static int      auto_redirects;

/* in-flight fetch */
static int      fetch_pid = -1;
static bool     loading;
static uint32_t load_start;
static char     load_url[URL_MAX];

/* hit boxes (screen coords, filled by paint) */
typedef struct { int x, y, w, h; } box_t;
enum { B_BACK, B_FWD, B_RELOAD, B_HOME, B_COUNT };
static box_t    btn_box[B_COUNT], url_box, view_box;

static bool in_box(box_t b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void scpy(char* d, int cap, const char* s) {
    int i = 0;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void scat(char* d, int cap, const char* s) {
    int n = (int)strlen(d);
    scpy(d + n, cap - n, s);
}
static bool starts_ci(const char* s, const char* p) {
    for (; *p; s++, p++) if (lower(*s) != lower(*p)) return false;
    return true;
}
static const char* find_ci(const char* h, int n, const char* needle) {
    int k = (int)strlen(needle);
    for (int i = 0; i + k <= n; i++) {
        int j = 0;
        while (j < k && lower(h[i + j]) == needle[j]) j++;
        if (j == k) return h + i;
    }
    return NULL;
}
static void set_status(const char* a, const char* b) {
    scpy(status, sizeof status, a);
    if (b) scat(status, sizeof status, b);
}

/* ======================================================================
   Character sets. Everything inside is CP866 (what the fonts draw).
   ====================================================================== */

static const char latin1_fold[65] =            /* U+00C0..U+00FF -> ASCII */
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";

/* One code point -> up to 3 CP866 bytes; returns the count (0 = drop). */
static int uni_to_cp866(uint32_t cp, uint8_t* o) {
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp >= 0x410 && cp <= 0x43F) { o[0] = (uint8_t)(0x80 + (cp - 0x410)); return 1; }
    if (cp >= 0x440 && cp <= 0x44F) { o[0] = (uint8_t)(0xE0 + (cp - 0x440)); return 1; }
    if (cp == 0x401) { o[0] = 0xF0; return 1; }
    if (cp == 0x451) { o[0] = 0xF1; return 1; }
    if (cp == 0x404) { o[0] = 0xF2; return 1; }
    if (cp == 0x454) { o[0] = 0xF3; return 1; }
    if (cp == 0x407) { o[0] = 0xF4; return 1; }
    if (cp == 0x457) { o[0] = 0xF5; return 1; }
    if (cp == 0x406) { o[0] = 'I'; return 1; }
    if (cp == 0x456) { o[0] = 'i'; return 1; }
    if (cp >= 0xC0 && cp <= 0xFF) { o[0] = (uint8_t)latin1_fold[cp - 0xC0]; return 1; }
    switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: o[0] = ' '; return 1;
    case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: o[0] = '"'; return 1;
    case 0x2018: case 0x2019: case 0x201A: case 0xB4: o[0] = '\''; return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        o[0] = '-'; return 1;
    case 0x2022: case 0xB7: case 0x25CF: case 0x2219: o[0] = 0xF9; return 1;
    case 0xB0: o[0] = 0xF8; return 1;
    case 0x2116: o[0] = 0xFC; return 1;
    case 0xA9: o[0] = '('; o[1] = 'c'; o[2] = ')'; return 3;
    case 0xAE: o[0] = '('; o[1] = 'R'; o[2] = ')'; return 3;
    case 0x2122: o[0] = 'T'; o[1] = 'M'; return 2;
    case 0x2026: o[0] = o[1] = o[2] = '.'; return 3;
    case 0x2190: o[0] = '<'; return 1;
    case 0x2192: o[0] = '>'; return 1;
    case 0xD7: o[0] = 'x'; return 1;
    case 0x20AC: o[0] = 'E'; return 1;
    case 0x20BD: o[0] = 0xE0; return 1;           /* rouble sign -> р */
    }
    if (cp >= 0x250 || cp == 0xAD) return 0;     /* other scripts, symbols, emoji, soft hyphen */
    o[0] = '?';
    return 1;
}

/* In place: output is never longer than input. */
static int utf8_to_cp866(uint8_t* b, int n) {
    int r = 0, w = 0;
    while (r < n) {
        uint8_t c = b[r];
        uint32_t cp; int k;
        if (c < 0x80) { b[w++] = c; r++; continue; }
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 3; }
        else { r++; continue; }
        bool ok = true;
        for (int i = 1; i <= k; i++) {
            if (r + i >= n || (b[r + i] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (b[r + i] & 0x3F);
        }
        if (!ok) { r++; continue; }
        r += k + 1;
        uint8_t o[3];
        int m = uni_to_cp866(cp, o);
        if (m > k + 1) m = k + 1;
        for (int i = 0; i < m; i++) b[w++] = o[i];
    }
    return w;
}

static int cp1251_to_cp866(uint8_t* b, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c < 0x80) continue;
        if (c >= 0xC0 && c <= 0xEF) b[i] = (uint8_t)(0x80 + (c - 0xC0));
        else if (c >= 0xF0) b[i] = (uint8_t)(0xE0 + (c - 0xF0));
        else if (c == 0xA8) b[i] = 0xF0;
        else if (c == 0xB8) b[i] = 0xF1;
        else if (c == 0xA0) b[i] = ' ';
        else if (c == 0x96 || c == 0x97) b[i] = '-';
        else if (c == 0xAB || c == 0xBB || c == 0x93 || c == 0x94) b[i] = '"';
        else if (c == 0x91 || c == 0x92) b[i] = '\'';
        else if (c == 0xB9) b[i] = 0xFC;
        else b[i] = '?';
    }
    return n;
}

/* CP866 byte -> UTF-8 (for URLs and form data). Returns bytes written. */
static int cp866_to_utf8(uint8_t c, uint8_t* o) {
    uint32_t cp;
    if (c < 0x80) { o[0] = c; return 1; }
    if (c <= 0xAF) cp = 0x410 + (c - 0x80);
    else if (c >= 0xE0 && c <= 0xEF) cp = 0x440 + (c - 0xE0);
    else if (c == 0xF0) cp = 0x401;
    else if (c == 0xF1) cp = 0x451;
    else { o[0] = '?'; return 1; }
    o[0] = (uint8_t)(0xC0 | (cp >> 6));
    o[1] = (uint8_t)(0x80 | (cp & 0x3F));
    return 2;
}

static const char HEX[] = "0123456789ABCDEF";

/* application/x-www-form-urlencoded, from CP866 text */
static void form_encode(const char* s, int n, char* out, int* op, int cap) {
    for (int i = 0; i < n && *op < cap - 7; i++) {
        uint8_t u[2];
        int m = cp866_to_utf8((uint8_t)s[i], u);
        for (int j = 0; j < m; j++) {
            uint8_t c = u[j];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') out[(*op)++] = (char)c;
            else if (c == ' ') out[(*op)++] = '+';
            else { out[(*op)++] = '%'; out[(*op)++] = HEX[c >> 4]; out[(*op)++] = HEX[c & 15]; }
        }
    }
    out[*op] = 0;
}

/* Internal (CP866) URL -> wire form: spaces/non-ASCII become %XX of UTF-8. */
static void wire_url(const char* in, char* out, int cap) {
    int p = 0;
    for (int i = 0; in[i] && p < cap - 7; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c > 0x20 && c < 0x7F && c != '"' && c != '<' && c != '>' && c != '\\' && c != '^' &&
            c != '`' && c != '{' && c != '|' && c != '}') { out[p++] = (char)c; continue; }
        uint8_t u[2];
        int m = cp866_to_utf8(c, u);
        for (int j = 0; j < m; j++) { out[p++] = '%'; out[p++] = HEX[u[j] >> 4]; out[p++] = HEX[u[j] & 15]; }
    }
    out[p] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* ======================================================================
   URLs
   ====================================================================== */

static bool url_parts(const char* u, int* scheme_end, int* host_end) {
    const char* s = strstr(u, "://");
    if (!s) return false;
    *scheme_end = (int)(s - u);
    int h = *scheme_end + 3;
    while (u[h] && u[h] != '/' && u[h] != '?' && u[h] != '#') h++;
    *host_end = h;
    return true;
}

static void url_host(const char* u, char* out, int cap) {
    int se, he;
    out[0] = 0;
    if (!url_parts(u, &se, &he)) return;
    int n = he - se - 3;
    if (n >= cap) n = cap - 1;
    memcpy(out, u + se + 3, (size_t)n);
    out[n] = 0;
}

static void remove_dot_segments(char* url) {
    int se, he;
    if (!url_parts(url, &se, &he)) return;
    char* path = url + he;
    static char out[URL_MAX];
    int o = 0, i = 0;
    while (path[i] && path[i] != '?' && path[i] != '#') {
        if (path[i] == '/' && path[i + 1] == '.' &&
            (path[i + 2] == '/' || path[i + 2] == 0 || path[i + 2] == '?')) { i += 2; continue; }
        if (path[i] == '/' && path[i + 1] == '.' && path[i + 2] == '.' &&
            (path[i + 3] == '/' || path[i + 3] == 0 || path[i + 3] == '?')) {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            i += 3;
            continue;
        }
        if (o < URL_MAX - 1) out[o++] = path[i];
        i++;
    }
    if (o == 0) out[o++] = '/';
    while (path[i] && o < URL_MAX - 1) out[o++] = path[i++];
    out[o] = 0;
    scpy(path, URL_MAX - he, out);
}

/* Value of query parameter `key`, percent-decoded (raw bytes). */
static bool query_param(const char* url, const char* key, char* out, int cap) {
    const char* q = strchr(url, '?');
    if (!q) return false;
    int kl = (int)strlen(key);
    for (const char* p = q + 1; *p; ) {
        if (!strncmp(p, key, (size_t)kl) && p[kl] == '=') {
            p += kl + 1;
            int o = 0;
            while (*p && *p != '&' && *p != '#' && o < cap - 1) {
                if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3;
                } else if (*p == '+') { out[o++] = ' '; p++; }
                else out[o++] = *p++;
            }
            out[o] = 0;
            return true;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

/* Redirector links (DuckDuckGo /l/?uddg=, Google /url?q=) -> the target. */
static void unwrap_redirect(char* url) {
    int se, he;
    if (!url_parts(url, &se, &he)) return;
    char host[128];
    url_host(url, host, sizeof host);
    const char* path = url + he;
    static char tgt[URL_MAX];
    bool got = false;
    if (strstr(host, "duckduckgo.com") && !strncmp(path, "/l/", 3))
        got = query_param(url, "uddg", tgt, sizeof tgt);
    else if (strstr(host, "google.") && !strncmp(path, "/url?", 5))
        got = query_param(url, "q", tgt, sizeof tgt) || query_param(url, "url", tgt, sizeof tgt);
    if (!got || strncmp(tgt, "http", 4)) return;
    int n = utf8_to_cp866((uint8_t*)tgt, (int)strlen(tgt));
    tgt[n] = 0;
    scpy(url, URL_MAX, tgt);
}

/* href relative to base -> absolute URL. false for javascript:, mailto:, #frag. */
static bool resolve_url(const char* base, const char* href, char* out) {
    while (*href == ' ' || *href == '\t' || *href == '\n' || *href == '\r') href++;
    if (!*href || href[0] == '#') return false;
    if (starts_ci(href, "javascript:") || starts_ci(href, "mailto:") || starts_ci(href, "tel:") ||
        starts_ci(href, "data:")) return false;
    int se, he;
    if (starts_ci(href, "http://") || starts_ci(href, "https://") || starts_ci(href, "about:")) {
        scpy(out, URL_MAX, href);
    } else if (!url_parts(base, &se, &he)) {
        return false;
    } else if (href[0] == '/' && href[1] == '/') {
        memcpy(out, base, (size_t)se + 1);
        out[se + 1] = 0;
        scat(out, URL_MAX, href);
    } else if (href[0] == '/') {
        memcpy(out, base, (size_t)he);
        out[he] = 0;
        scat(out, URL_MAX, href);
    } else if (href[0] == '?') {
        int e = he;
        while (base[e] && base[e] != '?' && base[e] != '#') e++;
        memcpy(out, base, (size_t)e);
        out[e] = 0;
        scat(out, URL_MAX, href);
    } else {
        int e = he, slash = -1;
        while (base[e] && base[e] != '?' && base[e] != '#') { if (base[e] == '/') slash = e; e++; }
        if (slash < 0) { memcpy(out, base, (size_t)he); out[he] = '/'; out[he + 1] = 0; }
        else { memcpy(out, base, (size_t)slash + 1); out[slash + 1] = 0; }
        scat(out, URL_MAX, href);
    }
    char* h = strchr(out, '#');
    if (h) *h = 0;
    for (char* p = out; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    remove_dot_segments(out);
    unwrap_redirect(out);
    return true;
}

static void search_url(const char* q, char* out) {
    scpy(out, URL_MAX, SEARCH_URL);
    int p = (int)strlen(out);
    form_encode(q, (int)strlen(q), out, &p, URL_MAX);
}

/* What the user typed -> a URL (or a search). */
static void normalize_typed(const char* in, char* out) {
    while (*in == ' ') in++;
    static char t[URL_MAX];
    scpy(t, sizeof t, in);
    int n = (int)strlen(t);
    while (n > 0 && t[n - 1] == ' ') t[--n] = 0;
    if (!n) { scpy(out, URL_MAX, HOME_URL); return; }
    if (strstr(t, "://") || starts_ci(t, "about:")) { scpy(out, URL_MAX, t); return; }
    bool space = strchr(t, ' ') != NULL, dot = strchr(t, '.') != NULL;
    bool local = starts_ci(t, "host") || starts_ci(t, "localhost") ||
                 (t[0] >= '0' && t[0] <= '9' && dot);
    if (space || (!dot && !local)) { search_url(t, out); return; }
    scpy(out, URL_MAX, local ? "http://" : "https://");
    scat(out, URL_MAX, t);
}

/* Google's results need JavaScript: send its searches to DuckDuckGo. */
static bool reroute_search(char* url) {
    char host[128];
    url_host(url, host, sizeof host);
    int se, he;
    if (!strstr(host, "google.") || !url_parts(url, &se, &he) || strncmp(url + he, "/search", 7)) return false;
    static char q[FIELD_MAX * 3];
    if (!query_param(url, "q", q, sizeof q)) return false;
    int n = utf8_to_cp866((uint8_t*)q, (int)strlen(q));
    q[n] = 0;
    search_url(q, url);
    return true;
}

/* ======================================================================
   Layout
   ====================================================================== */

static int font_h(int f) { return f == F_MONO ? 16 : uif_height((uif_t)f); }
static int text_w(int f, const char* s, int n) { return f == F_MONO ? n * 8 : uif_width_n((uif_t)f, s, n); }

typedef struct {
    int  view_w, cx, cy, line_h, line_first, margin, indent;
    bool dirty, space;
    int  bold, mono, small, h, pre, center;
    int  link;
    int  list_depth;
    int  list_num[10];                 /* 0 = ul, else next ol number */
    int  form;
} lay_t;

static int cur_font(lay_t* L) {
    if (L->h == 1 || L->h == 2) return UIF_BIG;
    if (L->h) return UIF_MED;
    if (L->mono || L->pre) return F_MONO;
    if (L->bold) return UIF_MED;
    if (L->small) return UIF_SMALL;
    return UIF_REG;
}
static uint32_t cur_fg(lay_t* L) { return L->link >= 0 ? COL_LINK : COL_TEXT; }

static int pool_put(const char* s, int n) {
    if (pool_used + n + 1 > POOL_CAP) return -1;
    int off = pool_used;
    memcpy(pool + off, s, (size_t)n);
    pool[off + n] = 0;
    pool_used += n + 1;
    return off;
}
static int url_put(const char* s) {
    int n = (int)strlen(s);
    if (url_used + n + 1 > URLPOOL_CAP) return -1;
    int off = url_used;
    memcpy(url_pool + off, s, (size_t)n + 1);
    url_used += n + 1;
    return off;
}

static run_t* add_run(int kind, int x, int y, int w, int h, int font, uint32_t fg,
                      int link, int field, int off, int len) {
    if (n_runs >= MAX_RUNS) return NULL;
    run_t* r = &runs[n_runs++];
    r->kind = (uint8_t)kind; r->x = x; r->y = y; r->w = (uint16_t)w; r->h = (uint16_t)h;
    r->font = (uint8_t)font; r->fg = fg; r->link = (int16_t)link; r->field = (int16_t)field;
    r->off = off; r->len = (uint16_t)len;
    return r;
}

static int left_x(lay_t* L) { return PAD + L->indent; }

static void end_line(lay_t* L) {
    if (!L->dirty) return;
    int shift = 0;
    if (L->center) {
        int w = L->cx - left_x(L), avail = L->view_w - PAD - left_x(L);
        if (w < avail) shift = (avail - w) / 2;
    }
    for (int i = L->line_first; i < n_runs; i++) {
        runs[i].y = L->cy + (L->line_h - runs[i].h) / 2;
        runs[i].x += shift;
    }
    L->cy += L->line_h + 3;
    L->line_h = 0;
    L->dirty = false;
    L->space = false;
}

static void block(lay_t* L, int margin) {
    end_line(L);
    if (margin > L->margin) L->margin = margin;
}

static void need_line(lay_t* L) {
    if (L->dirty) return;
    if (L->margin) { L->cy += L->margin; L->margin = 0; }
    L->cx = left_x(L);
    L->line_first = n_runs;
}

static void place(lay_t* L, int w, int h) {
    L->dirty = true;
    if (h > L->line_h) L->line_h = h;
    L->cx += w;
}

static void emit_piece(lay_t* L, const char* s, int n, int f, int sp) {
    uint32_t fg = cur_fg(L);
    int w = text_w(f, s, n);
    run_t* p = n_runs > L->line_first ? &runs[n_runs - 1] : NULL;
    if (p && p->kind == RK_TEXT && p->font == f && p->fg == fg && p->link == L->link &&
        p->off + p->len + 1 == pool_used && p->x + p->w == L->cx &&
        pool_used + n + 2 < POOL_CAP && p->len + n + 1 < 60000) {
        int at = pool_used - 1;
        if (sp) pool[at++] = ' ';
        memcpy(pool + at, s, (size_t)n);
        pool[at + n] = 0;
        pool_used = at + n + 1;
        p->len = (uint16_t)(p->len + (sp ? 1 : 0) + n);
        p->w = (uint16_t)(p->w + sp + w);
        place(L, sp + w, font_h(f));
        return;
    }
    L->cx += sp;
    int off = pool_put(s, n);
    if (off < 0) return;
    add_run(RK_TEXT, L->cx, L->cy, w, font_h(f), f, fg, L->link, -1, off, n);
    place(L, w, font_h(f));
}

static void emit_word(lay_t* L, const char* s, int n) {
    if (n <= 0) return;
    int f = cur_font(L);
    int avail = L->view_w - PAD - left_x(L);
    if (avail < 40) avail = 40;
    int sp = (L->space && L->dirty) ? text_w(f, " ", 1) : 0;
    L->space = false;
    int w = text_w(f, s, n);
    if (L->dirty && L->cx + sp + w > L->view_w - PAD) { end_line(L); sp = 0; }
    while (w > avail && n > 1) {                 /* longer than a line: hard-break it */
        need_line(L);
        int k = n;
        while (k > 1 && text_w(f, s, k) > avail - (L->cx - left_x(L))) k = k * 3 / 4 < k - 1 ? k * 3 / 4 : k - 1;
        emit_piece(L, s, k, f, sp);
        end_line(L);
        s += k; n -= k; sp = 0;
        w = text_w(f, s, n);
    }
    need_line(L);
    emit_piece(L, s, n, f, sp);
}

static void emit_box(lay_t* L, int kind, int w, int h, int field, int off, int len) {
    int sp = L->dirty ? (L->space ? 8 : 4) : 0;
    L->space = false;
    if (L->dirty && L->cx + sp + w > L->view_w - PAD) { end_line(L); sp = 0; }
    need_line(L);
    L->cx += sp;
    add_run(kind, L->cx, L->cy, w, h, UIF_MED, COL_TEXT, L->link, field, off, len);
    place(L, w, h);
}

static void emit_rule(lay_t* L) {
    block(L, 8);
    need_line(L);
    add_run(RK_RULE, left_x(L), L->cy, L->view_w - PAD - left_x(L), 1, 0, C_RULE, -1, -1, 0, 0);
    L->cy += 1;
    L->margin = 8;
}

/* ---------- entities ---------- */
static const struct { const char* n; uint16_t cp; } ents[] = {
    {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",0xA0},
    {"mdash",0x2014},{"ndash",0x2013},{"laquo",0xAB},{"raquo",0xBB},{"hellip",0x2026},
    {"copy",0xA9},{"reg",0xAE},{"trade",0x2122},{"bull",0x2022},{"middot",0xB7},
    {"rsquo",0x2019},{"lsquo",0x2018},{"ldquo",0x201C},{"rdquo",0x201D},{"bdquo",0x201E},
    {"times",0xD7},{"deg",0xB0},{"euro",0x20AC},{"shy",0xAD},{"larr",0x2190},{"rarr",0x2192},
    {"minus",0x2212},{"numero",0x2116},{"thinsp",0x2009},{"ensp",0x2002},{"emsp",0x2003},
    {"zwnj",0x200C},{"zwj",0x200D},{"lrm",0x200E},{"rlm",0x200F},
};

/* Decode entities of s[0..n) into out; returns length. */
static int decode_entities(const char* s, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 4; i++) {
        if (s[i] != '&') { out[o++] = s[i]; continue; }
        int j = i + 1;
        uint32_t cp = 0;
        bool ok = false;
        if (j < n && s[j] == '#') {
            j++;
            bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
            if (hex) j++;
            int d0 = j;
            while (j < n && j - d0 < 8) {
                int v = hex ? hexval(s[j]) : (s[j] >= '0' && s[j] <= '9' ? s[j] - '0' : -1);
                if (v < 0) break;
                cp = cp * (hex ? 16 : 10) + (uint32_t)v;
                j++;
            }
            ok = j > d0;
        } else {
            int k0 = j;
            while (j < n && j - k0 < 10 && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
            for (unsigned e = 0; e < sizeof ents / sizeof ents[0]; e++)
                if ((int)strlen(ents[e].n) == j - k0 && !strncmp(ents[e].n, s + k0, (size_t)(j - k0))) {
                    cp = ents[e].cp; ok = true; break;
                }
        }
        if (!ok) { out[o++] = '&'; continue; }
        if (j < n && s[j] == ';') j++;
        i = j - 1;
        uint8_t b[3];
        int m = uni_to_cp866(cp, b);
        for (int k = 0; k < m; k++) out[o++] = (char)b[k];
    }
    out[o] = 0;
    return o;
}

/* ---------- tags ---------- */
typedef struct { const char* a; int n; } attrs_t;

static bool get_attr(attrs_t A, const char* key, char* out, int cap) {
    const char* s = A.a;
    int n = A.n, i = 0, kl = (int)strlen(key);
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '/')) i++;
        int k0 = i;
        while (i < n && s[i] != '=' && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r' && s[i] != '>') i++;
        int k1 = i;
        while (i < n && (s[i] == ' ' || s[i] == '\n')) i++;
        int v0 = i, v1 = i;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && (s[i] == ' ' || s[i] == '\n')) i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                v0 = i;
                while (i < n && s[i] != q) i++;
                v1 = i;
                if (i < n) i++;
            } else {
                v0 = i;
                while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '>') i++;
                v1 = i;
            }
        }
        if (k1 == k0) { if (i == k0) i++; continue; }
        bool match = (k1 - k0 == kl);
        for (int j = 0; match && j < kl; j++) if (lower(s[k0 + j]) != key[j]) match = false;
        if (match) {
            if (out) decode_entities(s + v0, v1 - v0, out, cap);
            return true;
        }
    }
    return false;
}

static bool tag_is(const char* t, const char* const* list) {
    for (; *list; list++) if (!strcmp(t, *list)) return true;
    return false;
}
static const char* const T_BLOCK[] = { "div", "section", "article", "header", "footer", "nav", "main",
    "aside", "table", "tr", "center", "figure", "figcaption", "address", "fieldset", "details",
    "summary", "dd", "dt", "dl", "tbody", "thead", "caption", "legend", "html", "body", 0 };
static const char* const T_SKIP[] = { "script", "style", "svg", "template", "select", "noembed",
    "object", "math", "datalist", 0 };

static int add_field(lay_t* L, int type, attrs_t A) {
    if (n_fields >= MAX_FIELDS) return -1;
    int i = n_fields++;
    field_t* f = &fields[i];
    char name[64];
    if (!get_attr(A, "name", name, sizeof name)) name[0] = 0;
    bool keep = keep_fields && i < n_fields_prev && !strcmp(f->name, name) && f->type == type;
    f->form = L->form;
    f->type = (uint8_t)type;
    scpy(f->name, sizeof f->name, name);
    if (!keep) {
        if (!get_attr(A, "value", f->value, sizeof f->value)) f->value[0] = 0;
        f->len = (int)strlen(f->value);
        f->cur = f->len;
    }
    return i;
}

static void do_input(lay_t* L, attrs_t A) {
    char type[16];
    if (!get_attr(A, "type", type, sizeof type)) type[0] = 0;
    for (char* p = type; *p; p++) *p = lower(*p);
    char tmp[80];
    bool css_hidden = (get_attr(A, "style", tmp, sizeof tmp) && find_ci(tmp, (int)strlen(tmp), "none")) ||
                      (get_attr(A, "name", tmp, sizeof tmp) && find_ci(tmp, (int)strlen(tmp), "hidden"));
    if (!strcmp(type, "hidden") || (css_hidden && strcmp(type, "submit"))) { add_field(L, FT_HIDDEN, A); return; }
    if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        if (get_attr(A, "checked", NULL, 0)) {
            int i = add_field(L, FT_HIDDEN, A);
            if (i >= 0 && !fields[i].value[0]) { scpy(fields[i].value, FIELD_MAX, "on"); fields[i].len = 2; }
        }
        return;
    }
    if (!strcmp(type, "submit") || !strcmp(type, "image")) {
        int i = add_field(L, FT_SUBMIT, A);
        if (i < 0) return;
        static char lab[64];
        if (fields[i].value[0]) scpy(lab, sizeof lab, fields[i].value);
        else if (!get_attr(A, "title", lab, sizeof lab) && !get_attr(A, "alt", lab, sizeof lab)) lab[0] = 0;
        if (!lab[0]) scpy(lab, sizeof lab, "Submit");
        const char* label = lab;
        int n = (int)strlen(label);
        int off = pool_put(label, n);
        emit_box(L, RK_BUTTON, uif_width_n(UIF_MED, label, n) + 28, FIELD_H, i, off, n);
        return;
    }
    if (type[0] && strcmp(type, "text") && strcmp(type, "search") && strcmp(type, "email") &&
        strcmp(type, "url") && strcmp(type, "password") && strcmp(type, "tel") && strcmp(type, "number"))
        return;                                        /* reset, file, button, ... */
    int i = add_field(L, !strcmp(type, "password") ? FT_PASSWORD : FT_TEXT, A);
    if (i < 0) return;
    char sz[8];
    int w = 280;
    if (get_attr(A, "size", sz, sizeof sz)) { w = atoi(sz) * 9 + 20; }
    if (w < 120) w = 120;
    if (w > 480) w = 480;
    int avail = L->view_w - PAD - left_x(L);
    if (w > avail) w = avail;
    emit_box(L, RK_FIELD, w, FIELD_H, i, 0, 0);
}

/* Returns the position right after "</name...>" (or n). */
static int skip_to_close(const char* s, int i, int n, const char* name, int* inner_end) {
    char pat[16] = "</";
    scat(pat, sizeof pat, name);
    const char* e = find_ci(s + i, n - i, pat);
    if (!e) { if (inner_end) *inner_end = n; return n; }
    if (inner_end) *inner_end = (int)(e - s);
    int j = (int)(e - s);
    while (j < n && s[j] != '>') j++;
    return j < n ? j + 1 : n;
}

/* Text without tags, collapsed (for <title>, <button>, <textarea>). */
static void inner_text(const char* s, int n, char* out, int cap, bool keep_ws) {
    static char tmp[1024];
    int t = 0;
    for (int i = 0; i < n && t < (int)sizeof tmp - 1; i++) {
        if (s[i] == '<') { while (i < n && s[i] != '>') i++; continue; }
        tmp[t++] = s[i];
    }
    int m = decode_entities(tmp, t, tmp, sizeof tmp);
    int o = 0;
    bool sp = false;
    for (int i = 0; i < m && o < cap - 1; i++) {
        char c = tmp[i];
        if (!keep_ws && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) { sp = o > 0; continue; }
        if (sp) { out[o++] = ' '; sp = false; if (o >= cap - 1) break; }
        out[o++] = c;
    }
    out[o] = 0;
}

static void do_text(lay_t* L, const char* s, int n) {
    static char buf[8192];
    while (n > 0) {
        int chunk = n > 4000 ? 4000 : n;
        if (chunk < n) while (chunk > 1 && s[chunk - 1] != ' ' && s[chunk - 1] != '\n') chunk--;
        int m = decode_entities(s, chunk, buf, sizeof buf);
        s += chunk; n -= chunk;
        if (L->pre) {
            int a = 0;
            for (int i = 0; i <= m; i++) {
                if (i < m && buf[i] != '\n') continue;
                if (i > a) {                          /* expand tabs, keep spaces */
                    static char line[1024];
                    int o = 0;
                    for (int k = a; k < i && o < 1000; k++) {
                        if (buf[k] == '\t') { do line[o++] = ' '; while (o % 8 && o < 1000); }
                        else if (buf[k] != '\r') line[o++] = buf[k];
                    }
                    L->space = false;
                    emit_word(L, line, o);
                }
                if (i < m) {
                    if (!L->dirty) { need_line(L); L->cy += 16 + 3; }
                    else end_line(L);
                }
                a = i + 1;
            }
            continue;
        }
        int i = 0;
        while (i < m) {
            char c = buf[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f') { L->space = true; i++; continue; }
            int j = i;
            while (j < m && buf[j] != ' ' && buf[j] != '\n' && buf[j] != '\r' && buf[j] != '\t' && buf[j] != '\f') j++;
            emit_word(L, buf + i, j - i);
            i = j;
        }
    }
}

static void do_tag(lay_t* L, const char* src, int n, int* pos, const char* t, bool close, attrs_t A) {
    static char v[URL_MAX], abs[URL_MAX];
    if (tag_is(t, T_BLOCK)) {
        block(L, 0);
        if (!strcmp(t, "center")) L->center += close ? -1 : 1;
        if (L->center < 0) L->center = 0;
        return;
    }
    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) {
        block(L, close ? 8 : (t[1] <= '2' ? 18 : 12));
        L->h = close ? 0 : t[1] - '0';
        return;
    }
    if (!strcmp(t, "p")) { block(L, 10); return; }
    if (!strcmp(t, "br")) {
        if (L->dirty) end_line(L);
        else { need_line(L); L->cy += font_h(cur_font(L)) / 2; }
        return;
    }
    if (!strcmp(t, "hr")) { emit_rule(L); return; }
    if (!strcmp(t, "ul") || !strcmp(t, "ol") || !strcmp(t, "menu")) {
        block(L, L->list_depth ? 0 : 8);
        if (!close) {
            if (L->list_depth < 9) L->list_num[++L->list_depth] = (t[0] == 'o') ? 1 : 0;
            L->indent += 26;
        } else if (L->list_depth > 0) {
            L->list_depth--;
            L->indent -= 26;
            if (L->indent < 0) L->indent = 0;
        }
        return;
    }
    if (!strcmp(t, "li")) {
        block(L, 3);
        if (close) return;
        need_line(L);
        int f = cur_font(L), fh = font_h(f);
        int d = L->list_depth;
        if (d > 0 && L->list_num[d] > 0) {
            char num[12];
            itoa(L->list_num[d]++, num, 10);
            scat(num, sizeof num, ".");
            int nl = (int)strlen(num);
            int w = text_w(f, num, nl), off = pool_put(num, nl);
            if (off >= 0) add_run(RK_TEXT, L->cx - w - 6, L->cy, w, fh, f, C_INK_DIM, -1, -1, off, nl);
        } else {
            add_run(RK_BULLET, L->cx - 14, L->cy, 6, fh, 0, d > 1 ? C_GLYPH_DIM : C_GLYPH, -1, -1, 0, 0);
        }
        L->dirty = true;
        if (fh > L->line_h) L->line_h = fh;
        L->space = false;
        return;
    }
    if (!strcmp(t, "blockquote")) {
        block(L, 8);
        L->indent += close ? -24 : 24;
        if (L->indent < 0) L->indent = 0;
        return;
    }
    if (!strcmp(t, "pre")) {
        block(L, 8);
        L->pre += close ? -1 : 1;
        if (L->pre < 0) L->pre = 0;
        if (!close && *pos < n && src[*pos] == '\n') (*pos)++;
        return;
    }
    if (!strcmp(t, "code") || !strcmp(t, "tt") || !strcmp(t, "kbd") || !strcmp(t, "samp")) {
        L->mono += close ? -1 : 1; if (L->mono < 0) L->mono = 0; return;
    }
    if (!strcmp(t, "b") || !strcmp(t, "strong") || !strcmp(t, "th")) {
        if (t[1] == 'h') { if (!close && L->dirty) { L->cx += 16; L->space = false; } }
        L->bold += close ? -1 : 1; if (L->bold < 0) L->bold = 0; return;
    }
    if (!strcmp(t, "small") || !strcmp(t, "sub") || !strcmp(t, "sup")) {
        L->small += close ? -1 : 1; if (L->small < 0) L->small = 0; return;
    }
    if (!strcmp(t, "td")) {
        if (!close && L->dirty) { L->cx += 16; L->space = false; }
        return;
    }
    if (!strcmp(t, "a")) {
        if (close) { L->link = -1; return; }
        if (L->dirty && n_runs > L->line_first && runs[n_runs - 1].link >= 0) L->space = true;
        L->link = -1;
        if (get_attr(A, "href", v, sizeof v) && n_links < MAX_LINKS && resolve_url(base_url, v, abs)) {
            int off = url_put(abs);
            if (off >= 0) { link_off[n_links] = off; L->link = n_links++; }
        }
        return;
    }
    if (!strcmp(t, "img")) {
        if (!close && get_attr(A, "alt", v, 200) && v[0]) {
            bool sp = L->space;
            L->small++;
            L->space = sp;
            do_text(L, v, (int)strlen(v));
            L->small--;
        }
        return;
    }
    if (!strcmp(t, "form")) {
        block(L, 6);
        if (close) { L->form = -1; return; }
        if (n_forms >= MAX_FORMS) { L->form = -1; return; }
        form_t* f = &forms[n_forms];
        f->post = get_attr(A, "method", v, 16) && (v[0] == 'p' || v[0] == 'P');
        if (!get_attr(A, "action", v, sizeof v) || !resolve_url(base_url, v, abs)) scpy(abs, URL_MAX, base_url);
        f->action = url_put(abs);
        L->form = n_forms++;
        return;
    }
    if (!strcmp(t, "input")) { if (!close) do_input(L, A); return; }
    if (!strcmp(t, "button")) {
        if (close) return;
        int inner_end;
        int after = skip_to_close(src, *pos, n, "button", &inner_end);
        char type[16];
        if (!get_attr(A, "type", type, sizeof type)) scpy(type, sizeof type, "submit");
        if (lower(type[0]) != 's') { *pos = after; return; }
        int i = add_field(L, FT_SUBMIT, A);
        if (i < 0) { *pos = after; return; }
        inner_text(src + *pos, inner_end - *pos, v, 64, false);
        if (!v[0]) scpy(v, 64, fields[i].value[0] ? fields[i].value : "Submit");
        int ln = (int)strlen(v), off = pool_put(v, ln);
        emit_box(L, RK_BUTTON, uif_width_n(UIF_MED, v, ln) + 28, FIELD_H, i, off, ln);
        *pos = after;
        return;
    }
    if (!strcmp(t, "textarea")) {
        if (close) return;
        int inner_end;
        int after = skip_to_close(src, *pos, n, "textarea", &inner_end);
        int i = add_field(L, FT_TEXT, A);
        if (i >= 0) {
            if (!(keep_fields && i < n_fields_prev)) {
                inner_text(src + *pos, inner_end - *pos, fields[i].value, FIELD_MAX, false);
                fields[i].len = fields[i].cur = (int)strlen(fields[i].value);
            }
            int avail = L->view_w - PAD - left_x(L);
            block(L, 4);
            emit_box(L, RK_FIELD, avail < 480 ? avail : 480, FIELD_H, i, 0, 0);
            block(L, 4);
        }
        *pos = after;
        return;
    }
    if (!strcmp(t, "title")) {
        int inner_end;
        int after = skip_to_close(src, *pos, n, "title", &inner_end);
        if (!page_title[0]) inner_text(src + *pos, inner_end - *pos, page_title, sizeof page_title, false);
        *pos = after;
        return;
    }
    if (!strcmp(t, "base")) {
        if (get_attr(A, "href", v, sizeof v) && resolve_url(base_url, v, abs)) scpy(base_url, URL_MAX, abs);
        return;
    }
    if (!strcmp(t, "meta")) {
        if (get_attr(A, "http-equiv", v, 32) && starts_ci(v, "refresh") && get_attr(A, "content", v, sizeof v)) {
            const char* u = find_ci(v, (int)strlen(v), "url=");
            if (atoi(v) <= 5 && u) {
                u += 4;
                if (*u == '\'' || *u == '"') u++;
                scpy(abs, URL_MAX, u);
                int k = (int)strlen(abs);
                if (k && (abs[k - 1] == '\'' || abs[k - 1] == '"')) abs[k - 1] = 0;
                if (!resolve_url(base_url, abs, refresh_url)) refresh_url[0] = 0;
            }
        }
        return;
    }
    if (tag_is(t, T_SKIP) && !close) { *pos = skip_to_close(src, *pos, n, t, NULL); return; }
}

static void layout(int view_w) {
    static lay_t L;
    memset(&L, 0, sizeof L);
    L.view_w = view_w;
    L.cy = 14;
    L.cx = PAD;
    L.link = -1;
    L.form = -1;
    n_fields_prev = n_fields;
    n_runs = n_links = n_fields = n_forms = 0;
    pool_used = url_used = 0;
    page_title[0] = 0;
    refresh_url[0] = 0;

    const char* s = (const char*)resp;
    int n = resp_len;
    if (plain_text) {
        L.pre = 1;
        do_text(&L, s, n);
    } else {
        int i = 0;
        while (i < n) {
            if (s[i] != '<') {
                int j = i;
                while (j < n && s[j] != '<') j++;
                do_text(&L, s + i, j - i);
                i = j;
                continue;
            }
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                const char* e = find_ci(s + i + 4, n - i - 4, "-->");
                i = e ? (int)(e - s) + 3 : n;
                continue;
            }
            int j = i + 1;
            bool close = false;
            if (j < n && s[j] == '/') { close = true; j++; }
            if (j >= n || !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || s[j] == '!' || s[j] == '?')) {
                do_text(&L, s + i, 1);                /* a literal '<' */
                i++;
                continue;
            }
            char t[16];
            int tl = 0;
            while (j < n && s[j] != '>' && s[j] != ' ' && s[j] != '/' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r') {
                if (tl < 15) t[tl++] = lower(s[j]);
                j++;
            }
            t[tl] = 0;
            int a0 = j;
            char q = 0;
            while (j < n && (q || s[j] != '>')) {
                if (q) { if (s[j] == q) q = 0; }
                else if (s[j] == '"' || s[j] == '\'') q = s[j];
                j++;
            }
            attrs_t A = { s + a0, j - a0 };
            int pos = j < n ? j + 1 : n;
            if (t[0] != '!' && t[0] != '?') do_tag(&L, s, n, &pos, t, close, A);
            i = pos;
        }
    }
    end_line(&L);
    doc_h = L.cy + PAD;
    keep_fields = false;
}

/* ======================================================================
   Pages: fetch through curl, history, forms
   ====================================================================== */

#define DOC_MAX_W 940
static bool load_push, load_rerouted;

static const char HOME_HTML[] =
    "<title>Start</title><center><h1>SamaraOS Browser</h1>"
    "<p><small>HTTPS through curl + mbedTLS. Plain HTML only, no JavaScript.</small></p>"
    "<form action=\"https://html.duckduckgo.com/html/\"><input name=q size=36> "
    "<input type=submit value=\"Search\"></form>"
    "<p><a href=\"https://www.google.com/\">Google</a> &nbsp; "
    "<a href=\"https://ru.wikipedia.org/\">Wikipedia</a> &nbsp; "
    "<a href=\"https://news.ycombinator.com/\">Hacker News</a> &nbsp; "
    "<a href=\"https://lite.cnn.com/\">CNN Lite</a> &nbsp; "
    "<a href=\"https://text.npr.org/\">NPR Text</a></p></center>"
    "<hr><h3>Simple sites that work well</h3><ul>"
    "<li><a href=\"https://example.com/\">example.com</a>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">info.cern.ch</a> - the first web page"
    "<li><a href=\"https://en.wikipedia.org/wiki/Samara\">Samara on Wikipedia</a>"
    "<li><a href=\"https://wiby.me/\">wiby.me</a> - search engine for the old web"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a>"
    "<li><a href=\"http://host:8080/\">http://host:8080/</a> - a server on the host machine"
    "</ul><p><small>Type an address (google.com) or words to search in the bar above. "
    "Backspace = back, Tab = next field, PgUp/PgDn = scroll.</small></p>";

static int client_w(void) {
    int cx, cy, cw, ch;
    wm_client_rect(g_win, &cx, &cy, &cw, &ch);
    return cw;
}
static int layout_w(void) {
    int w = client_w() - 8;
    return w > DOC_MAX_W ? DOC_MAX_W : w;
}

static void set_edit(const char* s) {
    scpy(edit, URL_MAX, s);
    edit_len = (int)strlen(edit);
    edit_cur = edit_len;
}

static void focus_url(void) {
    focus = FOCUS_URL;
    set_edit(starts_ci(cur_url, HOME_URL) ? "" : cur_url);
    edit_all = edit_len > 0;
}

static void push_stack(char (*st)[URL_MAX], int* n, const char* u) {
    if (!u[0]) return;
    if (*n == HIST_MAX) { memmove(st[0], st[1], (size_t)(HIST_MAX - 1) * URL_MAX); (*n)--; }
    scpy(st[(*n)++], URL_MAX, u);
}

static void update_title(void) {
    if (!g_win) return;
    scpy(g_win->title, sizeof g_win->title, page_title[0] ? page_title : "Browser");
    if (page_title[0]) scat(g_win->title, sizeof g_win->title, " - Browser");
}

static void navigate(const char* url, const char* post, bool push);

/* resp[] holds the new page (CP866): lay it out and make it current. */
static void commit(const char* url, bool push) {
    if (push && strcmp(cur_url, url)) { push_stack(back_stack, &n_back, cur_url); n_fwd = 0; }
    scpy(cur_url, URL_MAX, url);
    scpy(base_url, URL_MAX, url);
    set_edit(starts_ci(url, HOME_URL) ? "" : url);
    scroll_y = 0;
    hover_link = hover_run = -1;
    focus = starts_ci(url, HOME_URL) ? FOCUS_URL : FOCUS_NONE;
    n_fields = 0;
    laid_w = layout_w();
    layout(laid_w);
    update_title();
    if (refresh_url[0] && auto_redirects < 4) {
        auto_redirects++;
        static char r[URL_MAX];
        scpy(r, URL_MAX, refresh_url);
        navigate(r, NULL, false);
    }
    if (g_win) g_win->needs_repaint = true;
}

static void show_html(const char* html, const char* url, bool push) {
    int n = (int)strlen(html);
    if (n > RESP_CAP - 1) n = RESP_CAP - 1;
    memcpy(resp, html, (size_t)n);
    resp_len = n;
    plain_text = false;
    commit(url, push);
}

static void show_error(const char* url, const char* title, const char* msg) {
    static char h[2048];
    scpy(h, sizeof h, "<title>");
    scat(h, sizeof h, title);
    scat(h, sizeof h, "</title><h2>");
    scat(h, sizeof h, title);
    scat(h, sizeof h, "</h2><p><code>");
    int p = (int)strlen(h);
    for (int i = 0; url[i] && p < 1400; i++) h[p++] = (url[i] == '<' || url[i] == '&') ? ' ' : url[i];
    h[p] = 0;
    scat(h, sizeof h, "</code></p><p>");
    p = (int)strlen(h);
    for (int i = 0; msg[i] && p < 1900; i++) h[p++] = (msg[i] == '<' || msg[i] == '&') ? ' ' : msg[i];
    h[p] = 0;
    scat(h, sizeof h, "</p><p><a href=\"about:home\">Start page</a></p>");
    show_html(h, url, load_push);
}

static void kill_fetch(void) {
    if (fetch_pid > 0 && proc_alive(fetch_pid)) {
        proc_t* p = proc_by_pid(fetch_pid);
        if (p) proc_send_signal(p, 9);
    }
    fetch_pid = -1;
    loading = false;
}

static void start_fetch(const char* url, const char* post) {
    kill_fetch();
    fs_node_t* root = fs_root();
    fs_unlink(root, PAGE_FILE);
    fs_unlink(root, META_FILE);
    fs_unlink(root, ERR_FILE);

    static char wire[URL_MAX * 3], data[4096];
    wire_url(url, wire, sizeof wire);
    char* argv[40];
    int a = 0;
    argv[a++] = (char*)"curl";
    argv[a++] = (char*)"-sS";
    argv[a++] = (char*)"-L";
    argv[a++] = (char*)"--max-redirs";   argv[a++] = (char*)"10";
    argv[a++] = (char*)"-m";             argv[a++] = (char*)"45";
    argv[a++] = (char*)"--connect-timeout"; argv[a++] = (char*)"15";
    argv[a++] = (char*)"-A";             argv[a++] = (char*)USER_AGENT;
    argv[a++] = (char*)"-H";             argv[a++] = (char*)"Accept-Language: ru,en;q=0.8";
    argv[a++] = (char*)"-H";             argv[a++] = (char*)"Accept: text/html,text/plain;q=0.9,*/*;q=0.5";
    argv[a++] = (char*)"-o";             argv[a++] = (char*)PAGE_FILE;
    argv[a++] = (char*)"--stderr";       argv[a++] = (char*)ERR_FILE;
    argv[a++] = (char*)"-w";
    argv[a++] = (char*)"%output{" META_FILE "}%{http_code}\\n%{content_type}\\n%{url_effective}\\n";
    if (post) {
        scpy(data, sizeof data, post);
        argv[a++] = (char*)"--data-raw";
        argv[a++] = data;
    }
    argv[a++] = wire;
    argv[a] = NULL;
    char* envp[] = { (char*)"PATH=/bin:/usr/bin", (char*)"HOME=/root",
                     (char*)"CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt", NULL };
    fetch_pid = proc_spawn_detached(CURL, argv, envp);
    if (fetch_pid < 0) {
        fetch_pid = -1;
        show_error(url, "Browser can't fetch pages", "/usr/bin/curl is missing (userland/build-curl.sh).");
        return;
    }
    loading = true;
    now_ms = load_start = pit_uptime_ms();
    scpy(load_url, URL_MAX, url);
    char host[128];
    url_host(url, host, sizeof host);
    set_status("Connecting to ", host);
}

static int read_file(const char* path, char* out, int cap) {
    fs_node_t* n = fs_resolve(fs_root(), path);
    if (!n || !n->data) return n ? 0 : -1;
    int len = (int)n->size < cap ? (int)n->size : cap;
    memcpy(out, n->data, (size_t)len);
    return len;
}

static void finish_fetch(void) {
    loading = false;
    fetch_pid = -1;
    static char meta[URL_MAX * 3 + 256];
    int ml = read_file(META_FILE, meta, sizeof meta - 1);
    if (ml < 0) ml = 0;
    meta[ml] = 0;
    char* line[3] = { meta, NULL, NULL };
    for (int i = 0, k = 1; i < ml && k < 3; i++)
        if (meta[i] == '\n') { meta[i] = 0; line[k++] = meta + i + 1; }
    for (char* p = line[2] ? line[2] : meta; *p; p++) if (*p == '\n') *p = 0;
    int code = atoi(meta);
    const char* ctype = line[1] ? line[1] : "";
    const char* eff = (line[2] && line[2][0]) ? line[2] : load_url;

    if (code == 0) {
        static char err[512];
        int el = read_file(ERR_FILE, err, sizeof err - 1);
        err[el > 0 ? el : 0] = 0;
        for (char* p = err; *p; p++) if (*p == '\n') *p = ' ';
        char* msg = strstr(err, ") ");
        show_error(load_url, "Can't open this page", msg ? msg + 2 : (err[0] ? err : "no answer (network down?)"));
        set_status("Failed: ", load_url);
        return;
    }
    int n = read_file(PAGE_FILE, (char*)resp, RESP_CAP - 1);
    if (n < 0) n = 0;
    fs_node_t* root = fs_root();
    fs_unlink(root, PAGE_FILE);
    fs_unlink(root, META_FILE);
    fs_unlink(root, ERR_FILE);

    static char eff_u[URL_MAX];
    scpy(eff_u, URL_MAX, eff);
    bool html = !ctype[0] || find_ci(ctype, (int)strlen(ctype), "html") || find_ci(ctype, (int)strlen(ctype), "xml");
    bool text = find_ci(ctype, (int)strlen(ctype), "text/") || find_ci(ctype, (int)strlen(ctype), "json");
    if (!html && !text) {
        static char m[200];
        scpy(m, sizeof m, "This is not a web page: ");
        scat(m, sizeof m, ctype);
        char num[16];
        itoa(n, num, 10);
        scat(m, sizeof m, ", ");
        scat(m, sizeof m, num);
        scat(m, sizeof m, " bytes. Use wget in the terminal to save it.");
        show_error(eff_u, "File", m);
        return;
    }
    const char* cs = find_ci(ctype, (int)strlen(ctype), "charset=");
    bool w1251 = cs && (find_ci(cs, (int)strlen(cs), "1251") != NULL);
    if (!cs) {                                   /* <meta charset> in the head */
        int k = n < 4096 ? n : 4096;
        const char* mc = find_ci((const char*)resp, k, "charset=");
        if (mc && find_ci(mc, 24, "1251")) w1251 = true;
    }
    resp_len = w1251 ? cp1251_to_cp866(resp, n) : utf8_to_cp866(resp, n);
    plain_text = !html;
    commit(eff_u, load_push);
    int32_t ms = (int32_t)(pit_uptime_ms() - load_start);
    if (ms < 0) ms = 0;
    static char st[160];
    char num[16];
    scpy(st, sizeof st, code >= 400 ? "HTTP error " : "Done  ");
    if (code >= 400) { itoa(code, num, 10); scat(st, sizeof st, num); scat(st, sizeof st, "  "); }
    itoa((n + 1023) / 1024, num, 10); scat(st, sizeof st, num); scat(st, sizeof st, " KB in ");
    itoa((int)(ms / 1000), num, 10); scat(st, sizeof st, num); scat(st, sizeof st, ".");
    itoa((int)(ms % 1000) / 100, num, 10); scat(st, sizeof st, num); scat(st, sizeof st, " s");
    if (load_rerouted) scat(st, sizeof st, "   (Google search needs JavaScript - results by DuckDuckGo)");
    if (n >= RESP_CAP - 1) scat(st, sizeof st, "   (page truncated)");
    set_status(st, NULL);
}

static void navigate(const char* in, const char* post, bool push) {
    static char u[URL_MAX];
    scpy(u, URL_MAX, in);
    load_rerouted = reroute_search(u);
    load_push = push;
    if (starts_ci(u, "about:")) {
        kill_fetch();
        show_html(HOME_HTML, HOME_URL, push);
        set_status("Start page", NULL);
        return;
    }
    set_edit(u);
    focus = FOCUS_NONE;
    start_fetch(u, post);
    if (g_win) g_win->needs_repaint = true;
}

static void go(const char* url, const char* post) {
    auto_redirects = 0;
    navigate(url, post, true);
}

static void go_back(void) {
    if (!n_back) return;
    push_stack(fwd_stack, &n_fwd, cur_url);
    static char u[URL_MAX];
    scpy(u, URL_MAX, back_stack[--n_back]);
    auto_redirects = 4;
    navigate(u, NULL, false);
}
static void go_forward(void) {
    if (!n_fwd) return;
    push_stack(back_stack, &n_back, cur_url);
    static char u[URL_MAX];
    scpy(u, URL_MAX, fwd_stack[--n_fwd]);
    auto_redirects = 0;
    navigate(u, NULL, false);
}
static void reload(void) {
    if (loading) { kill_fetch(); set_status("Stopped", NULL); return; }
    auto_redirects = 0;
    navigate(cur_url[0] ? cur_url : HOME_URL, NULL, false);
}

static void submit_form(int form, int submitter) {
    if (form < 0 || form >= n_forms || forms[form].action < 0) return;
    static char q[4096];
    int p = 0;
    q[0] = 0;
    for (int i = 0; i < n_fields; i++) {
        field_t* f = &fields[i];
        if (f->form != form || !f->name[0]) continue;
        if (f->type == FT_SUBMIT && i != submitter) continue;
        if (p && p < (int)sizeof q - 2) q[p++] = '&';
        form_encode(f->name, (int)strlen(f->name), q, &p, sizeof q);
        if (p < (int)sizeof q - 2) q[p++] = '=';
        form_encode(f->value, f->len, q, &p, sizeof q);
    }
    q[p] = 0;
    static char u[URL_MAX];
    scpy(u, URL_MAX, url_pool + forms[form].action);
    if (forms[form].post) { go(u, q); return; }
    char* qm = strchr(u, '?');
    if (qm) *qm = 0;
    scat(u, URL_MAX, "?");
    scat(u, URL_MAX, q);
    go(u, NULL);
}

/* ======================================================================
   Drawing
   ====================================================================== */

#define COL_UNDERLINE RGB(0xB4, 0xC6, 0xE4)
#define COL_BTN_HOT   RGB(0xEE, 0xB0, 0x55)

static int clip_x, clip_y, clip_w, clip_h;       /* the WM's clip for this paint */

static void clip_to(int x, int y, int w, int h) {
    int x0 = x > clip_x ? x : clip_x, y0 = y > clip_y ? y : clip_y;
    int x1 = (x + w < clip_x + clip_w) ? x + w : clip_x + clip_w;
    int y1 = (y + h < clip_y + clip_h) ? y + h : clip_y + clip_h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    gfx_set_clip(x0, y0, x1 - x0, y1 - y0);
}
static void clip_restore(void) { gfx_set_clip(clip_x, clip_y, clip_w, clip_h); }

static void thick_line(int x0, int y0, int x1, int y1, uint32_t c) {
    gfx_line(x0, y0, x1, y1, c);
    gfx_line(x0 + 1, y0, x1 + 1, y1, c);
    gfx_line(x0, y0 + 1, x1, y1 + 1, c);
}

static void draw_icon(int i, box_t b, bool enabled) {
    bool hov = enabled && hover_btn == i;
    uint32_t bg = hov ? C_BTN_HOVER : C_SURFACE;
    if (hov) gfx_rrect_fill(b.x, b.y, b.w, b.h, 7, GFX_CORNERS_ALL, bg);
    uint32_t c = !enabled ? C_GLYPH_DIM : hov ? C_INK : C_GLYPH;
    int mx = b.x + b.w / 2, my = b.y + b.h / 2;
    switch (i) {
    case B_BACK:
    case B_FWD: {
        int d = (i == B_BACK) ? 1 : -1;
        thick_line(mx - 7 * d, my, mx + 7 * d, my, c);
        thick_line(mx - 7 * d, my, mx - 2 * d, my - 5, c);
        thick_line(mx - 7 * d, my, mx - 2 * d, my + 5, c);
        break;
    }
    case B_RELOAD:
        if (loading) {
            thick_line(mx - 6, my - 6, mx + 6, my + 6, c);
            thick_line(mx - 6, my + 6, mx + 6, my - 6, c);
        } else {
            gfx_circle(mx, my, 7, c);
            gfx_circle(mx, my, 6, c);
            gfx_rect_fill(mx + 1, my - 8, 8, 7, bg);            /* gap at the top right */
            for (int k = 0; k < 5; k++) gfx_rect_fill(mx + 1 + k, my - 8 + k, 6 - k * 1, 1, c);
            gfx_rect_fill(mx + 1, my - 8, 1, 5, c);
        }
        break;
    case B_HOME:
        thick_line(mx - 8, my, mx, my - 8, c);
        thick_line(mx, my - 8, mx + 8, my, c);
        gfx_rect_fill(mx - 5, my - 1, 2, 9, c);
        gfx_rect_fill(mx + 4, my - 1, 2, 9, c);
        gfx_rect_fill(mx - 5, my + 7, 11, 2, c);
        gfx_rect_fill(mx - 1, my + 2, 3, 6, c);
        break;
    }
}

/* Rounded text box; caret at `cur` when focused. Text scrolls to keep it visible. */
static void draw_textbox(int x, int y, int w, int h, const char* s, int len, int cur,
                         bool foc, const char* placeholder) {
    gfx_rrect_fill(x, y, w, h, 8, GFX_CORNERS_ALL, foc ? C_ACCENT : C_RULE);
    int t = foc ? 2 : 1;
    gfx_rrect_fill(x + t, y + t, w - 2 * t, h - 2 * t, 8 - t, GFX_CORNERS_ALL, C_WHITE);
    int tx = x + 12, avail = w - 24;
    int cw = uif_width_n(UIF_REG, s, cur);
    int off = cw > avail ? cw - avail : 0;
    clip_to(x + 4, y + 2, w - 8, h - 4);
    if (!len && placeholder) uif_draw_mid(tx, y + h / 2, UIF_REG, placeholder, C_INK_DIM);
    if (foc && s == edit && edit_all)
        gfx_rrect_fill(tx - 2 - off, y + 6, uif_width_n(UIF_REG, s, len) + 4, h - 12, 3, GFX_CORNERS_ALL, RGB(0xF6, 0xDC, 0xB0));
    static char tmp[URL_MAX];
    int n = len < URL_MAX - 1 ? len : URL_MAX - 1;
    memcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    uif_draw_mid(tx - off, y + h / 2, UIF_REG, tmp, C_INK);
    if (foc && !(s == edit && edit_all) && (now_ms / 530) % 2 == 0) gfx_rect_fill(tx - off + cw, y + 7, 2, h - 14, C_ACCENT);
    clip_restore();
}

/* Address bar while not editing: host dark, the rest dimmed. */
static void draw_url_view(int x, int y, int w, int h) {
    gfx_rrect_fill(x, y, w, h, 8, GFX_CORNERS_ALL, C_RULE);
    gfx_rrect_fill(x + 1, y + 1, w - 2, h - 2, 7, GFX_CORNERS_ALL, C_WHITE);
    clip_to(x + 4, y + 2, w - 8, h - 4);
    int tx = x + 12, mid = y + h / 2;
    const char* u = edit;
    if (!u[0]) {
        uif_draw_mid(tx, mid, UIF_REG, "Search or type an address", C_INK_DIM);
    } else {
        int se, he;
        if (url_parts(u, &se, &he)) {
            if (!strncmp(u, "https", 5)) {                     /* small padlock */
                gfx_rect(tx + 2, mid - 6, 6, 6, C_ONLINE);
                gfx_rrect_fill(tx, mid - 1, 10, 8, 2, GFX_CORNERS_ALL, C_ONLINE);
                tx += 16;
            }
            static char part[URL_MAX];
            int hl = he - se - 3;
            memcpy(part, u + se + 3, (size_t)hl);
            part[hl] = 0;
            tx = uif_draw_mid(tx, mid, UIF_REG, part, C_INK);
            uif_draw_mid(tx, mid, UIF_REG, u + he, C_INK_DIM);
        } else {
            uif_draw_mid(tx, mid, UIF_REG, u, C_INK);
        }
    }
    clip_restore();
}

static int doc_ox(void) { int d = (view_box.w - 8 - laid_w) / 2; return view_box.x + (d > 0 ? d : 0); }
static int doc_oy(void) { return view_box.y - scroll_y; }

static void clamp_scroll(void) {
    int mx = doc_h - view_box.h;
    if (mx < 0) mx = 0;
    if (scroll_y > mx) scroll_y = mx;
    if (scroll_y < 0) scroll_y = 0;
}

static void draw_run(int idx, int ox, int oy) {
    run_t* r = &runs[idx];
    int x = ox + r->x, y = oy + r->y;
    switch (r->kind) {
    case RK_TEXT: {
        bool hov = r->link >= 0 && r->link == hover_link;
        uint32_t c = hov ? COL_LINK_HOV : r->fg;
        const char* s = pool + r->off;
        int asc;
        if (r->font == F_MONO) {
            for (int i = 0; i < r->len; i++) uif_mono_char(x + i * 8, y, (uint8_t)s[i], c, 0, false);
            asc = 13;
        } else {
            uif_draw(x, y, (uif_t)r->font, s, c);
            asc = uif_face((uif_t)r->font)->ascent;
        }
        if (r->link >= 0) gfx_rect_fill(x, y + asc + 2, r->w, 1, hov ? COL_LINK_HOV : COL_UNDERLINE);
        break;
    }
    case RK_RULE:
        gfx_rect_fill(x, y, r->w, 1, C_RULE);
        break;
    case RK_BULLET:
        if (r->fg == C_GLYPH) gfx_disc(x + 3, y + r->h / 2 + 1, 3, r->fg);
        else gfx_circle(x + 3, y + r->h / 2 + 1, 3, C_GLYPH);
        break;
    case RK_FIELD: {
        field_t* f = &fields[r->field];
        static char stars[FIELD_MAX];
        const char* s = f->value;
        if (f->type == FT_PASSWORD) {
            for (int i = 0; i < f->len; i++) stars[i] = '*';
            stars[f->len] = 0;
            s = stars;
        }
        draw_textbox(x, y, r->w, r->h, s, f->len, f->cur, focus == r->field, NULL);
        clip_to(view_box.x, view_box.y, view_box.w, view_box.h);
        break;
    }
    case RK_BUTTON: {
        bool hov = hover_run == idx;
        gfx_rrect_fill(x, y, r->w, r->h, 8, GFX_CORNERS_ALL, hov ? COL_BTN_HOT : C_ACCENT);
        uif_draw_center(x, y, r->w, r->h, UIF_MED, pool + r->off, C_INK);
        break;
    }
    }
}

static void br_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_get_clip(&clip_x, &clip_y, &clip_w, &clip_h);
    view_box = (box_t){ cx, cy + TB_H, cw, ch - TB_H - SB_H };
    if (resp_len && layout_w() != laid_w) {
        laid_w = layout_w();
        keep_fields = true;
        layout(laid_w);
    }

    /* toolbar */
    gfx_rect_fill(cx, cy, cw, TB_H, C_SURFACE);
    gfx_rect_fill(cx, cy + TB_H - 1, cw, 1, C_RULE);
    int bx = cx + 10, by = cy + (TB_H - BTN) / 2;
    bool en[B_COUNT] = { n_back > 0, n_fwd > 0, true, true };
    for (int i = 0; i < B_COUNT; i++) {
        btn_box[i] = (box_t){ bx, by, BTN, BTN };
        draw_icon(i, btn_box[i], en[i]);
        bx += BTN + 2;
    }
    int ux = bx + 8, uw = cx + cw - 12 - ux;
    url_box = (box_t){ ux, by, uw, BTN };
    if (focus == FOCUS_URL) draw_textbox(ux, by, uw, BTN, edit, edit_len, edit_cur, true, "Search or type an address");
    else draw_url_view(ux, by, uw, BTN);
    if (loading) {
        int seg = cw / 4, span = cw + seg;
        int el = (int32_t)(now_ms - load_start);
        int p = (el < 0 ? 0 : el) / 2 % span - seg;
        int x0 = p < 0 ? 0 : p, x1 = p + seg > cw ? cw : p + seg;
        if (x1 > x0) gfx_rect_fill(cx + x0, cy + TB_H - 2, x1 - x0, 2, C_ACCENT);
    }

    /* page */
    gfx_rect_fill(view_box.x, view_box.y, view_box.w, view_box.h, COL_PAPER);
    clamp_scroll();
    clip_to(view_box.x, view_box.y, view_box.w, view_box.h);
    int ox = doc_ox(), oy = doc_oy();
    for (int i = 0; i < n_runs; i++) {
        run_t* r = &runs[i];
        if (r->y + r->h < scroll_y || r->y > scroll_y + view_box.h) continue;
        draw_run(i, ox, oy);
    }
    if (doc_h > view_box.h) {
        int th = view_box.h * view_box.h / doc_h;
        if (th < 30) th = 30;
        int ty = view_box.y + (view_box.h - th) * scroll_y / (doc_h - view_box.h);
        gfx_rrect_fill(cx + cw - 9, ty + 3, 5, th - 6, 2, GFX_CORNERS_ALL, C_GLYPH_DIM);
    }
    clip_restore();

    /* status bar */
    int sy = cy + ch - SB_H;
    gfx_rect_fill(cx, sy, cw, SB_H, C_SURFACE);
    gfx_rect_fill(cx, sy, cw, 1, C_RULE);
    int mid = sy + SB_H / 2 + 1;
    if (hover_link >= 0 && hover_link < n_links) {
        uif_draw_fit(cx + 12, uif_top_for_mid(UIF_SMALL, mid), UIF_SMALL, url_pool + link_off[hover_link], cw - 24, COL_LINK);
    } else {
        if (loading) gfx_disc(cx + 16, mid, 3, (now_ms / 300) % 2 ? C_ACCENT : C_GLYPH_DIM);
        uif_draw_fit(cx + (loading ? 26 : 12), uif_top_for_mid(UIF_SMALL, mid), UIF_SMALL, status, cw - 40, C_INK_DIM);
    }
}

/* ======================================================================
   Input
   ====================================================================== */

static int hit_run(int sx, int sy) {
    if (!in_box(view_box, sx, sy)) return -1;
    int dx = sx - doc_ox(), dy = sy - doc_oy();
    for (int i = 0; i < n_runs; i++) {
        run_t* r = &runs[i];
        if (r->link < 0 && r->kind != RK_FIELD && r->kind != RK_BUTTON) continue;
        if (dx >= r->x && dx < r->x + r->w && dy >= r->y - 1 && dy < r->y + r->h + 1) return i;
    }
    return -1;
}

static void update_hover(window_t* w) {
    static int lmx = -1, lmy = -1, lscroll = -1, lruns = -1;
    int mx, my; uint8_t btn;
    mouse_get(&mx, &my, &btn);
    if (mx == lmx && my == lmy && scroll_y == lscroll && n_runs == lruns) return;
    lmx = mx; lmy = my; lscroll = scroll_y; lruns = n_runs;
    int hb = -1;
    for (int i = 0; i < B_COUNT; i++) if (in_box(btn_box[i], mx, my)) hb = i;
    int hr = hit_run(mx, my);
    int hl = hr >= 0 ? runs[hr].link : -1;
    if (hb != hover_btn || hr != hover_run || hl != hover_link) {
        hover_btn = hb; hover_run = hr; hover_link = hl;
        w->needs_repaint = true;
    }
}

static void br_tick(window_t* w, uint32_t now) {
    uint32_t prev = now_ms;
    now_ms = now;
    if (loading) {
        if (fetch_pid < 0 || !proc_alive(fetch_pid)) finish_fetch();
        else if ((int32_t)(now - load_start) > 60000) {
            set_status("Timed out", NULL);
            kill_fetch();
            show_error(load_url, "The site took too long", "No complete answer in 60 seconds.");
        }
        w->needs_repaint = true;
    }
    if (focus != FOCUS_NONE && (now / 530) != (prev / 530)) w->needs_repaint = true;
    update_hover(w);
}

static void br_scroll(window_t* w, int dz) {
    scroll_y += dz * 54;
    clamp_scroll();
    w->needs_repaint = true;
}

static void br_release(window_t* w) { (void)w; click_consumed = false; }

static int caret_from_x(const char* s, int len, int x0, int x) {
    int best = 0, bd = 1 << 30;
    for (int k = 0; k <= len; k++) {
        int d = x0 + uif_width_n(UIF_REG, s, k) - x;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

static void br_click(window_t* w, int rx, int ry) {
    if (click_consumed) return;
    click_consumed = true;
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    int sx = rx + cx, sy = ry + cy;
    w->needs_repaint = true;
    if (in_box(btn_box[B_BACK], sx, sy))   { go_back(); return; }
    if (in_box(btn_box[B_FWD], sx, sy))    { go_forward(); return; }
    if (in_box(btn_box[B_RELOAD], sx, sy)) { reload(); return; }
    if (in_box(btn_box[B_HOME], sx, sy))   { go(HOME_URL, NULL); return; }
    if (in_box(url_box, sx, sy)) {
        if (focus != FOCUS_URL) focus_url();
        else {
            edit_all = false;
            int cwid = uif_width_n(UIF_REG, edit, edit_cur), avail = url_box.w - 24;
            int off = cwid > avail ? cwid - avail : 0;
            edit_cur = caret_from_x(edit, edit_len, url_box.x + 12 - off, sx);
        }
        return;
    }
    int r = hit_run(sx, sy);
    if (r < 0) { focus = FOCUS_NONE; return; }
    run_t* rr = &runs[r];
    if (rr->kind == RK_FIELD) {
        field_t* f = &fields[rr->field];
        focus = rr->field;
        if (f->type != FT_PASSWORD) f->cur = caret_from_x(f->value, f->len, doc_ox() + rr->x + 12, sx);
        else f->cur = f->len;
        return;
    }
    if (rr->kind == RK_BUTTON) { submit_form(fields[rr->field].form, rr->field); return; }
    if (rr->link >= 0) go(url_pool + link_off[rr->link], NULL);
}

/* Line editing shared by the address bar and form fields. */
static bool edit_key(char* s, int* len, int* cur, int cap, uint8_t c, bool text_mode) {
    if (c == '\b') {
        if (*cur > 0) { memmove(s + *cur - 1, s + *cur, (size_t)(*len - *cur + 1)); (*len)--; (*cur)--; }
        return true;
    }
    if (!text_mode) {
        if (c == K_DEL)   { if (*cur < *len) { memmove(s + *cur, s + *cur + 1, (size_t)(*len - *cur)); (*len)--; } return true; }
        if (c == K_LEFT)  { if (*cur > 0) (*cur)--; return true; }
        if (c == K_RIGHT) { if (*cur < *len) (*cur)++; return true; }
        if (c == K_HOME)  { *cur = 0; return true; }
        if (c == K_END)   { *cur = *len; return true; }
        if (c >= 0x80 && c < 0xA0) return false;             /* other special keys */
    }
    if (c >= 0x20 && c != 0x7F && *len < cap - 1) {
        memmove(s + *cur + 1, s + *cur, (size_t)(*len - *cur + 1));
        s[(*cur)++] = (char)c;
        (*len)++;
        return true;
    }
    return false;
}

static void focus_next_field(void) {
    int start = focus >= 0 ? focus + 1 : 0;
    for (int k = 0; k < n_fields; k++) {
        int i = (start + k) % n_fields;
        if (fields[i].type == FT_TEXT || fields[i].type == FT_PASSWORD) {
            focus = i;
            fields[i].cur = fields[i].len;
            for (int r = 0; r < n_runs; r++)                  /* scroll it into view */
                if (runs[r].kind == RK_FIELD && runs[r].field == i) {
                    if (runs[r].y < scroll_y || runs[r].y + runs[r].h > scroll_y + view_box.h)
                        scroll_y = runs[r].y - view_box.h / 3;
                    clamp_scroll();
                }
            return;
        }
    }
    focus = FOCUS_URL;
}

static void br_key(window_t* w, char ch) {
    uint8_t c = (uint8_t)ch;
    w->needs_repaint = true;
    /* In the RU layout, 0x80.. are Cyrillic letters, not arrow keys. */
    bool typing = focus != FOCUS_NONE;
    bool ru_text = typing && kbd_is_ru() && ((c >= 0x80 && c <= 0xAF) || (c >= 0xE0 && c <= 0xF1));
    if (c == 0x1B) {
        if (loading) { kill_fetch(); set_status("Stopped", NULL); return; }
        if (typing) { focus = FOCUS_NONE; set_edit(starts_ci(cur_url, HOME_URL) ? "" : cur_url); return; }
        wm_close(w);
        return;
    }
    if (c == '\t') { focus_next_field(); return; }
    if (!ru_text) {
        int page = view_box.h - 60;
        if (c == K_PGUP) { scroll_y -= page; clamp_scroll(); return; }
        if (c == K_PGDN) { scroll_y += page; clamp_scroll(); return; }
        if (c == K_F1 + 4) { reload(); return; }             /* F5 */
    }
    if (focus == FOCUS_URL) {
        if (c == '\n' || c == '\r') {
            static char u[URL_MAX];
            edit[edit_len] = 0;
            normalize_typed(edit, u);
            go(u, NULL);
            return;
        }
        if (edit_all) {
            edit_all = false;
            if (c == '\b' || c == K_DEL || c >= 0x20) { edit_len = edit_cur = 0; edit[0] = 0; }
            if (c == '\b' || c == K_DEL) return;
        }
        edit_key(edit, &edit_len, &edit_cur, URL_MAX, c, ru_text);
        return;
    }
    if (focus >= 0 && focus < n_fields) {
        field_t* f = &fields[focus];
        if (c == '\n' || c == '\r') { submit_form(f->form, -1); return; }
        edit_key(f->value, &f->len, &f->cur, FIELD_MAX, c, ru_text);
        return;
    }
    /* page focused: navigation keys */
    if (c == K_UP)   { scroll_y -= 40; clamp_scroll(); return; }
    if (c == K_DOWN) { scroll_y += 40; clamp_scroll(); return; }
    if (c == ' ')    { scroll_y += view_box.h - 60; clamp_scroll(); return; }
    if (c == K_HOME) { scroll_y = 0; return; }
    if (c == K_END)  { scroll_y = doc_h; clamp_scroll(); return; }
    if (c == '\b' || c == K_LEFT) { go_back(); return; }
    if (c == K_RIGHT) { go_forward(); return; }
    if (c == '/' || c == 'l' || c == 'L') { focus_url(); return; }
    if (c == 'r' || c == 'R') { reload(); return; }
}

static void br_close(window_t* w) {
    (void)w;
    kill_fetch();
    g_win = NULL;
}

/* ======================================================================
   Entry
   ====================================================================== */

int browser_open(const char* url) {
    if (!gfx_ready()) return -1;
    if (g_win && g_win->open) {
        if (url && *url) { static char u[URL_MAX]; normalize_typed(url, u); go(u, NULL); }
        return 0;
    }
    if (!resp) {
        resp     = kmalloc_big(RESP_CAP);
        pool     = kmalloc_big(POOL_CAP);
        url_pool = kmalloc_big(URLPOOL_CAP);
        runs     = kmalloc_big(sizeof(run_t) * MAX_RUNS);
        if (!resp || !pool || !url_pool || !runs) {
            if (resp) kfree(resp);
            if (pool) kfree(pool);
            if (url_pool) kfree(url_pool);
            if (runs) kfree(runs);
            resp = NULL; pool = NULL; url_pool = NULL; runs = NULL;
            return -1;
        }
    }
    cur_url[0] = base_url[0] = 0;
    n_back = n_fwd = 0;
    resp_len = n_runs = n_links = n_fields = n_forms = 0;
    doc_h = scroll_y = 0;
    hover_link = hover_btn = hover_run = -1;
    click_consumed = false;
    fetch_pid = -1;
    loading = false;
    set_edit("");

    int W = gfx_w(), H = gfx_h();
    int ww = 1100, wh = 780;
    if (ww > W - 80) ww = W - 80;
    if (wh > H - 120) wh = H - 120;
    int wx = (W - ww) / 2, wy = (H - wh) / 2 - 24;
    if (wy < 30) wy = 30;

    g_win = wm_open_app_ex(wx, wy, ww, wh, "Browser", br_paint, br_key, br_click, br_tick, true, NULL);
    if (!g_win) return -1;
    g_win->on_release = br_release;
    g_win->on_close   = br_close;
    g_win->on_scroll  = br_scroll;
    g_win->opaque     = true;
    g_win->min_w = 520;
    g_win->min_h = 300;

    wm_client_rect(g_win, &view_box.x, &view_box.y, &view_box.w, &view_box.h);
    view_box.y += TB_H;
    view_box.h -= TB_H + SB_H;
    auto_redirects = 0;
    load_push = false;
    show_html(HOME_HTML, HOME_URL, false);
    set_status("Start page", NULL);
    if (url && *url) { static char u[URL_MAX]; normalize_typed(url, u); go(u, NULL); }
    return 0;
}
