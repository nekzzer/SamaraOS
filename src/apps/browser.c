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
 * CSS: a small subset (colors, backgrounds, display:none, weight, size,
 * alignment, margins) from <style> and style="". JavaScript: pages with
 * <script> go through /usr/bin/domjs (quickjs + tiny DOM) once after the
 * download, the browser lays out what comes back. Timers run on a fake
 * clock, so no animations or click handlers. Google search still goes to
 * DuckDuckGo's HTML version. */

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
#include "apps/imgdec.h"

/* ---------- tunables ---------- */
#define RESP_CAP     (4 * 1024 * 1024)   /* page bytes kept */
#define POOL_CAP     (2 * 1024 * 1024)       /* laid-out text */
#define URLPOOL_CAP  (512 * 1024)        /* link targets */
#define MAX_RUNS     40000
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
#define JS_OUT       "/tmp/.browser-js"
#define JS_SEQ       "/tmp/.browser-js-seq"   /* live page: bumped on every new render */
#define JS_NAV       "/tmp/.browser-nav"
#define JS_EV        "/tmp/.browser-ev"
#define DOMJS        "/usr/bin/domjs"
#define CURL         "/usr/bin/curl"
#define USER_AGENT   "Dillo/3.0.5"        /* gets Google's classic no-JS page */
#define SEARCH_URL   "https://html.duckduckgo.com/html/?q="
#define HOME_URL     "about:home"

/* ---------- colours (theme family) ---------- */
#define COL_PAPER    RGB(0xFB, 0xFA, 0xF7)
// page defaults are web colours, not the (dark) desktop theme: dark ink on paper
#define PAGE_INK     RGB(0x1D, 0x1E, 0x21)
#define PAGE_DIM     RGB(0x8C, 0x88, 0x80)
#define PAGE_RULE    RGB(0xD4, 0xCF, 0xC6)
#define COL_TEXT     PAGE_INK
#define COL_LINK     RGB(0x1F, 0x5C, 0xB8)
#define COL_LINK_HOV RGB(0xC2, 0x6A, 0x12)

/* ---------- document model ---------- */
enum { RK_TEXT, RK_RULE, RK_BULLET, RK_FIELD, RK_BUTTON, RK_IMAGE };
enum { FT_TEXT, FT_PASSWORD, FT_HIDDEN, FT_SUBMIT };

typedef struct {
    int      x, y;           /* document pixels */
    uint16_t w, h;
    int      off;            /* text in pool */
    uint16_t len;
    uint8_t  font, kind, scale, flags;
    int8_t   ls;
    int16_t  clip;
    uint32_t fg;
    int16_t  link, field;
    int      js;             /* data-sjs of the element, for live pages */
} run_t;

typedef struct {
    int     form, js;
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
static uint32_t page_bg;
static bool     page_bg_set;

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

/* images of the page: fetched one at a time once the page is up */
#define MAX_IMGS 64
#define IMG_FILE "/tmp/.browser-img"
typedef struct { char* url; uint8_t st; int w, h; uint32_t* px; uint32_t* sc; int sw, sh; uint32_t sbg; } img_t;
enum { IM_WAIT, IM_LOAD, IM_OK, IM_BAD };
static img_t    imgs[MAX_IMGS];
static int      n_imgs, img_pid = -1, img_cur = -1;
static uint32_t img_relayout_at;
static int      js_pid = -1;
static int      js_live = -1;          /* domjs still running the page: clicks go to it */
static int      js_seq;
static bool     js_pending;
static int      js_code;
static char     js_eff[URL_MAX], js_ctype[80];
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

/* The page is parsed into a small DOM first, then every element gets a
   computed style (cascade over <style> sheets + style="" + a UA sheet) and
   the layout walks the tree. Boxes are recorded for backgrounds/borders,
   text goes to runs as before. No floats in the kernel, everything is ints. */

static int font_h(int f) { return f == F_MONO ? 16 : uif_height((uif_t)f); }
static int font_asc(int f) { return f == F_MONO ? 13 : uif_face((uif_t)f)->ascent; }
static int text_w(int f, const char* s, int n) { return f == F_MONO ? n * 8 : uif_width_n((uif_t)f, s, n); }

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
static int ga_v0, ga_v1;              /* raw value of the last get_attr hit */

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
            ga_v0 = v0; ga_v1 = v1;
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

/* ---------- images ---------- */
static void img_reset(void) {
    for (int i = 0; i < n_imgs; i++) {
        if (imgs[i].url) kfree(imgs[i].url);
        if (imgs[i].px) kfree(imgs[i].px);
        if (imgs[i].sc) kfree(imgs[i].sc);
    }
    memset(imgs, 0, sizeof imgs);
    n_imgs = 0;
    img_cur = -1;
}

/* absolute url -> image slot (new ones wait for the fetcher) */
static int img_find(const char* u) {
    if (!starts_ci(u, "http")) return -1;
    for (int i = 0; i < n_imgs; i++) if (!strcmp(imgs[i].url, u)) return i;
    if (n_imgs >= MAX_IMGS) return -1;
    int n = (int)strlen(u);
    char* c = kmalloc((uint32_t)n + 1);
    if (!c) return -1;
    memcpy(c, u, (size_t)n + 1);
    img_t* im = &imgs[n_imgs];
    memset(im, 0, sizeof *im);
    im->url = c;
    return n_imgs++;
}

/* url(...) in css -> slot */
static int img_css(const char* p) {
    static char u[URL_MAX], abs_u[URL_MAX];
    p += 4;
    while (*p == ' ') p++;
    char q = (*p == '"' || *p == '\'') ? *p++ : 0;
    int k = 0;
    while (*p && k < URL_MAX - 1 && (q ? *p != q : (*p != ')' && *p != ' '))) u[k++] = *p++;
    u[k] = 0;
    if (!k || starts_ci(u, "data:") || !resolve_url(base_url, u, abs_u)) return -1;
    return img_find(abs_u);
}

/* ---------- dom ---------- */
#define MAX_NODES 60000
#define MAX_CLS 40000

typedef struct {
    int parent, first, last, next, prev;
    int a, an;                 /* attrs of an element / text of a text node, in resp */
    uint32_t th, idh;
    int cls, idx;
    uint8_t ncls, text;
    char tag[10];
    int mmax, mmin;            /* measure cache, -1 = not yet */
} node_t;

static node_t*   nodes;
static int       n_nodes;
static uint32_t* clsh;
static int       n_clsh;

static uint32_t hsh(const char* s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

static const char* const T_VOID[] = { "br", "hr", "img", "input", "meta", "link", "base", "area", "col",
    "embed", "param", "source", "track", "wbr", 0 };
static const char* const T_RAW[] = { "script", "style", "textarea", "title", "svg", "math", "template",
    "select", "noembed", "xmp", "iframe", 0 };
static const char* const T_CLOSEP[] = { "div", "ul", "ol", "table", "h1", "h2", "h3", "h4", "h5", "h6",
    "pre", "form", "p", "blockquote", "section", "article", "header", "footer", "nav", "dl", "hr", "main",
    "aside", "figure", "address", "fieldset", "details", "menu", 0 };
static const char* const T_INL[] = { "a", "span", "b", "i", "em", "strong", "font", "small", "code", "u", "s", 0 };

static int new_node(int parent, int text) {
    if (n_nodes >= MAX_NODES) return -1;
    int k = n_nodes++;
    node_t* N = &nodes[k];
    memset(N, 0, sizeof *N);
    N->parent = parent;
    N->first = N->last = N->next = N->prev = -1;
    N->text = (uint8_t)text;
    N->mmax = -1;
    if (parent >= 0) {
        node_t* P = &nodes[parent];
        if (P->last >= 0) { nodes[P->last].next = k; N->prev = P->last; } else P->first = k;
        P->last = k;
        if (!text) {
            int p = N->prev;
            while (p >= 0 && nodes[p].text) p = nodes[p].prev;
            N->idx = p >= 0 ? nodes[p].idx + 1 : 0;
        }
    }
    return k;
}

static bool is_tag(int n, const char* t) { return n >= 0 && !nodes[n].text && !strcmp(nodes[n].tag, t); }

/* pops the stack up to (and including) the nearest `want`, stops at `stop` */
static int pop_to(int* stk, int sp, const char* const* want, const char* const* stop) {
    for (int k = sp; k > 0; k--) {
        if (tag_is(nodes[stk[k]].tag, want)) return k - 1;
        if (stop && tag_is(nodes[stk[k]].tag, stop)) break;
    }
    return sp;
}

static void dom_build(const char* s, int n) {
    static const char* const W_P[] = { "p", 0 }, * const W_LI[] = { "li", 0 }, * const S_LI[] = { "ul", "ol", 0 };
    static const char* const W_D[] = { "dt", "dd", 0 }, * const S_D[] = { "dl", 0 };
    static const char* const W_TR[] = { "tr", 0 }, * const S_TR[] = { "table", "tbody", "thead", "tfoot", 0 };
    static const char* const W_TD[] = { "td", "th", 0 }, * const S_TD[] = { "tr", "table", 0 };
    static const char* const W_TB[] = { "tbody", "thead", "tfoot", 0 }, * const S_TB[] = { "table", 0 };
    n_nodes = n_clsh = 0;
    int root = new_node(-1, 0);
    scpy(nodes[root].tag, 10, "#doc");
    static int stk[200];
    int sp = 0;
    stk[0] = root;
    static char cb[512];
    int i = 0;
    while (i < n) {
        if (s[i] != '<') {
            int j = i;
            while (j < n && s[j] != '<') j++;
            int k = new_node(stk[sp], 1);
            if (k < 0) break;
            nodes[k].a = i; nodes[k].an = j - i;
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
            int k = new_node(stk[sp], 1);           /* a literal '<' */
            if (k < 0) break;
            nodes[k].a = i; nodes[k].an = 1;
            i++;
            continue;
        }
        char t[10];
        int tl = 0;
        while (j < n && s[j] != '>' && s[j] != ' ' && s[j] != '/' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r') {
            if (tl < 9) t[tl++] = lower(s[j]);
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
        int pos = j < n ? j + 1 : n;
        if (t[0] == '!' || t[0] == '?') { i = pos; continue; }
        if (close) {
            for (int k = sp; k > 0; k--) if (!strcmp(nodes[stk[k]].tag, t)) { sp = k - 1; break; }
            i = pos;
            continue;
        }
        /* implied end tags, same idea as in dom.js */
        if (tag_is(t, T_CLOSEP))
            for (int k = sp; k > 0; k--) {
                if (is_tag(stk[k], "p")) { sp = k - 1; break; }
                if (!tag_is(nodes[stk[k]].tag, T_INL)) break;
            }
        if (!strcmp(t, "li")) sp = pop_to(stk, sp, W_LI, S_LI);
        else if (!strcmp(t, "dt") || !strcmp(t, "dd")) sp = pop_to(stk, sp, W_D, S_D);
        else if (!strcmp(t, "tr")) sp = pop_to(stk, sp, W_TR, S_TR);
        else if (!strcmp(t, "td") || !strcmp(t, "th")) sp = pop_to(stk, sp, W_TD, S_TD);
        else if (tag_is(t, W_TB)) sp = pop_to(stk, sp, W_TB, S_TB);
        else if (!strcmp(t, "option") && is_tag(stk[sp], "option")) sp--;
        (void)W_P;

        int k = new_node(stk[sp], 0);
        if (k < 0) break;
        node_t* N = &nodes[k];
        N->a = a0; N->an = j - a0;
        scpy(N->tag, 10, t);
        N->th = hsh(t, tl);
        attrs_t A = { s + a0, j - a0 };
        if (get_attr(A, "id", cb, 64)) N->idh = hsh(cb, (int)strlen(cb));
        if (get_attr(A, "class", cb, sizeof cb)) {
            N->cls = n_clsh;
            for (char* p = cb; *p; ) {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                char* e = p;
                while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
                if (e > p && n_clsh < MAX_CLS && N->ncls < 16) { clsh[n_clsh++] = hsh(p, (int)(e - p)); N->ncls++; }
                p = e;
            }
        }
        bool self = N->an > 0 && s[j - 1] == '/';
        if (tag_is(t, T_RAW) && !self) {
            char pat[16] = "</";
            scat(pat, sizeof pat, t);
            const char* e = find_ci(s + pos, n - pos, pat);
            int end = e ? (int)(e - s) : n;
            if (!strcmp(t, "style") || !strcmp(t, "textarea") || !strcmp(t, "title")) {
                int c = new_node(k, 1);
                if (c >= 0) { nodes[c].a = pos; nodes[c].an = end - pos; }
            }
            int m = end;
            while (m < n && s[m] != '>') m++;
            i = m < n ? m + 1 : n;
            continue;
        }
        if (!tag_is(t, T_VOID) && !self && sp < 199) stk[++sp] = k;
        i = pos;
    }
}

static int elem_prev(int n) { int p = nodes[n].prev; while (p >= 0 && nodes[p].text) p = nodes[p].prev; return p; }
static int elem_next(int n) { int p = nodes[n].next; while (p >= 0 && nodes[p].text) p = nodes[p].next; return p; }
static attrs_t node_attrs(int n) { attrs_t A = { (const char*)resp + nodes[n].a, nodes[n].an }; return A; }
static int elem_parent(int n) { int p = nodes[n].parent; return p > 0 ? p : -1; }

/* ---------- css: rules ---------- */
#define MAX_RULES 8000
#define MAX_COMPS 16000
#define MAX_DECLS 40000
#define MAX_ACONDS 2000

enum { PF_FIRST = 1, PF_LAST = 2, PF_NTH = 4, PF_NTHL = 8, PF_ROOT = 16, PF_EMPTY = 32, PF_LINK = 64,
       PF_NEVER = 128, PF_FTYPE = 256, PF_LTYPE = 512, PF_CHECKED = 1024, PF_DISABLED = 2048, PF_NTHT = 4096 };

typedef struct {
    uint32_t tag, id, cls[6];
    uint8_t ncls, comb, nattr, nsub, subk;
    uint16_t pf;
    int16_t na, nb;
    int attr, sub;
} comp_t;
typedef struct { char n[20]; uint8_t op, ci; const char* v; int vl; } acond_t;
typedef struct { int comp, d0, spec, next, stamp; uint16_t nd; uint8_t ncomp, pel; } rule_t;
typedef struct { const char* n; const char* v; uint16_t nl, vl; uint8_t p, imp; } decl_t;

static comp_t*  comps;
static rule_t*  rules;
static decl_t*  decls;
static acond_t* aconds;
static int      n_comps, n_rules, n_decls, n_aconds, n_ua;
static int      bucket[1024], ubucket;

enum {
    P_NONE, P_VAR, P_COLOR, P_BG, P_BGC, P_BGI, P_DISPLAY, P_VIS, P_FONT, P_FS, P_FW, P_FST, P_FF, P_LH, P_LS,
    P_WSP, P_TA, P_TI, P_TT, P_TD, P_TDL, P_WS, P_VA, P_LIST, P_LST,
    P_M, P_MT, P_MR, P_MB, P_ML, P_P, P_PT, P_PR, P_PB, P_PL,
    P_B, P_BT, P_BR, P_BB, P_BL, P_BW, P_BC, P_BS, P_BTW, P_BRW, P_BBW, P_BLW, P_BTC, P_BRC, P_BBC, P_BLC,
    P_BTS, P_BRS, P_BBS, P_BLS, P_RAD,
    P_W, P_H, P_MINW, P_MAXW, P_MINH, P_BOX, P_OVF, P_OVFX, P_OVFY, P_FLOAT, P_CLEAR, P_POS,
    P_TOP, P_RIGHT, P_BOTTOM, P_LEFT, P_INSET, P_OPA, P_CONTENT,
    P_FDIR, P_FWRAP, P_FFLOW, P_JC, P_AI, P_FLEX, P_FGROW, P_FBASIS, P_GAP, P_CGAP, P_RGAP, P_GTC, P_GCOL,
    P_CLIP, P_CLIPPATH, P_MI, P_MBL, P_MIS, P_MIE, P_PI, P_PBL, P_PIS, P_PIE, P_BSP, P_PLACE, P_BGSIZE, P_BGREP, P_GTA, P_GT, P_GAREA, P_GROW, P_GCS, P_GCE, P_GRS, P_GRE, P_MASK, P_WMASK,
    P_COUNT
};
static const char* const pnames[P_COUNT] = {
    "", "", "color", "background", "background-color", "background-image", "display", "visibility", "font",
    "font-size", "font-weight", "font-style", "font-family", "line-height", "letter-spacing",
    "word-spacing", "text-align", "text-indent", "text-transform", "text-decoration", "text-decoration-line",
    "white-space", "vertical-align", "list-style", "list-style-type",
    "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
    "border", "border-top", "border-right", "border-bottom", "border-left", "border-width", "border-color",
    "border-style", "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style", "border-radius",
    "width", "height", "min-width", "max-width", "min-height", "box-sizing", "overflow", "overflow-x",
    "overflow-y", "float", "clear", "position", "top", "right", "bottom", "left", "inset", "opacity", "content",
    "flex-direction", "flex-wrap", "flex-flow", "justify-content", "align-items", "flex", "flex-grow",
    "flex-basis", "gap", "column-gap", "row-gap", "grid-template-columns", "grid-column",
    "clip", "clip-path", "margin-inline", "margin-block", "margin-inline-start", "margin-inline-end",
    "padding-inline", "padding-block", "padding-inline-start", "padding-inline-end", "border-spacing",
    "place-items", "background-size", "background-repeat", "grid-template-areas", "grid-template", "grid-area",
    "grid-row", "grid-column-start", "grid-column-end", "grid-row-start", "grid-row-end", "mask-image", "-webkit-mask-image",
};

static bool isws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }

/* s[i] == '{' -> index of the matching '}' (or n) */
static int blk_end(const char* s, int i, int n) {
    int d = 0;
    char q = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (q) { if (c == '\\') i++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '{') d++;
        else if (c == '}' && --d == 0) return i;
    }
    return n;
}

/* first char from `stop` at depth 0, outside quotes and parens */
static int find_top(const char* s, int i, int n, const char* stop) {
    int d = 0;
    char q = 0;
    for (; i < n; i++) {
        char c = s[i];
        if (q) { if (c == '\\') i++; else if (c == q) q = 0; continue; }
        if (d <= 0 && strchr(stop, c)) return i;
        if (c == '"' || c == '\'') q = c;
        else if (c == '(' || c == '[') d++;
        else if (c == ')' || c == ']') d--;
    }
    return n;
}

static void css_strip(char* s, int n) {
    char q = 0;
    for (int i = 0; i < n; i++) {
        if (q) { if (s[i] == '\\') i++; else if (s[i] == q || s[i] == '\n') q = 0; continue; }
        if (s[i] == '"' || s[i] == '\'') { q = s[i]; continue; }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            j = j + 1 < n ? j + 2 : n;
            for (int k = i; k < j; k++) s[k] = ' ';
            i = j - 1;
        }
    }
}

static int prop_id(const char* s, int n) {
    for (int i = 2; i < P_COUNT; i++)
        if ((int)strlen(pnames[i]) == n && !memcmp(pnames[i], s, (size_t)n)) return i;
    return 0;
}

static int css_decls(const char* s, int n, int cap) {
    int cnt = 0, i = 0;
    char pn[32];
    while (i < n) {
        int a = i, d = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(') d++;
            else if (c == ')') d--;
            else if (c == '{') { i = blk_end(s, i, n) + 1; a = i; continue; }   /* css nesting: skipped */
            else if (c == ';' && d <= 0) break;
            i++;
        }
        int b = i < n ? i : n;
        if (i < n) i++;
        int c = a;
        while (c < b && s[c] != ':') c++;
        if (c >= b) continue;
        int n0 = a, n1 = c;
        while (n0 < n1 && isws(s[n0])) n0++;
        while (n1 > n0 && isws(s[n1 - 1])) n1--;
        int v0 = c + 1, v1 = b;
        while (v0 < v1 && isws(s[v0])) v0++;
        while (v1 > v0 && isws(s[v1 - 1])) v1--;
        bool imp = false;
        if (v1 - v0 >= 10 && starts_ci(s + v1 - 9, "important")) {
            int k = v1 - 9;
            while (k > v0 && isws(s[k - 1])) k--;
            if (k > v0 && s[k - 1] == '!') { imp = true; v1 = k - 1; while (v1 > v0 && isws(s[v1 - 1])) v1--; }
        }
        int p;
        if (n1 - n0 > 2 && s[n0] == '-' && s[n0 + 1] == '-') p = P_VAR;
        else {
            int l = 0;
            for (int k = n0; k < n1 && l < 31; k++) pn[l++] = lower(s[k]);
            pn[l] = 0;
            p = prop_id(pn, l);
        }
        if (!p || n_decls >= cap) continue;
        decl_t* D = &decls[n_decls++];
        D->n = s + n0; D->nl = (uint16_t)(n1 - n0);
        D->v = s + v0; D->vl = (uint16_t)(v1 - v0 > 60000 ? 60000 : v1 - v0);
        D->p = (uint8_t)p; D->imp = imp;
        cnt++;
    }
    return cnt;
}

static int rd_ident(const char* s, int i, int n, char* o, int cap) {
    int k = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i++;
            if (hexval(s[i]) >= 0) {
                int v = 0, d = 0;
                while (i < n && d < 6 && hexval(s[i]) >= 0) { v = v * 16 + hexval(s[i]); i++; d++; }
                if (i < n && s[i] == ' ') i++;
                if (k < cap - 1) o[k++] = (char)(v < 256 ? v : '?');
                continue;
            }
            if (k < cap - 1) o[k++] = s[i];
            i++;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (uint8_t)c >= 0x80)) break;
        if (k < cap - 1) o[k++] = c;
        i++;
    }
    o[k] = 0;
    return i;
}

/* an+b, odd, even */
static void nth_parse(const char* s, int n, int16_t* a, int16_t* b) {
    char t[32];
    int k = 0;
    for (int i = 0; i < n && k < 31; i++) { if (isws(s[i])) continue; if (starts_ci(s + i, "of")) break; t[k++] = lower(s[i]); }
    t[k] = 0;
    if (!strcmp(t, "odd")) { *a = 2; *b = 1; return; }
    if (!strcmp(t, "even")) { *a = 2; *b = 0; return; }
    char* np = strchr(t, 'n');
    if (!np) { *a = 0; *b = (int16_t)atoi(t); return; }
    *np = 0;
    *a = !t[0] || !strcmp(t, "+") ? 1 : !strcmp(t, "-") ? -1 : (int16_t)atoi(t);
    const char* r = np + 1;
    *b = 0;
    if (*r == '+') *b = (int16_t)atoi(r + 1);
    else if (*r == '-') *b = (int16_t)-atoi(r + 1);
}

static int comp_parse(const char* s, int i, int n, comp_t* c, int* spec, uint8_t* pel);

/* :not(a, b) / :is(...) arguments -> contiguous compounds */
static bool sub_parse(const char* s, int a0, int a1, comp_t* c, int kind, int* spec) {
    comp_t tmp[8];
    int nt = 0, i = a0, best = 0;
    while (i < a1) {
        int e = find_top(s, i, a1, ",");
        int x = i, y = e;
        while (x < y && isws(s[x])) x++;
        while (y > x && isws(s[y - 1])) y--;
        if (y > x) {
            if (nt == 8) return false;
            int sp = 0;
            uint8_t pe = 0;
            int r = comp_parse(s, x, y, &tmp[nt], &sp, &pe);
            if (r != y || pe) return false;              /* complex selector inside: give up */
            if (sp > best) best = sp;
            nt++;
        }
        i = e + 1;
    }
    if (!nt || n_comps + nt > MAX_COMPS) return false;
    if (c->nsub) {
        if (c->subk != kind || c->sub + c->nsub != n_comps) return false;
    } else { c->sub = n_comps; c->subk = (uint8_t)kind; }
    memcpy(comps + n_comps, tmp, sizeof(comp_t) * (size_t)nt);
    n_comps += nt;
    c->nsub = (uint8_t)(c->nsub + nt);
    if (kind != 3) *spec += best;                        /* :where() weighs nothing */
    return true;
}

static int comp_parse(const char* s, int i, int n, comp_t* c, int* spec, uint8_t* pel) {
    memset(c, 0, sizeof *c);
    c->attr = n_aconds;
    char id[64];
    bool any = false;
    while (i < n) {
        char ch = s[i];
        if (ch == '*') { i++; any = true; continue; }
        if (ch == '#' || ch == '.') {
            i = rd_ident(s, i + 1, n, id, sizeof id);
            if (!id[0]) return -1;
            uint32_t h = hsh(id, (int)strlen(id));
            if (ch == '#') { c->id = h; *spec += 10000; }
            else { if (c->ncls < 6) c->cls[c->ncls++] = h; else c->pf |= PF_NEVER; *spec += 100; }
            any = true;
            continue;
        }
        if (ch == '[') {
            int e = find_top(s, i + 1, n, "]");
            if (e >= n || n_aconds >= MAX_ACONDS) return -1;
            acond_t* A = &aconds[n_aconds];
            int k = i + 1, l = 0;
            while (k < e && isws(s[k])) k++;
            while (k < e && s[k] != '=' && !isws(s[k]) && !strchr("~^$*|", s[k])) { if (l < 19) A->n[l++] = lower(s[k]); k++; }
            A->n[l] = 0;
            A->op = 0; A->ci = 0; A->v = NULL; A->vl = 0;
            while (k < e && isws(s[k])) k++;
            if (k < e) {
                if (s[k] == '=') { A->op = '='; k++; }
                else if (k + 1 < e && s[k + 1] == '=') { A->op = (uint8_t)s[k]; k += 2; }
                else return -1;
                while (k < e && isws(s[k])) k++;
                if (k < e && (s[k] == '"' || s[k] == '\'')) {
                    char qq = s[k++];
                    int v0 = k;
                    while (k < e && s[k] != qq) k++;
                    A->v = s + v0; A->vl = k - v0;
                    k++;
                } else {
                    int v0 = k;
                    while (k < e && !isws(s[k])) k++;
                    A->v = s + v0; A->vl = k - v0;
                }
                while (k < e && isws(s[k])) k++;
                if (k < e && (s[k] == 'i' || s[k] == 'I')) A->ci = 1;
            }
            n_aconds++;
            c->nattr++;
            *spec += 100;
            any = true;
            i = e + 1;
            continue;
        }
        if (ch == ':') {
            bool el = i + 1 < n && s[i + 1] == ':';
            i += el ? 2 : 1;
            char nm[32];
            i = rd_ident(s, i, n, nm, sizeof nm);
            for (char* p = nm; *p; p++) *p = lower(*p);
            int a0 = -1, a1 = -1;
            if (i < n && s[i] == '(') {
                a0 = i + 1;
                a1 = find_top(s, a0, n, ")");
                if (a1 >= n) return -1;
                i = a1 + 1;
            }
            any = true;
            if (!strcmp(nm, "before") || !strcmp(nm, "after")) { *pel = nm[0] == 'b' ? 1 : 2; *spec += 1; continue; }
            if (el) return -1;                                /* ::marker, ::placeholder, ... */
            *spec += 100;
            if (!strcmp(nm, "first-child")) c->pf |= PF_FIRST;
            else if (!strcmp(nm, "last-child")) c->pf |= PF_LAST;
            else if (!strcmp(nm, "only-child")) c->pf |= PF_FIRST | PF_LAST;
            else if (!strcmp(nm, "first-of-type")) c->pf |= PF_FTYPE;
            else if (!strcmp(nm, "last-of-type")) c->pf |= PF_LTYPE;
            else if (!strcmp(nm, "only-of-type")) c->pf |= PF_FTYPE | PF_LTYPE;
            else if (!strcmp(nm, "nth-child") && a0 >= 0) { c->pf |= PF_NTH; nth_parse(s + a0, a1 - a0, &c->na, &c->nb); }
            else if (!strcmp(nm, "nth-last-child") && a0 >= 0) { c->pf |= PF_NTHL; nth_parse(s + a0, a1 - a0, &c->na, &c->nb); }
            else if (!strcmp(nm, "nth-of-type") && a0 >= 0) { c->pf |= PF_NTHT; nth_parse(s + a0, a1 - a0, &c->na, &c->nb); }
            else if (!strcmp(nm, "root")) c->pf |= PF_ROOT;
            else if (!strcmp(nm, "empty")) c->pf |= PF_EMPTY;
            else if (!strcmp(nm, "link") || !strcmp(nm, "any-link")) c->pf |= PF_LINK;
            else if (!strcmp(nm, "checked")) c->pf |= PF_CHECKED;
            else if (!strcmp(nm, "disabled")) c->pf |= PF_DISABLED;
            else if (!strcmp(nm, "not") && a0 >= 0) { *spec -= 100; if (!sub_parse(s, a0, a1, c, 1, spec)) return -1; }
            else if ((!strcmp(nm, "is") || !strcmp(nm, "matches") || !strcmp(nm, "-webkit-any") || !strcmp(nm, "where")) && a0 >= 0) {
                *spec -= 100;
                if (!sub_parse(s, a0, a1, c, nm[0] == 'w' ? 3 : 2, spec)) return -1;
            }
            else if (!strcmp(nm, "lang") || !strcmp(nm, "dir") || !strcmp(nm, "defined") || !strcmp(nm, "enabled") ||
                     !strcmp(nm, "scope") || !strcmp(nm, "optional") || !strcmp(nm, "valid") || !strcmp(nm, "read-write")) ;
            else c->pf |= PF_NEVER;                           /* hover, focus, visited, has...: never */
            continue;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '\\') {
            i = rd_ident(s, i, n, id, sizeof id);
            for (char* p = id; *p; p++) *p = lower(*p);
            c->tag = hsh(id, (int)strlen(id));
            *spec += 1;
            any = true;
            continue;
        }
        break;
    }
    return any ? i : -1;
}

static bool sel_add(const char* s, int n, int d0, int nd) {
    comp_t tmp[8];
    int nc = 0, spec = 0, i = 0;
    uint8_t pel = 0;
    char comb = 0;
    int c0 = n_comps, a0 = n_aconds;
    while (i < n) {
        while (i < n && isws(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') { comb = s[i++]; continue; }
        if (nc == 8 || pel) goto bad;
        int e = comp_parse(s, i, n, &tmp[nc], &spec, &pel);
        if (e < 0) goto bad;
        tmp[nc].comb = (uint8_t)(nc ? (comb ? comb : ' ') : 0);
        nc++;
        comb = 0;
        i = e;
        if (i < n && !isws(s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~') goto bad;
    }
    if (!nc || n_rules >= MAX_RULES || n_comps + nc > MAX_COMPS) goto bad;
    rule_t* r = &rules[n_rules];
    r->comp = n_comps;
    memcpy(comps + n_comps, tmp, sizeof(comp_t) * (size_t)nc);
    n_comps += nc;
    r->ncomp = (uint8_t)nc; r->pel = pel; r->spec = spec; r->d0 = d0; r->nd = (uint16_t)nd; r->stamp = 0;
    comp_t* R = &tmp[nc - 1];
    uint32_t key = R->id ? R->id : R->ncls ? R->cls[0] : R->tag;
    if (key) { r->next = bucket[key & 1023]; bucket[key & 1023] = n_rules; }
    else { r->next = ubucket; ubucket = n_rules; }
    n_rules++;
    return true;
bad:
    n_comps = c0; n_aconds = a0;
    return false;
}

static int g_vw = 940, g_vh = 700;

static int media_len(const char* p) {
    int v = atoi(p);
    while ((*p >= '0' && *p <= '9') || *p == '.') p++;
    if (starts_ci(p, "em") || starts_ci(p, "rem")) v *= 16;
    return v;
}

static bool media_feat(const char* f) {
    while (*f == ' ') f++;
    const char* c = strchr(f, ':');
    const char* cl = strchr(f, ')');
    if (!c || (cl && c > cl)) {
        /* range syntax: (width >= 600px), (400px <= width < 900px) */
        const char* w = strstr(f, "width");
        if (!w || (cl && w > cl)) return false;
        bool ok = true;
        for (const char* p = f; *p && *p != ')'; p++) {
            if (*p != '<' && *p != '>') continue;
            bool lt = *p == '<', eq = p[1] == '=';
            const char* num;
            if (p > w) { num = p + 1 + eq; while (*num == ' ') num++; }
            else { num = f; lt = !lt; }
            int v = media_len(num);
            if (lt) ok = ok && (eq ? g_vw <= v : g_vw < v);
            else ok = ok && (eq ? g_vw >= v : g_vw > v);
            if (eq) p++;
            if (p < w) f = w;                                /* next number is after "width" */
        }
        return ok;
    }
    const char* v = c + 1;
    while (*v == ' ') v++;
    int nl = (int)(c - f);
    while (nl > 0 && f[nl - 1] == ' ') nl--;
#define FN(x) (nl == (int)sizeof(x) - 1 && !strncmp(f, x, (size_t)nl))
    if (FN("min-width")) return g_vw >= media_len(v);
    if (FN("max-width")) return g_vw <= media_len(v);
    if (FN("width")) return g_vw == media_len(v);
    if (FN("min-height")) return g_vh >= media_len(v);
    if (FN("max-height")) return g_vh <= media_len(v);
    if (FN("prefers-color-scheme")) return starts_ci(v, "light");
    if (FN("prefers-reduced-motion")) return !starts_ci(v, "reduce");
    if (FN("prefers-contrast") || FN("forced-colors")) return starts_ci(v, "no-pref") || starts_ci(v, "none");
    if (FN("orientation")) return starts_ci(v, "landscape");
    if (FN("hover") || FN("any-hover")) return starts_ci(v, "hover");
    if (FN("pointer") || FN("any-pointer")) return starts_ci(v, "fine");
    if (FN("scripting")) return starts_ci(v, "enabled");
    if (FN("display-mode")) return starts_ci(v, "browser");
    if (FN("min-resolution") || FN("-webkit-min-device-pixel-ratio") || FN("min--moz-device-pixel-ratio"))
        return atoi(v) <= 1 && !strstr(v, "dpi");
    if (FN("max-resolution") || FN("-webkit-max-device-pixel-ratio")) return true;
#undef FN
    return false;
}

static bool media_ok(const char* s, int n) {
    static char b[400];
    if (n > 399) n = 399;
    int k = 0;
    for (int i = 0; i < n; i++) b[k++] = isws(s[i]) ? ' ' : lower(s[i]);
    b[k] = 0;
    char* p = b;
    for (;;) {
        char* e = strchr(p, ',');
        if (e) *e = 0;
        while (*p == ' ') p++;
        bool neg = !strncmp(p, "not ", 4);
        if (neg) p += 4;
        if (!strncmp(p, "only ", 5)) p += 5;
        bool ok = true;
        if (!strncmp(p, "print", 5) || !strncmp(p, "speech", 6) || !strncmp(p, "tv", 2) || !strncmp(p, "aural", 5)) ok = false;
        for (char* q = strchr(p, '('); q && ok; q = strchr(q + 1, '(')) {
            bool fneg = q - p >= 4 && !strncmp(q - 4, "not ", 4);
            bool r = media_feat(q + 1);
            if (fneg) r = !r;
            if (!r) ok = false;
        }
        if (ok != neg) return true;
        if (!e) return false;
        p = e + 1;
    }
}

static void css_parse(const char* s, int n, int depth) {
    int i = 0;
    int cap = MAX_DECLS - 400;                               /* headroom for style="" */
    while (i < n) {
        while (i < n && (isws(s[i]) || s[i] == ';' || s[i] == '}')) i++;
        if (i >= n) break;
        if (s[i] == '<' && i + 3 < n && s[i + 1] == '!') { i += 4; continue; }
        if (s[i] == '-' && i + 2 < n && s[i + 1] == '-' && s[i + 2] == '>') { i += 3; continue; }
        if (s[i] == '@') {
            char nm[24];
            int k = rd_ident(s, i + 1, n, nm, sizeof nm);
            for (char* p = nm; *p; p++) *p = lower(*p);
            int e = find_top(s, k, n, "{;");
            if (e >= n || s[e] == ';') { i = e + 1; continue; }
            int be = blk_end(s, e, n);
            if (depth < 5) {
                if (!strcmp(nm, "media")) { if (media_ok(s + k, e - k)) css_parse(s + e + 1, be - e - 1, depth + 1); }
                else if (!strcmp(nm, "supports")) {
                    int x = k;
                    while (x < e && isws(s[x])) x++;
                    if (!starts_ci(s + x, "not") && !find_ci(s + k, e - k, "selector(")) css_parse(s + e + 1, be - e - 1, depth + 1);
                }
                else if (!strcmp(nm, "layer") || !strcmp(nm, "scope") || !strcmp(nm, "-moz-document") || !strcmp(nm, "document"))
                    css_parse(s + e + 1, be - e - 1, depth + 1);
            }
            i = be + 1;
            continue;
        }
        int a = i;
        int e = find_top(s, i, n, "{}");
        if (e >= n) break;
        if (s[e] == '}') { i = e + 1; continue; }
        int be = blk_end(s, e, n);
        int d0 = n_decls;
        int nd = css_decls(s + e + 1, be - e - 1, cap);
        if (nd)
            for (int k = a; k < e; ) {
                int c = find_top(s, k, e, ",");
                sel_add(s + k, c - k, d0, nd);
                k = c + 1;
            }
        i = be + 1;
    }
}

static const char UA_CSS[] =
    "html,body,div,section,article,header,footer,nav,main,aside,p,h1,h2,h3,h4,h5,h6,ul,ol,dl,dt,dd,"
    "blockquote,pre,form,fieldset,figure,figcaption,address,details,summary,hr,center,menu,dir,legend,"
    "hgroup,search,optgroup,listing,plaintext,xmp{display:block}"
    "head,script,style,title,meta,link,base,template,noscript,svg,math,select,datalist,option,param,source,"
    "track,area,map,iframe,object,embed,canvas,video,audio,noembed,col,colgroup,dialog,[hidden],"
    "input[type=hidden]{display:none}dialog[open]{display:block}"
    "body{margin:8px}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:1em 0;font-weight:bold}h4{margin:1.33em 0;font-weight:bold}"
    "h5{font-size:.83em;margin:1.67em 0;font-weight:bold}h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
    "p,dl,pre,ul,ol,menu,dir{margin:1em 0}blockquote,figure{margin:1em 40px}"
    "ul,ol,menu,dir{padding-left:40px}li{display:list-item}ol{list-style-type:decimal}"
    "ul ul,ol ul,ul ol,ol ol{margin:0}ul ul,ol ul{list-style-type:circle}ul ul ul,ul ol ul,ol ul ul{list-style-type:square}"
    "dd{margin-left:40px}b,strong,th,dt{font-weight:bold}i,em,cite,var,dfn,address{font-style:italic}"
    "code,kbd,samp,tt,pre,xmp,listing,plaintext{font-family:monospace}pre,xmp,listing,plaintext{white-space:pre}"
    "small,sub,sup{font-size:smaller}big{font-size:larger}sub{vertical-align:sub}sup{vertical-align:super}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a:link{color:#1f5cb8;text-decoration:underline}mark{background:#ff0;color:#000}"
    "hr{border-top:1px solid #d6d2c8;margin:.5em 0}"
    "table{display:table;border-spacing:2px}tr{display:table-row}td,th{display:table-cell;padding:1px;vertical-align:middle}"
    "thead,tbody,tfoot{display:table-row-group}caption{display:table-caption;text-align:center}th{text-align:center}"
    "center{text-align:-webkit-center}fieldset{margin:0 2px;padding:.35em .75em .6em;border:2px groove #ccc}"
    "legend{padding:0 2px}summary{display:list-item;list-style-type:disclosure-open}"
    "img,input,button,textarea{display:inline-block}q:before{content:open-quote}q:after{content:close-quote}"
    "nobr{white-space:nowrap}";

/* ---------- css: values ---------- */
#define LA (-99999)

static const char CSS_COLORS[] =
    "aliceblue f0f8ff antiquewhite faebd7 aqua 00ffff aquamarine 7fffd4 azure f0ffff beige f5f5dc bisque ffe4c4 "
    "black 000000 blanchedalmond ffebcd blue 0000ff blueviolet 8a2be2 brown a52a2a burlywood deb887 cadetblue 5f9ea0 "
    "chartreuse 7fff00 chocolate d2691e coral ff7f50 cornflowerblue 6495ed cornsilk fff8dc crimson dc143c cyan 00ffff "
    "darkblue 00008b darkcyan 008b8b darkgoldenrod b8860b darkgray a9a9a9 darkgreen 006400 darkgrey a9a9a9 "
    "darkkhaki bdb76b darkmagenta 8b008b darkolivegreen 556b2f darkorange ff8c00 darkorchid 9932cc darkred 8b0000 "
    "darksalmon e9967a darkseagreen 8fbc8f darkslateblue 483d8b darkslategray 2f4f4f darkslategrey 2f4f4f "
    "darkturquoise 00ced1 darkviolet 9400d3 deeppink ff1493 deepskyblue 00bfff dimgray 696969 dimgrey 696969 "
    "dodgerblue 1e90ff firebrick b22222 floralwhite fffaf0 forestgreen 228b22 fuchsia ff00ff gainsboro dcdcdc "
    "ghostwhite f8f8ff gold ffd700 goldenrod daa520 gray 808080 green 008000 greenyellow adff2f grey 808080 "
    "honeydew f0fff0 hotpink ff69b4 indianred cd5c5c indigo 4b0082 ivory fffff0 khaki f0e68c lavender e6e6fa "
    "lavenderblush fff0f5 lawngreen 7cfc00 lemonchiffon fffacd lightblue add8e6 lightcoral f08080 lightcyan e0ffff "
    "lightgoldenrodyellow fafad2 lightgray d3d3d3 lightgreen 90ee90 lightgrey d3d3d3 lightpink ffb6c1 "
    "lightsalmon ffa07a lightseagreen 20b2aa lightskyblue 87cefa lightslategray 778899 lightslategrey 778899 "
    "lightsteelblue b0c4de lightyellow ffffe0 lime 00ff00 limegreen 32cd32 linen faf0e6 magenta ff00ff maroon 800000 "
    "mediumaquamarine 66cdaa mediumblue 0000cd mediumorchid ba55d3 mediumpurple 9370db mediumseagreen 3cb371 "
    "mediumslateblue 7b68ee mediumspringgreen 00fa9a mediumturquoise 48d1cc mediumvioletred c71585 "
    "midnightblue 191970 mintcream f5fffa mistyrose ffe4e1 moccasin ffe4b5 navajowhite ffdead navy 000080 "
    "oldlace fdf5e6 olive 808000 olivedrab 6b8e23 orange ffa500 orangered ff4500 orchid da70d6 palegoldenrod eee8aa "
    "palegreen 98fb98 paleturquoise afeeee palevioletred db7093 papayawhip ffefd5 peachpuff ffdab9 peru cd853f "
    "pink ffc0cb plum dda0dd powderblue b0e0e6 purple 800080 rebeccapurple 663399 red ff0000 rosybrown bc8f8f "
    "royalblue 4169e1 saddlebrown 8b4513 salmon fa8072 sandybrown f4a460 seagreen 2e8b57 seashell fff5ee "
    "sienna a0522d silver c0c0c0 skyblue 87ceeb slateblue 6a5acd slategray 708090 slategrey 708090 snow fffafa "
    "springgreen 00ff7f steelblue 4682b4 tan d2b48c teal 008080 thistle d8bfd8 tomato ff6347 turquoise 40e0d0 "
    "violet ee82ee wheat f5deb3 white ffffff whitesmoke f5f5f5 yellow ffff00 yellowgreen 9acd32 "
    "canvas fbfaf7 canvastext 26231f linktext 1f5cb8 buttonface efefef buttontext 000000 graytext 808080 ";

static const char* skipws(const char* p) { while (*p == ' ') p++; return p; }
static bool isal(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* decimal number -> thousandths */
static const char* pnum(const char* p, int* out) {
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (!((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9'))) return NULL;
    long long v = 0;
    while (*p >= '0' && *p <= '9') { if (v < 10000000) v = v * 10 + (*p - '0'); p++; }
    v *= 1000;
    if (*p == '.') {
        p++;
        int m = 100;
        while (*p >= '0' && *p <= '9') { v += (*p - '0') * m; m /= 10; p++; }
    }
    if ((*p == 'e' || *p == 'E') && ((p[1] >= '0' && p[1] <= '9') || (p[1] == '-' && p[2] >= '0' && p[2] <= '9'))) {
        p++;
        bool en = *p == '-';
        if (en) p++;
        int e = 0;
        while (*p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
        while (e-- > 0 && v) v = en ? v / 10 : (v < 100000000 ? v * 10 : v);
    }
    *out = (int)(neg ? -v : v);
    return p;
}

typedef struct { long long v; int num; } cv_t;        /* v: 1/1000 px (or plain number) */
static const char* calc_sum(const char* p, int fs, int pct, cv_t* r);

static const char* calc_fac(const char* p, int fs, int pct, cv_t* r) {
    p = skipws(p);
    if (*p == '(') {
        p = calc_sum(p + 1, fs, pct, r);
        if (!p) return NULL;
        p = skipws(p);
        return *p == ')' ? p + 1 : p;
    }
    if (starts_ci(p, "calc(")) return calc_fac(p + 4, fs, pct, r);
    if (starts_ci(p, "min(") || starts_ci(p, "max(") || starts_ci(p, "clamp(")) {
        int kind = lower(p[1]) == 'i' ? 0 : lower(p[1]) == 'a' ? 1 : 2;
        p = strchr(p, '(') + 1;
        cv_t a[3];
        int k = 0;
        for (;;) {
            cv_t t;
            p = calc_sum(p, fs, pct, &t);
            if (!p) return NULL;
            if (k < 3) a[k++] = t;
            p = skipws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ')') p++;
            break;
        }
        *r = a[0];
        if (kind == 2 && k == 3) { *r = a[1]; if (r->v < a[0].v) *r = a[0]; if (r->v > a[2].v) *r = a[2]; }
        else for (int i = 1; i < k; i++) if (kind == 0 ? a[i].v < r->v : a[i].v > r->v) *r = a[i];
        return p;
    }
    int v;
    const char* e = pnum(p, &v);
    if (!e) return NULL;
    char u[8];
    int ul = 0;
    while ((isal(*e) || *e == '%') && ul < 7) u[ul++] = lower(*e++);
    u[ul] = 0;
    long long x = v;
    r->num = 0;
    if (!ul) r->num = 1;
    else if (!strcmp(u, "px")) ;
    else if (!strcmp(u, "em")) x = x * fs;
    else if (!strcmp(u, "rem")) x = x * 16;
    else if (!strcmp(u, "%")) x = x * pct / 100;
    else if (!strcmp(u, "pt")) x = x * 4 / 3;
    else if (!strcmp(u, "pc")) x = x * 16;
    else if (!strcmp(u, "in")) x = x * 96;
    else if (!strcmp(u, "cm")) x = x * 9600 / 254;
    else if (!strcmp(u, "mm")) x = x * 960 / 254;
    else if (!strcmp(u, "vw")) x = x * g_vw / 100;
    else if (!strcmp(u, "vh") || !strcmp(u, "dvh") || !strcmp(u, "svh") || !strcmp(u, "lvh")) x = x * g_vh / 100;
    else if (!strcmp(u, "vmin")) x = x * (g_vw < g_vh ? g_vw : g_vh) / 100;
    else if (!strcmp(u, "vmax")) x = x * (g_vw > g_vh ? g_vw : g_vh) / 100;
    else if (!strcmp(u, "ch") || !strcmp(u, "ex")) x = x * fs / 2;
    else if (!strcmp(u, "fr")) r->num = 2;
    else return NULL;
    r->v = x;
    return e;
}

static const char* calc_term(const char* p, int fs, int pct, cv_t* r) {
    p = calc_fac(p, fs, pct, r);
    while (p) {
        const char* q = skipws(p);
        if (*q != '*' && *q != '/') break;
        cv_t b;
        const char* e = calc_fac(q + 1, fs, pct, &b);
        if (!e) return NULL;
        if (*q == '*') {
            if (r->num == 1) { r->v = r->v * b.v / 1000; r->num = b.num; }
            else r->v = r->v * b.v / 1000;
        } else if (b.v) r->v = r->v * 1000 / b.v;
        p = e;
    }
    return p;
}

static const char* calc_sum(const char* p, int fs, int pct, cv_t* r) {
    p = calc_term(p, fs, pct, r);
    while (p) {
        const char* q = skipws(p);
        if ((*q != '+' && *q != '-') || q == p) break;         /* needs a space before */
        cv_t b;
        const char* e = calc_term(q + 1, fs, pct, &b);
        if (!e) return NULL;
        r->v = *q == '+' ? r->v + b.v : r->v - b.v;
        if (!b.num) r->num = 0;
        p = e;
    }
    return p;
}

/* one length: px out, LA for auto. false = not a length */
static bool plen(const char* p, int fs, int pct, int* out, const char** end) {
    p = skipws(p);
    if (starts_ci(p, "auto")) { *out = LA; if (end) *end = p + 4; return true; }
    cv_t r;
    const char* e = calc_fac(p, fs, pct, &r);
    if (!e || r.num == 2) return false;
    *out = (int)(r.v / 1000);
    if (end) *end = e;
    return true;
}

static uint32_t mix(uint32_t a, uint32_t b, int al) {
    int r = (int)((a >> 16) & 255) * (255 - al) + (int)((b >> 16) & 255) * al;
    int g = (int)((a >> 8) & 255) * (255 - al) + (int)((b >> 8) & 255) * al;
    int c = (int)(a & 255) * (255 - al) + (int)(b & 255) * al;
    return RGB(r / 255, g / 255, c / 255);
}
static int lum(uint32_t c) { return (int)((c >> 16) & 255) * 3 + (int)((c >> 8) & 255) * 6 + (int)(c & 255); }

/* args of rgb()/hsl(): commas, spaces or slash, % flagged */
static int fargs(const char* p, int* v, uint8_t* pc, const char** end) {
    int k = 0;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '/') p++;
        if (*p == ')' || !*p) break;
        int x;
        const char* e = pnum(p, &x);
        if (!e) {
            if (starts_ci(p, "none")) { x = 0; e = p + 4; }
            else return -1;
        }
        uint8_t f = 0;
        if (*e == '%') { f = 1; e++; }
        else if (starts_ci(e, "turn")) { x *= 360; e += 4; }
        else if (starts_ci(e, "deg")) e += 3;
        else if (starts_ci(e, "rad")) { x = x * 573 / 10; e += 3; }
        if (k < 4) { v[k] = x; pc[k] = f; k++; }
        p = e;
    }
    if (*p == ')') p++;
    *end = p;
    return k;
}

static bool pcolor(const char* v, uint32_t cur, uint32_t* out, uint8_t* al, const char** end) {
    v = skipws(v);
    if (end) *end = v;
    if (*v == '#') {
        int d[8], n = 0;
        v++;
        while (n < 8 && hexval(v[n]) >= 0) { d[n] = hexval(v[n]); n++; }
        if (end) *end = v + n;
        *al = 255;
        if (n == 3 || n == 4) { *out = RGB(d[0] * 17, d[1] * 17, d[2] * 17); if (n == 4) *al = (uint8_t)(d[3] * 17); return true; }
        if (n == 6 || n == 8) { *out = RGB(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5]); if (n == 8) *al = (uint8_t)(d[6] * 16 + d[7]); return true; }
        return false;
    }
    if (starts_ci(v, "rgb") || starts_ci(v, "hsl")) {
        const char* q = strchr(v, '(');
        if (!q) return false;
        int x[4];
        uint8_t pc[4];
        const char* e;
        int k = fargs(q + 1, x, pc, &e);
        if (k < 3) return false;
        if (end) *end = e;
        *al = 255;
        if (k == 4) { int a = pc[3] ? x[3] / 100 : x[3]; if (a < 0) a = 0; if (a > 1000) a = 1000; *al = (uint8_t)(a * 255 / 1000); }
        if (lower(v[0]) == 'r') {
            int c[3];
            for (int i = 0; i < 3; i++) {
                c[i] = pc[i] ? x[i] * 255 / 100000 : x[i] / 1000;
                if (c[i] < 0) c[i] = 0;
                if (c[i] > 255) c[i] = 255;
            }
            *out = RGB(c[0], c[1], c[2]);
            return true;
        }
        int h = (x[0] / 1000) % 360, s = x[1] / 100, l = x[2] / 100;     /* s, l: 0..1000 */
        if (h < 0) h += 360;
        if (s > 1000) s = 1000;
        if (l > 1000) l = 1000;
        int c = (1000 - (2 * l - 1000 < 0 ? 1000 - 2 * l : 2 * l - 1000)) * s / 1000;
        int hp = h * 1000 / 60;
        int md = hp % 2000 - 1000;
        int xx = c * (1000 - (md < 0 ? -md : md)) / 1000;
        int m = l - c / 2, r1 = 0, g1 = 0, b1 = 0;
        switch (h / 60) {
        case 0: r1 = c; g1 = xx; break;
        case 1: r1 = xx; g1 = c; break;
        case 2: g1 = c; b1 = xx; break;
        case 3: g1 = xx; b1 = c; break;
        case 4: r1 = xx; b1 = c; break;
        default: r1 = c; b1 = xx; break;
        }
        *out = RGB((r1 + m) * 255 / 1000, (g1 + m) * 255 / 1000, (b1 + m) * 255 / 1000);
        return true;
    }
    char w[24];
    int k = 0;
    while (isal(v[k]) && k < 23) { w[k] = lower(v[k]); k++; }
    w[k] = 0;
    if (!k || isal(v[k]) || v[k] == '-' || v[k] == '(') return false;
    if (end) *end = v + k;
    if (!strcmp(w, "transparent")) { *out = 0; *al = 0; return true; }
    if (!strcmp(w, "currentcolor")) { *out = cur; *al = 255; return true; }
    for (const char* p = CSS_COLORS; *p; ) {
        const char* e = strchr(p, ' ');
        if ((int)(e - p) == k && !strncmp(p, w, (size_t)k)) {
            uint32_t c = 0;
            for (int i = 1; i <= 6; i++) c = c * 16 + (uint32_t)hexval(e[i]);
            *out = c; *al = 255;
            return true;
        }
        p = e + 8;
    }
    return false;
}

/* gradients: just the average of the stops */
static bool grad_color(const char* v, uint32_t* out) {
    int r = 0, g = 0, b = 0, k = 0;
    for (const char* p = v; *p; ) {
        uint32_t c;
        uint8_t a;
        const char* e;
        bool st = p == v || !(isal(p[-1]) || p[-1] == '-' || (p[-1] >= '0' && p[-1] <= '9'));
        if (st && (*p == '#' || isal(*p)) && pcolor(p, 0, &c, &a, &e) && e > p) {
            if (a > 40) { r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; k++; }
            p = e;
            continue;
        }
        p++;
    }
    if (!k) return false;
    *out = RGB(r / k, g / k, b / k);
    return true;
}

/* ---------- computed style ---------- */
enum { D_NONE, D_INLINE, D_BLOCK, D_IBLOCK, D_LI, D_FLEX, D_IFLEX, D_GRID, D_IGRID, D_TABLE, D_ITABLE,
       D_ROW, D_CELL, D_RGROUP, D_CAPTION, D_CONTENTS };
enum { LS_NONE, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DEC, LS_LALPHA, LS_UALPHA, LS_LROMAN, LS_UROMAN, LS_OPEN };
enum { TD_UL = 1, TD_STRIKE = 2, TD_OVER = 4 };

typedef struct {
    /* inherited */
    uint32_t fg, pbg;
    int fs, lh, indent, ls, wsp, bsp;
    uint8_t bold, italic, mono, align, ws, tt, vis, lst, cblk, deco, opa;
    /* the rest */
    uint8_t disp, pos, flt, clr, ovf, bbox, va, odeco, fdir, fwrap, jc, ai, mauto, offs, bga, fga, hclip;
    uint8_t gspan, ntrk, bcset, ownopa, bfc, fixh, bgsz, bgrep, masked;
    int bgimg;
    uint32_t garea;
    int16_t gcs, gce, grs, gre;     /* grid lines, 0 = auto; gre < 0 with gspan style spans */
    const char* gta;                /* grid-template-areas, in gta_pool */
    uint8_t bs[4];
    int m[4], p[4], bw[4];
    uint32_t bc[4], bg;
    int rad, w, h, minw, maxw, minh;
    int off[4];
    int gap, rgap, grow, basis, cbw, trkmin;
    int trk[12];
    uint8_t trkfr[12];
    int cntl;
    char cnt[80];
} style_t;

static void style_reset(style_t* S) {
    S->disp = D_INLINE;
    S->pos = S->flt = S->clr = S->ovf = S->bbox = S->va = S->odeco = 0;
    S->fdir = S->fwrap = S->jc = S->ai = S->mauto = S->offs = S->bga = S->hclip = 0;
    S->gspan = S->ntrk = S->bcset = S->bfc = S->fixh = 0;
    S->fga = S->ownopa = 255;
    memset(S->bs, 0, sizeof S->bs);
    memset(S->m, 0, sizeof S->m);
    memset(S->p, 0, sizeof S->p);
    memset(S->bw, 0, sizeof S->bw);
    memset(S->off, 0, sizeof S->off);
    S->bg = 0;
    S->rad = 0;
    S->w = S->h = S->maxw = LA;
    S->minw = S->minh = 0;
    S->gap = S->rgap = S->grow = 0;
    S->basis = LA;
    S->trkmin = 0;
    S->cntl = -1;
    S->bgimg = -1;
    S->bgsz = S->bgrep = 0;
    S->garea = 0;
    S->masked = 0;
    S->gcs = S->gce = S->grs = S->gre = 0;
    S->gta = NULL;
}

/* custom properties: a stack, children see what their ancestors pushed */
#define MAX_VARS 4096
static struct { uint32_t h; const char* v; int vl; } vars[MAX_VARS];
static int n_vars;

static void var_push(const char* n, int nl, const char* v, int vl) {
    if (n_vars >= MAX_VARS) return;
    vars[n_vars].h = hsh(n, nl);
    vars[n_vars].v = v;
    vars[n_vars].vl = vl;
    n_vars++;
}

static int var_sub(const char* s, int n, char* o, int cap, int dep) {
    int k = 0;
    for (int i = 0; i < n; ) {
        if (i + 4 < n && lower(s[i]) == 'v' && lower(s[i + 1]) == 'a' && lower(s[i + 2]) == 'r' && s[i + 3] == '(') {
            int j = i + 4;
            while (j < n && isws(s[j])) j++;
            int n0 = j;
            while (j < n && s[j] != ',' && s[j] != ')' && !isws(s[j])) j++;
            uint32_t h = hsh(s + n0, j - n0);
            int d = 1, fb = -1, end = n;
            for (int q = j; q < n; q++) {
                if (s[q] == '(') d++;
                else if (s[q] == ')') { if (--d == 0) { end = q; break; } }
                else if (s[q] == ',' && d == 1 && fb < 0) fb = q + 1;
            }
            int vi = -1;
            for (int q = n_vars - 1; q >= 0; q--) if (vars[q].h == h) { vi = q; break; }
            int r = -1;
            if (dep < 6 && vi >= 0) r = var_sub(vars[vi].v, vars[vi].vl, o + k, cap - k, dep + 1);
            else if (dep < 6 && fb >= 0) r = var_sub(s + fb, end - fb, o + k, cap - k, dep + 1);
            if (r < 0) return -1;
            k += r;
            i = end + 1;
            continue;
        }
        if (k >= cap - 1) break;
        char c = s[i++];
        o[k++] = (c == '\n' || c == '\t' || c == '\r') ? ' ' : c;
    }
    o[k] = 0;
    return k;
}

static int bstyle(const char* v) {
    if (starts_ci(v, "none") || starts_ci(v, "hidden")) return 0;
    if (starts_ci(v, "dashed")) return 2;
    if (starts_ci(v, "dotted")) return 3;
    if (starts_ci(v, "solid") || starts_ci(v, "double") || starts_ci(v, "groove") || starts_ci(v, "ridge") ||
        starts_ci(v, "inset") || starts_ci(v, "outset")) return 1;
    return -1;
}
static bool bwidth(const char* v, int fs, int* w, const char** e) {
    v = skipws(v);
    if (starts_ci(v, "thin")) { *w = 1; *e = v + 4; return true; }
    if (starts_ci(v, "medium")) { *w = 3; *e = v + 6; return true; }
    if (starts_ci(v, "thick")) { *w = 5; *e = v + 5; return true; }
    if (!plen(v, fs, 0, w, e) || *w == LA) return false;
    if (*w < 0) *w = 0;
    if (*w > 0 && *w < 1) *w = 1;
    return true;
}

/* "1px solid red" onto the sides in mask */
static void border_sh(style_t* S, const char* v, int mask) {
    int w = 3, st = 0;
    uint32_t c = S->fg;
    uint8_t a = 255;
    bool hc = false;
    for (const char* p = skipws(v); *p; p = skipws(p)) {
        const char* e;
        int x = bstyle(p);
        if (x >= 0) { st = x; while (*p && *p != ' ') p++; continue; }
        if (bwidth(p, S->fs, &w, &e)) { p = e; continue; }
        if (pcolor(p, S->fg, &c, &a, &e) && e > p) { hc = a > 0; if (!a) st = st ? st : st; p = e; continue; }
        while (*p && *p != ' ') p++;
    }
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) continue;
        S->bw[i] = w; S->bs[i] = (uint8_t)st;
        S->bc[i] = c;
        if (hc || a == 0) S->bcset |= (uint8_t)(1 << i); else S->bcset &= (uint8_t)~(1 << i);
        if (a == 0) S->bw[i] = 0;
    }
}

/* 1..4 values -> top right bottom left */
static int four(const char* v, int* out, int fs, int pct, uint8_t* au) {
    int x[4], n = 0;
    const char* p = skipws(v);
    while (*p && n < 4) {
        const char* e;
        if (!plen(p, fs, pct, &x[n], &e)) return 0;
        n++;
        p = skipws(e);
    }
    if (!n) return 0;
    int t = x[0], r = n > 1 ? x[1] : t, b = n > 2 ? x[2] : t, l = n > 3 ? x[3] : r;
    out[0] = t; out[1] = r; out[2] = b; out[3] = l;
    if (au) {
        *au = 0;
        for (int i = 0; i < 4; i++) if (out[i] == LA) { *au |= (uint8_t)(1 << i); out[i] = 0; }
    }
    return n;
}

static int disp_kw(const char* v) {
    if (starts_ci(v, "none")) return D_NONE;
    if (starts_ci(v, "inline-block") || starts_ci(v, "-webkit-inline-box")) return D_IBLOCK;
    if (starts_ci(v, "inline-flex") || starts_ci(v, "inline flex")) return D_IFLEX;
    if (starts_ci(v, "inline-grid") || starts_ci(v, "inline grid")) return D_IGRID;
    if (starts_ci(v, "inline-table")) return D_ITABLE;
    if (starts_ci(v, "inline")) return D_INLINE;
    if (starts_ci(v, "list-item")) return D_LI;
    if (starts_ci(v, "flex") || starts_ci(v, "block flex")) return D_FLEX;
    if (starts_ci(v, "grid") || starts_ci(v, "block grid")) return D_GRID;
    if (starts_ci(v, "table-row-group") || starts_ci(v, "table-header-group") || starts_ci(v, "table-footer-group")) return D_RGROUP;
    if (starts_ci(v, "table-row")) return D_ROW;
    if (starts_ci(v, "table-cell")) return D_CELL;
    if (starts_ci(v, "table-caption")) return D_CAPTION;
    if (starts_ci(v, "table-column")) return D_NONE;
    if (starts_ci(v, "table")) return D_TABLE;
    if (starts_ci(v, "contents")) return D_CONTENTS;
    if (starts_ci(v, "block") || starts_ci(v, "flow-root") || starts_ci(v, "run-in") || starts_ci(v, "-webkit-box") ||
        starts_ci(v, "flow")) return D_BLOCK;
    return -1;
}

static int fsize_kw(const char* v, int pfs) {
    static const struct { const char* n; int px; } ks[] = {
        {"xx-small",9},{"x-small",10},{"small",13},{"medium",16},{"large",18},{"x-large",24},{"xx-large",32},{"xxx-large",48} };
    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++)
        if (starts_ci(v, ks[i].n) && !isal(v[strlen(ks[i].n)]) && v[strlen(ks[i].n)] != '-') return ks[i].px;
    if (starts_ci(v, "smaller")) return pfs * 5 / 6;
    if (starts_ci(v, "larger")) return pfs * 6 / 5;
    return -1;
}

static void set_fs(style_t* S, const style_t* P, const char* v) {
    int x = fsize_kw(v, P->fs);
    if (x < 0 && (!plen(v, P->fs, P->fs, &x, NULL) || x == LA)) return;
    if (x < 6) x = 6;
    if (x > 120) x = 120;
    S->fs = x;
}

static void set_lh(style_t* S, const char* v) {
    if (starts_ci(v, "normal")) { S->lh = 0; return; }
    int m;
    const char* e = pnum(v, &m);
    if (e && !isal(*e) && *e != '%') { S->lh = S->fs * m / 1000; return; }
    int x;
    if (plen(v, S->fs, S->fs, &x, NULL) && x != LA) S->lh = x < 0 ? 0 : x;
}

static void set_ff(style_t* S, const char* v) {
    static char b[120];
    int k = 0;
    for (; v[k] && k < 119; k++) b[k] = lower(v[k]);
    b[k] = 0;
    S->mono = strstr(b, "mono") || strstr(b, "courier") || strstr(b, "consolas") || strstr(b, "menlo") ||
              strstr(b, "fixed") || strstr(b, "lucida console");
}

static void set_font(style_t* S, const style_t* P, const char* v) {
    if (starts_ci(v, "caption") || starts_ci(v, "menu") || starts_ci(v, "icon") || starts_ci(v, "message-box") ||
        starts_ci(v, "status-bar") || starts_ci(v, "small-caption")) return;
    S->bold = 0; S->italic = 0; S->lh = 0;
    const char* p = skipws(v);
    while (*p) {
        const char* e = p;
        while (*e && *e != ' ' && *e != '/') e++;
        int m;
        if (starts_ci(p, "italic") || starts_ci(p, "oblique")) S->italic = 1;
        else if (starts_ci(p, "bold") || starts_ci(p, "bolder")) S->bold = 1;
        else if (starts_ci(p, "normal") || starts_ci(p, "small-caps") || starts_ci(p, "lighter") || starts_ci(p, "light")) ;
        else if (pnum(p, &m) && pnum(p, &m) == e && m >= 100000) S->bold = m >= 600000;
        else {
            set_fs(S, P, p);
            p = e;
            if (*p == '/') { set_lh(S, p + 1); while (*p && *p != ' ') p++; }
            set_ff(S, skipws(p));
            return;
        }
        p = skipws(e);
    }
}

/* grid-template-columns */
static void set_tracks(style_t* S, const char* v) {
    S->ntrk = 0;
    S->trkmin = 0;
    const char* p = skipws(v);
    while (*p && S->ntrk < 12) {
        if (*p == '[') { while (*p && *p != ']') p++; if (*p) p++; p = skipws(p); continue; }
        if (starts_ci(p, "repeat(")) {
            p += 7;
            int cnt = 0;
            bool autof = false;
            if (starts_ci(p, "auto-fill") || starts_ci(p, "auto-fit")) { autof = true; }
            else cnt = atoi(p);
            while (*p && *p != ',') p++;
            if (*p) p++;
            p = skipws(p);
            if (autof) {
                /* repeat(auto-fill, minmax(200px, 1fr)): count is decided at layout */
                int x = 0;
                const char* q = starts_ci(p, "minmax(") ? p + 7 : p;
                if (plen(q, S->fs, S->cbw, &x, NULL) && x != LA) S->trkmin = x > 20 ? x : 20;
                else S->trkmin = 200;
                return;
            }
            const char* rs = p;
            int d = 1;
            while (*p && d) { if (*p == '(') d++; else if (*p == ')') d--; if (d) p++; }
            static char one[160];
            int l = (int)(p - rs) < 159 ? (int)(p - rs) : 159;
            memcpy(one, rs, (size_t)l);
            one[l] = 0;
            if (*p) p++;
            style_t t = *S;
            t.ntrk = 0;
            set_tracks(&t, one);
            for (int r = 0; r < cnt && S->ntrk < 12; r++)
                for (int i = 0; i < t.ntrk && S->ntrk < 12; i++) { S->trk[S->ntrk] = t.trk[i]; S->trkfr[S->ntrk++] = t.trkfr[i]; }
            p = skipws(p);
            continue;
        }
        const char* q = p;
        bool mm = starts_ci(p, "minmax(");
        if (mm) {                                            /* minmax(a, b): the max side decides */
            const char* c = strchr(p, ',');
            q = c ? skipws(c + 1) : p + 7;
        } else if (starts_ci(p, "fit-content(")) q = p + 12;
        int m, x;
        const char* e = pnum(q, &m);
        int i = S->ntrk;
        if (e && starts_ci(e, "fr")) { S->trk[i] = m / 10; S->trkfr[i] = 1; }
        else if (starts_ci(q, "auto") || starts_ci(q, "min-content") || starts_ci(q, "max-content")) { S->trk[i] = 0; S->trkfr[i] = 2; }
        else if (plen(q, S->fs, S->cbw, &x, NULL) && x != LA) { S->trk[i] = x; S->trkfr[i] = mm ? 3 : 0; }   /* 3: up to x */
        else { S->trk[i] = 0; S->trkfr[i] = 2; }
        S->ntrk++;
        if (mm || q != p) { int d = 0; while (*p) { if (*p == '(') d++; else if (*p == ')' && --d == 0) { p++; break; } p++; } }
        else while (*p && *p != ' ') p++;
        p = skipws(p);
    }
}

/* grid-template-areas strings live here for one layout */
static char gta_pool[16384];
static int  gta_used;

static const char* gta_keep(const char* v) {
    int n = (int)strlen(v);
    for (int i = 0; i < gta_used; i += (int)strlen(gta_pool + i) + 1)
        if (!strcmp(gta_pool + i, v)) return gta_pool + i;
    if (gta_used + n + 1 > (int)sizeof gta_pool) return NULL;
    char* d = gta_pool + gta_used;
    memcpy(d, v, (size_t)n + 1);
    gta_used += n + 1;
    return d;
}

/* "2", "span 3", "-1", "name" (lines by name: no) */
static void grid_line(const char* v, int16_t* a, int16_t* b, uint8_t* span) {
    v = skipws(v);
    const char* sl = strchr(v, '/');
    if (starts_ci(v, "span")) { *span = (uint8_t)atoi(v + 4); *a = 0; }
    else if ((*v >= '0' && *v <= '9') || *v == '-') *a = (int16_t)atoi(v);
    if (!sl) return;
    sl = skipws(sl + 1);
    if (starts_ci(sl, "span")) { int s = atoi(sl + 4); if (*a > 0) *b = (int16_t)(*a + s); else *span = (uint8_t)s; }
    else if ((*sl >= '0' && *sl <= '9') || *sl == '-') *b = (int16_t)atoi(sl);
}

static void apply(style_t* S, const style_t* P, int pr, const char* v0, int vl) {
    static char vb[600];
    if (var_sub(v0, vl, vb, sizeof vb, 0) < 0) return;
    const char* v = skipws(vb);
    int fs = S->fs, cb = S->cbw, x, i;
    uint32_t c;
    uint8_t a;
    const char* e;
    if (starts_ci(v, "inherit")) {
        switch (pr) {
        case P_COLOR: S->fg = P->fg; break;
        case P_BG: case P_BGC: S->bg = P->bg; S->bga = P->bga; break;
        case P_FS: S->fs = P->fs; break;
        case P_FW: S->bold = P->bold; break;
        case P_TA: S->align = P->align; break;
        case P_DISPLAY: S->disp = P->disp; break;
        case P_VA: S->va = P->va; break;
        case P_W: S->w = P->w; break;
        case P_H: S->h = P->h; break;
        }
        return;
    }
    if (starts_ci(v, "initial") || starts_ci(v, "unset") || starts_ci(v, "revert")) {
        if (pr == P_COLOR) S->fg = COL_TEXT;
        if (pr == P_BG || pr == P_BGC) S->bga = 0;
        return;
    }
    switch (pr) {
    case P_COLOR: if (pcolor(v, P->fg, &c, &a, NULL)) { S->fg = c; S->fga = a; } break;
    case P_BGC: if (pcolor(v, S->fg, &c, &a, NULL)) { S->bg = c; S->bga = a; } break;
    case P_BG:
    case P_BGI:
        if (pr == P_BG) { S->bga = 0; S->bgimg = -1; S->bgsz = 0; S->bgrep = 0; }
        for (const char* p = v; *p; ) {
            p = skipws(p);
            if (!*p) break;
            if (starts_ci(p, "linear-gradient") || starts_ci(p, "radial-gradient") || starts_ci(p, "repeating-") ||
                starts_ci(p, "conic-gradient") || starts_ci(p, "-webkit-linear")) {
                if (grad_color(p, &c)) { S->bg = c; S->bga = 255; }
            }
            if (starts_ci(p, "url(")) S->bgimg = img_css(p);
            else if (starts_ci(p, "none")) S->bgimg = -1;
            else if (starts_ci(p, "no-repeat")) S->bgrep = 1;
            else if (starts_ci(p, "cover")) S->bgsz = 1;
            else if (starts_ci(p, "contain")) S->bgsz = 2;
            else if (starts_ci(p, "center")) S->bgrep |= 2;
            else if (pr == P_BG && pcolor(p, S->fg, &c, &a, &e) && e > p) { S->bg = c; S->bga = a; p = e; continue; }
            int d = 0;
            while (*p && (d || *p != ' ')) { if (*p == '(') d++; else if (*p == ')') d--; p++; }
        }
        break;
    case P_BGSIZE: S->bgsz = starts_ci(v, "cover") ? 1 : starts_ci(v, "contain") ? 2 : strstr(v, "100%") ? 1 : 0; break;
    case P_BGREP: S->bgrep = (uint8_t)((S->bgrep & 2) | (starts_ci(v, "no-repeat") ? 1 : 0)); break;
    case P_DISPLAY: x = disp_kw(v); if (x >= 0) S->disp = (uint8_t)x; if (starts_ci(v, "flow-root")) S->bfc = 1; break;
    case P_VIS: S->vis = starts_ci(v, "hidden") || starts_ci(v, "collapse"); break;
    case P_FONT: set_font(S, P, v); break;
    case P_FS: set_fs(S, P, v); break;
    case P_FW:
        if (starts_ci(v, "bold") || starts_ci(v, "bolder")) S->bold = 1;
        else if (starts_ci(v, "normal") || starts_ci(v, "lighter")) S->bold = 0;
        else if (v[0] >= '0' && v[0] <= '9') S->bold = atoi(v) >= 600;
        break;
    case P_FST: S->italic = starts_ci(v, "italic") || starts_ci(v, "oblique"); break;
    case P_FF: set_ff(S, v); break;
    case P_LH: set_lh(S, v); break;
    case P_LS: case P_WSP:
        x = 0;
        if (!starts_ci(v, "normal") && (!plen(v, fs, fs, &x, NULL) || x == LA)) break;
        if (x > 40) x = 40;
        if (x < -4) x = -4;
        if (pr == P_LS) S->ls = x; else S->wsp = x;
        break;
    case P_TA:
        S->cblk = 0;
        if (starts_ci(v, "center")) S->align = 1;
        else if (starts_ci(v, "right") || starts_ci(v, "end")) S->align = 2;
        else if (starts_ci(v, "justify")) S->align = 3;
        else if (starts_ci(v, "-webkit-center") || starts_ci(v, "-moz-center")) { S->align = 1; S->cblk = 1; }
        else S->align = 0;
        break;
    case P_TI: if (plen(v, fs, cb, &x, NULL) && x != LA) S->indent = x; break;
    case P_TT:
        S->tt = starts_ci(v, "uppercase") ? 1 : starts_ci(v, "lowercase") ? 2 : starts_ci(v, "capitalize") ? 3 : 0;
        break;
    case P_TD: case P_TDL:
        S->odeco = 0;
        if (strstr(v, "underline")) S->odeco |= TD_UL;
        if (strstr(v, "line-through")) S->odeco |= TD_STRIKE;
        if (strstr(v, "overline")) S->odeco |= TD_OVER;
        break;
    case P_WS:
        S->ws = starts_ci(v, "nowrap") ? 1 : starts_ci(v, "pre-wrap") || starts_ci(v, "break-spaces") ? 3 :
                starts_ci(v, "pre-line") ? 4 : starts_ci(v, "pre") ? 2 : 0;
        break;
    case P_VA:
        S->va = starts_ci(v, "middle") ? 1 : starts_ci(v, "top") || starts_ci(v, "text-top") ? 2 :
                starts_ci(v, "bottom") || starts_ci(v, "text-bottom") ? 3 : starts_ci(v, "sub") ? 4 :
                starts_ci(v, "super") ? 5 : 0;
        break;
    case P_LIST: case P_LST: {
        static const char* const kw[] = { "none", "disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha",
                                          "lower-roman", "upper-roman", "disclosure", 0 };
        for (const char* p = v; *p; p = skipws(p)) {
            for (i = 0; kw[i]; i++) if (starts_ci(p, kw[i])) { S->lst = (uint8_t)i; break; }
            if (starts_ci(p, "lower-latin")) S->lst = LS_LALPHA;
            if (starts_ci(p, "upper-latin")) S->lst = LS_UALPHA;
            if (starts_ci(p, "decimal-leading")) S->lst = LS_DEC;
            if (*p == '"' || *p == '\'') S->lst = LS_NONE;          /* string markers: skip */
            while (*p && *p != ' ') p++;
        }
        break;
    }
    case P_M: four(v, S->m, fs, cb, &S->mauto); break;
    case P_P: four(v, S->p, fs, cb, NULL); for (i = 0; i < 4; i++) if (S->p[i] < 0) S->p[i] = 0; break;
    case P_MT: case P_MR: case P_MB: case P_ML: case P_MIS: case P_MIE:
        i = pr == P_MIS ? 3 : pr == P_MIE ? 1 : pr - P_MT;
        if (plen(v, fs, cb, &x, NULL)) {
            if (x == LA) { S->mauto |= (uint8_t)(1 << i); x = 0; } else S->mauto &= (uint8_t)~(1 << i);
            S->m[i] = x;
        }
        break;
    case P_MI: case P_MBL: case P_PI: case P_PBL: {
        int t[4];
        uint8_t au = 0;
        if (four(v, t, fs, cb, &au)) {
            /* two values: start end */
            int* d = (pr == P_MI || pr == P_MBL) ? S->m : S->p;
            int s0 = (pr == P_MI || pr == P_PI) ? 3 : 0, s1 = s0 == 3 ? 1 : 2;
            d[s0] = t[0]; d[s1] = t[1];
            if (pr == P_MI) { S->mauto = (uint8_t)((S->mauto & ~10) | ((au & 1) ? 8 : 0) | ((au & 2) ? 2 : 0)); }
        }
        break;
    }
    case P_PT: case P_PR: case P_PB: case P_PL: case P_PIS: case P_PIE:
        i = pr == P_PIS ? 3 : pr == P_PIE ? 1 : pr - P_PT;
        if (plen(v, fs, cb, &x, NULL) && x != LA) S->p[i] = x < 0 ? 0 : x;
        break;
    case P_B: border_sh(S, v, 15); break;
    case P_BT: case P_BR: case P_BB: case P_BL: border_sh(S, v, 1 << (pr - P_BT)); break;
    case P_BW: {
        int t[4], n = 0;
        for (const char* p = skipws(v); *p && n < 4; p = skipws(p)) { if (!bwidth(p, fs, &t[n], &e)) break; n++; p = e; }
        if (!n) break;
        int tt[4] = { t[0], n > 1 ? t[1] : t[0], n > 2 ? t[2] : t[0], n > 3 ? t[3] : (n > 1 ? t[1] : t[0]) };
        for (i = 0; i < 4; i++) S->bw[i] = tt[i];
        break;
    }
    case P_BC: {
        uint32_t t[4];
        int n = 0;
        uint8_t al[4];
        for (const char* p = skipws(v); *p && n < 4; p = skipws(p)) { if (!pcolor(p, S->fg, &t[n], &al[n], &e) || e == p) break; n++; p = e; }
        if (!n) break;
        for (i = 0; i < 4; i++) {
            int k = i < n ? i : (i == 3 && n > 1) ? 1 : 0;
            S->bc[i] = t[k];
            S->bcset |= (uint8_t)(1 << i);
            if (!al[k]) S->bs[i] = 0;
        }
        break;
    }
    case P_BS: {
        int t[4], n = 0;
        for (const char* p = skipws(v); *p && n < 4; p = skipws(p)) { t[n] = bstyle(p); if (t[n] < 0) break; n++; while (*p && *p != ' ') p++; }
        if (!n) break;
        for (i = 0; i < 4; i++) S->bs[i] = (uint8_t)t[i < n ? i : (i == 3 && n > 1) ? 1 : 0];
        break;
    }
    case P_BTW: case P_BRW: case P_BBW: case P_BLW: if (bwidth(v, fs, &x, &e)) S->bw[pr - P_BTW] = x; break;
    case P_BTC: case P_BRC: case P_BBC: case P_BLC:
        if (pcolor(v, S->fg, &c, &a, NULL)) { i = pr - P_BTC; S->bc[i] = c; S->bcset |= (uint8_t)(1 << i); if (!a) S->bs[i] = 0; }
        break;
    case P_BTS: case P_BRS: case P_BBS: case P_BLS: x = bstyle(v); if (x >= 0) S->bs[pr - P_BTS] = (uint8_t)x; break;
    case P_RAD: if (plen(v, fs, 400, &x, NULL) && x != LA) S->rad = x; break;
    case P_W: case P_MINW: case P_MAXW:
        if (starts_ci(v, "none") || starts_ci(v, "max-content") || starts_ci(v, "min-content") || starts_ci(v, "fit-content")) x = LA;
        else if (!plen(v, fs, cb, &x, NULL)) break;
        if (x != LA && x < 0) x = 0;
        if (pr == P_W) S->w = x; else if (pr == P_MAXW) S->maxw = x; else S->minw = x == LA ? 0 : x;
        if (pr == P_W && x != LA && strchr(v, '%') && !strchr(v, '(')) S->fixh |= 2;     /* percent width */
        break;
    case P_H: case P_MINH:
        if (strchr(v, '%') && !strstr(v, "calc")) break;          /* % of an unknown height */
        if (!plen(v, fs, 0, &x, NULL)) break;
        if (x != LA && x < 0) x = 0;
        if (pr == P_H) S->h = x; else S->minh = x == LA ? 0 : x;
        break;
    case P_BOX: S->bbox = starts_ci(v, "border-box"); break;
    case P_OVF: case P_OVFX: case P_OVFY:
        if (starts_ci(v, "hidden") || starts_ci(v, "clip")) S->ovf = 1;
        else if ((starts_ci(v, "auto") || starts_ci(v, "scroll")) && S->ovf != 1) S->ovf = 2;
        break;
    case P_FLOAT: S->flt = starts_ci(v, "left") || starts_ci(v, "inline-start") ? 1 : starts_ci(v, "right") || starts_ci(v, "inline-end") ? 2 : 0; break;
    case P_CLEAR: S->clr = starts_ci(v, "both") ? 3 : starts_ci(v, "left") ? 1 : starts_ci(v, "right") ? 2 : 0; break;
    case P_POS:
        S->pos = starts_ci(v, "relative") || starts_ci(v, "sticky") || starts_ci(v, "-webkit-sticky") ? 1 :
                 starts_ci(v, "absolute") ? 2 : starts_ci(v, "fixed") ? 3 : 0;
        break;
    case P_TOP: case P_RIGHT: case P_BOTTOM: case P_LEFT:
        i = pr - P_TOP;
        if (plen(v, fs, (i & 1) ? cb : g_vh, &x, NULL)) {
            if (x == LA) S->offs &= (uint8_t)~(1 << i); else { S->off[i] = x; S->offs |= (uint8_t)(1 << i); }
        }
        break;
    case P_INSET: {
        int t[4];
        uint8_t au = 0;
        if (four(v, t, fs, cb, &au)) for (i = 0; i < 4; i++) {
            if (au & (1 << i)) S->offs &= (uint8_t)~(1 << i); else { S->off[i] = t[i]; S->offs |= (uint8_t)(1 << i); }
        }
        break;
    }
    case P_OPA: {
        int m;
        e = pnum(v, &m);
        if (!e) break;
        if (*e == '%') m /= 100;
        if (m < 0) m = 0;
        if (m > 1000) m = 1000;
        S->ownopa = (uint8_t)(m * 255 / 1000);
        break;
    }
    case P_CONTENT: {
        if (starts_ci(v, "none") || starts_ci(v, "normal")) { S->cntl = -1; break; }
        int k = 0;
        for (const char* p = v; *p; ) {
            p = skipws(p);
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) {
                    if (*p == '\\' && p[1]) {
                        p++;
                        if (hexval(*p) >= 0) {
                            uint32_t cp = 0;
                            int d = 0;
                            while (d < 6 && hexval(*p) >= 0) { cp = cp * 16 + (uint32_t)hexval(*p); p++; d++; }
                            if (*p == ' ') p++;
                            uint8_t o[3];
                            int m = cp == 0xA ? 0 : uni_to_cp866(cp, o);
                            for (int j = 0; j < m && k < 78; j++) S->cnt[k++] = (char)o[j];
                            continue;
                        }
                    }
                    if (k < 78) S->cnt[k++] = *p;
                    p++;
                }
                if (*p) p++;
            } else if (starts_ci(p, "open-quote") || starts_ci(p, "close-quote")) {
                if (k < 78) S->cnt[k++] = '"';
                p += 10;
                if (*p == 'e') p++;
            } else {
                while (*p && *p != ' ') p++;                    /* attr(), counter(), url(): not done */
            }
        }
        S->cnt[k] = 0;
        S->cntl = k;
        break;
    }
    case P_FDIR: S->fdir = starts_ci(v, "column"); break;
    case P_FWRAP: S->fwrap = starts_ci(v, "wrap"); break;
    case P_FFLOW: S->fdir = strstr(v, "column") != NULL; S->fwrap = strstr(v, "wrap") && !strstr(v, "nowrap"); break;
    case P_JC:
        S->jc = strstr(v, "center") ? 1 : strstr(v, "end") || strstr(v, "right") ? 2 : strstr(v, "between") ? 3 :
                strstr(v, "around") ? 4 : strstr(v, "evenly") ? 5 : 0;
        break;
    case P_AI: case P_PLACE:
        S->ai = strstr(v, "center") ? 2 : strstr(v, "end") ? 3 : strstr(v, "start") || strstr(v, "baseline") ? 1 : 0;
        break;
    case P_FLEX:
        if (starts_ci(v, "none")) { S->grow = 0; S->basis = LA; break; }
        if (starts_ci(v, "auto")) { S->grow = 100; S->basis = LA; break; }
        {
            int m;
            e = pnum(v, &m);
            if (e && !isal(*e) && *e != '%') {
                S->grow = m / 10;
                S->basis = 0;
                const char* p = skipws(e);
                if (pnum(p, &m) && !isal(*pnum(p, &m)) && *pnum(p, &m) != '%') p = skipws(pnum(p, &m));
                if (*p && plen(p, fs, cb, &x, NULL)) S->basis = x;
            } else if (plen(v, fs, cb, &x, NULL)) { S->grow = 100; S->basis = x; }
        }
        break;
    case P_FGROW: { int m; if (pnum(v, &m)) S->grow = m / 10; break; }
    case P_FBASIS: if (plen(v, fs, cb, &x, NULL)) S->basis = x; else S->basis = LA; break;
    case P_GAP: case P_CGAP: case P_RGAP:
        if (starts_ci(v, "normal")) x = 0;
        else if (!plen(v, fs, cb, &x, &e) || x == LA) break;
        if (x < 0) x = 0;
        if (pr != P_CGAP) S->rgap = x;
        if (pr != P_RGAP) S->gap = x;
        if (pr == P_GAP && plen(e, fs, cb, &x, NULL) && x != LA) S->gap = x;
        break;
    case P_GTC: if (!starts_ci(v, "none") && !starts_ci(v, "subgrid")) set_tracks(S, v); break;
    case P_GCOL: S->gcs = S->gce = 0; S->gspan = 0; grid_line(v, &S->gcs, &S->gce, &S->gspan); break;
    case P_GCS: { int16_t b = 0; uint8_t sp = 0; grid_line(v, &S->gcs, &b, &sp); if (sp) S->gspan = sp; break; }
    case P_GCE: { int16_t a = 0, b = 0; uint8_t sp = 0; grid_line(v, &a, &b, &sp); if (sp) S->gspan = sp; else S->gce = a; break; }
    case P_GROW: { uint8_t sp = 0; S->grs = S->gre = 0; grid_line(v, &S->grs, &S->gre, &sp); if (sp && S->grs > 0) S->gre = (int16_t)(S->grs + sp); break; }
    case P_GRS: { int16_t b = 0; uint8_t sp = 0; grid_line(v, &S->grs, &b, &sp); break; }
    case P_GRE: { int16_t a = 0, b = 0; uint8_t sp = 0; grid_line(v, &a, &b, &sp); S->gre = a; break; }
    case P_MASK: case P_WMASK: S->masked = !starts_ci(v, "none"); break;
    case P_GTA: S->gta = starts_ci(v, "none") ? NULL : gta_keep(v); break;
    case P_GT: {
        /* rows / columns, or with area strings in front of the rows */
        if (starts_ci(v, "none")) { S->ntrk = 0; S->gta = NULL; break; }
        const char* sl = NULL;
        for (const char* q = v; *q; q++) if (*q == '/') sl = q;
        if (sl) set_tracks(S, sl + 1);
        if (strchr(v, '"') || strchr(v, '\'')) {
            static char ar[400];
            int k = 0;
            for (const char* q = v; *q && (!sl || q < sl) && k < 398; q++) {
                if (*q != '"' && *q != '\'') continue;
                char qq = *q++;
                ar[k++] = '"';
                while (*q && *q != qq && k < 397) ar[k++] = *q++;
                ar[k++] = '"';
                ar[k++] = ' ';
            }
            ar[k] = 0;
            S->gta = gta_keep(ar);
        }
        break;
    }
    case P_GAREA: {
        const char* q = skipws(v);
        if ((*q >= '0' && *q <= '9') || *q == '-' || starts_ci(q, "span")) {   /* row-start / col-start / row-end / col-end */
            int x[4] = { 0, 0, 0, 0 }, k = 0;
            for (const char* r = q; *r && k < 4; ) { x[k++] = atoi(r); while (*r && *r != '/') r++; if (*r) r++; }
            S->grs = (int16_t)x[0]; S->gcs = (int16_t)x[1]; S->gre = (int16_t)x[2]; S->gce = (int16_t)x[3];
        } else if (!starts_ci(q, "auto")) {
            int k = 0;
            while (q[k] && q[k] != ' ' && q[k] != '/') k++;
            S->garea = hsh(q, k);
        }
        break;
    }
    case P_CLIP: if (starts_ci(v, "rect(")) { if (!strstr(v, "auto")) S->hclip = 1; } break;
    case P_CLIPPATH: if (strstr(v, "inset(50%") || strstr(v, "circle(0") || strstr(v, "inset(100%")) S->hclip = 1; break;
    case P_BSP: if (plen(v, fs, 0, &x, NULL) && x != LA) S->bsp = x; break;
    }
}

/* ---------- matching ---------- */
static bool comp_match(const comp_t* c, int n);

static bool acond_ok(const acond_t* A, int n) {
    static char v[512];
    if (!get_attr(node_attrs(n), A->n, v, sizeof v)) return false;
    if (!A->op) return true;
    int vl = (int)strlen(v), k = A->vl;
    const char* w = A->v;
#define EQ(a, b, l) (A->ci ? ({ bool ok_ = true; for (int q_ = 0; q_ < (l); q_++) if (lower((a)[q_]) != lower((b)[q_])) { ok_ = false; break; } ok_; }) : !memcmp(a, b, (size_t)(l)))
    switch (A->op) {
    case '=': return vl == k && EQ(v, w, k);
    case '^': return k && vl >= k && EQ(v, w, k);
    case '$': return k && vl >= k && EQ(v + vl - k, w, k);
    case '*': for (int i = 0; k && i + k <= vl; i++) if (EQ(v + i, w, k)) return true; return false;
    case '|': return (vl == k || (vl > k && v[k] == '-')) && EQ(v, w, k);
    case '~':
        for (int i = 0; i < vl; ) {
            while (i < vl && isws(v[i])) i++;
            int j = i;
            while (j < vl && !isws(v[j])) j++;
            if (j - i == k && k && EQ(v + i, w, k)) return true;
            i = j;
        }
        return false;
    }
#undef EQ
    return false;
}

static bool nth_ok(int pos, int a, int b) {
    if (!a) return pos == b;
    int d = pos - b;
    return d / a >= 0 && d % a == 0;
}

static bool comp_match(const comp_t* c, int n) {
    node_t* N = &nodes[n];
    if (c->tag && c->tag != N->th) return false;
    if (c->id && c->id != N->idh) return false;
    for (int i = 0; i < c->ncls; i++) {
        bool f = false;
        for (int k = 0; k < N->ncls; k++) if (clsh[N->cls + k] == c->cls[i]) { f = true; break; }
        if (!f) return false;
    }
    if (c->pf) {
        uint16_t pf = c->pf;
        if (pf & PF_NEVER) return false;
        if ((pf & PF_ROOT) && !(N->parent == 0 && !strcmp(N->tag, "html"))) return false;
        if ((pf & PF_FIRST) && elem_prev(n) >= 0) return false;
        if ((pf & PF_LAST) && elem_next(n) >= 0) return false;
        if ((pf & PF_NTH) && !nth_ok(N->idx + 1, c->na, c->nb)) return false;
        if (pf & PF_NTHL) {
            int k = 1;
            for (int p = elem_next(n); p >= 0; p = elem_next(p)) k++;
            if (!nth_ok(k, c->na, c->nb)) return false;
        }
        if (pf & (PF_FTYPE | PF_NTHT)) {
            int k = 1;
            for (int p = elem_prev(n); p >= 0; p = elem_prev(p)) if (nodes[p].th == N->th) k++;
            if ((pf & PF_FTYPE) && k != 1) return false;
            if ((pf & PF_NTHT) && !nth_ok(k, c->na, c->nb)) return false;
        }
        if (pf & PF_LTYPE)
            for (int p = elem_next(n); p >= 0; p = elem_next(p)) if (nodes[p].th == N->th) return false;
        if ((pf & PF_EMPTY) && N->first >= 0) return false;
        if ((pf & PF_LINK) && !((!strcmp(N->tag, "a") || !strcmp(N->tag, "area")) && get_attr(node_attrs(n), "href", NULL, 0))) return false;
        if ((pf & PF_CHECKED) && !get_attr(node_attrs(n), "checked", NULL, 0) && !get_attr(node_attrs(n), "selected", NULL, 0)) return false;
        if ((pf & PF_DISABLED) && !get_attr(node_attrs(n), "disabled", NULL, 0)) return false;
    }
    for (int i = 0; i < c->nattr; i++) if (!acond_ok(&aconds[c->attr + i], n)) return false;
    if (c->nsub) {
        bool any = false;
        for (int i = 0; i < c->nsub && !any; i++) any = comp_match(&comps[c->sub + i], n);
        if (c->subk == 1 ? any : !any) return false;
    }
    return true;
}

static bool chain_match(const rule_t* r, int k, int n) {
    const comp_t* c = &comps[r->comp + k];
    if (!comp_match(c, n)) return false;
    if (k == 0) return true;
    switch (c->comb) {
    case '>': { int p = elem_parent(n); return p >= 0 && chain_match(r, k - 1, p); }
    case '+': { int p = elem_prev(n); return p >= 0 && chain_match(r, k - 1, p); }
    case '~': for (int p = elem_prev(n); p >= 0; p = elem_prev(p)) if (chain_match(r, k - 1, p)) return true; return false;
    default: for (int p = elem_parent(n); p >= 0; p = elem_parent(p)) if (chain_match(r, k - 1, p)) return true; return false;
    }
}

#define MAX_MATCH 600
static int mt[MAX_MATCH], n_mt;
static int g_stamp;

static void mt_add_chain(int r, int n) {
    for (; r >= 0; r = rules[r].next) {
        if (rules[r].stamp == g_stamp) continue;
        rules[r].stamp = g_stamp;
        if (n_mt < MAX_MATCH && chain_match(&rules[r], rules[r].ncomp - 1, n)) mt[n_mt++] = r;
    }
}

/* matching rules of n in cascade order: UA first, then specificity, then source order */
static void collect(int n) {
    n_mt = 0;
    g_stamp++;
    node_t* N = &nodes[n];
    if (N->idh) mt_add_chain(bucket[N->idh & 1023], n);
    for (int k = 0; k < N->ncls; k++) mt_add_chain(bucket[clsh[N->cls + k] & 1023], n);
    mt_add_chain(bucket[N->th & 1023], n);
    mt_add_chain(ubucket, n);
    for (int i = 1; i < n_mt; i++) {
        int x = mt[i], j = i - 1;
        long long kx = (long long)(x >= n_ua) << 40 | (long long)rules[x].spec << 16 | x;
        while (j >= 0) {
            int y = mt[j];
            long long ky = (long long)(y >= n_ua) << 40 | (long long)rules[y].spec << 16 | y;
            if (ky <= kx) break;
            mt[j + 1] = y;
            j--;
        }
        mt[j + 1] = x;
    }
}

static bool pass_of(int p, int pass) {
    if (pass == 0) return p == P_VAR;
    if (pass == 1) return p == P_FS || p == P_FONT || p == P_COLOR;
    return p != P_VAR && p != P_FS && p != P_FONT && p != P_COLOR;
}

static void hints(style_t* S, const style_t* P, int n, int pass);

/* one pass over the matched rules for pseudo element `pel`, plus style="" */
static void run_pass(style_t* S, const style_t* P, int n, int pel, int pass, int i0, int ni) {
    for (int imp = 0; imp < 2; imp++) {
        bool hinted = imp || pel;
        for (int m = 0; m < n_mt; m++) {
            rule_t* r = &rules[mt[m]];
            if (r->pel != pel) continue;
            if (!hinted && mt[m] >= n_ua) { hints(S, P, n, pass); hinted = true; }
            for (int d = r->d0; d < r->d0 + r->nd; d++) {
                decl_t* D = &decls[d];
                if (D->imp != imp || !pass_of(D->p, pass)) continue;
                if (D->p == P_VAR) var_push(D->n, D->nl, D->v, D->vl);
                else apply(S, P, D->p, D->v, D->vl);
            }
        }
        if (!hinted) hints(S, P, n, pass);
        if (pel) continue;
        for (int d = i0; d < i0 + ni; d++) {
            decl_t* D = &decls[d];
            if (D->imp != imp || !pass_of(D->p, pass)) continue;
            if (D->p == P_VAR) var_push(D->n, D->nl, D->v, D->vl);
            else apply(S, P, D->p, D->v, D->vl);
        }
    }
}

static int find_table(int n) {
    for (int p = elem_parent(n), k = 0; p >= 0 && k < 4; p = elem_parent(p), k++) if (is_tag(p, "table")) return p;
    return -1;
}

/* old presentational attributes: bgcolor, width, align, <font> ... */
static void hints(style_t* S, const style_t* P, int n, int pass) {
    static char v[80];
    attrs_t A = node_attrs(n);
    const char* t = nodes[n].tag;
    uint32_t c;
    uint8_t a;
    int x;
    if (pass == 1) {
        if ((!strcmp(t, "font") && get_attr(A, "color", v, sizeof v)) || (!strcmp(t, "body") && get_attr(A, "text", v, sizeof v)))
            if (pcolor(v, S->fg, &c, &a, NULL)) S->fg = c;
        if (!strcmp(t, "font") && get_attr(A, "size", v, sizeof v)) {
            static const int sz[] = { 10, 10, 13, 16, 18, 24, 32, 48 };
            int k = atoi(v[0] == '+' || v[0] == '-' ? v + 1 : v);
            if (v[0] == '+') k = 3 + k; else if (v[0] == '-') k = 3 - k;
            if (k < 1) k = 1;
            if (k > 7) k = 7;
            S->fs = sz[k];
        }
        return;
    }
    if (pass != 2) return;
    if (!strcmp(t, "font") && get_attr(A, "face", v, sizeof v)) set_ff(S, v);
    if (get_attr(A, "bgcolor", v, sizeof v) && pcolor(v, S->fg, &c, &a, NULL)) { S->bg = c; S->bga = 255; }
    bool cell = !strcmp(t, "td") || !strcmp(t, "th");
    if (cell || !strcmp(t, "table") || !strcmp(t, "img") || !strcmp(t, "hr") || !strcmp(t, "col") || !strcmp(t, "iframe")) {
        if (get_attr(A, "width", v, sizeof v) && v[0] >= '0' && v[0] <= '9') {
            x = atoi(v);
            if (strchr(v, '%')) { S->w = S->cbw * x / 100; S->fixh |= 2; } else S->w = x;
        }
        if (get_attr(A, "height", v, sizeof v) && !strchr(v, '%')) S->h = atoi(v);
    }
    if (get_attr(A, "align", v, sizeof v)) {
        bool c_ = starts_ci(v, "center") || starts_ci(v, "middle"), r_ = starts_ci(v, "right"), l_ = starts_ci(v, "left");
        if (!strcmp(t, "table")) { if (c_) S->mauto |= 10; else if (r_) S->flt = 2; else if (l_ && 0) S->flt = 1; }
        else if (!strcmp(t, "img")) { if (r_) S->flt = 2; else if (l_) S->flt = 1; }
        else if (c_) { S->align = 1; S->cblk = 1; }
        else if (r_) S->align = 2;
        else if (l_) S->align = 0;
    }
    if (get_attr(A, "valign", v, sizeof v)) S->va = starts_ci(v, "top") ? 2 : starts_ci(v, "bottom") ? 3 : 1;
    if (get_attr(A, "nowrap", NULL, 0) && cell) S->ws = 1;
    if (!strcmp(t, "table")) {
        if (get_attr(A, "cellspacing", v, sizeof v)) S->bsp = atoi(v);
        if (get_attr(A, "border", v, sizeof v) && atoi(v) > 0)
            for (int i = 0; i < 4; i++) { S->bw[i] = 1; S->bs[i] = 1; S->bc[i] = RGB(0x80, 0x80, 0x80); S->bcset |= 15; }
    }
    if (cell) {
        int tb = find_table(n);
        if (tb >= 0) {
            attrs_t T = node_attrs(tb);
            if (get_attr(T, "cellpadding", v, sizeof v)) for (int i = 0; i < 4; i++) S->p[i] = atoi(v);
            if (get_attr(T, "border", v, sizeof v) && atoi(v) > 0)
                for (int i = 0; i < 4; i++) { S->bw[i] = 1; S->bs[i] = 1; S->bc[i] = RGB(0xA0, 0xA0, 0xA0); S->bcset |= 15; }
        }
    }
    (void)P;
}

static void finish_style(style_t* S, const style_t* P, int n) {
    for (int i = 0; i < 4; i++) {
        if (!S->bs[i]) S->bw[i] = 0;
        if (!(S->bcset & (1 << i))) S->bc[i] = S->fg;
        if (S->bw[i] > 40) S->bw[i] = 40;
    }
    S->deco = (uint8_t)(P->deco | S->odeco);
    S->opa = (uint8_t)(P->opa * S->ownopa / 255);
    if (S->fga < 255) S->fg = mix(P->pbg, S->fg, S->fga);
    if (S->bga) S->pbg = mix(P->pbg, S->bg, S->bga);
    if (S->opa < 255) {
        S->fg = mix(S->pbg, S->fg, S->opa);
        for (int i = 0; i < 4; i++) S->bc[i] = mix(S->pbg, S->bc[i], S->opa);
        S->bga = (uint8_t)(S->bga * S->opa / 255);
    }
    if (S->rad > 12) S->rad = 12;
    if (S->masked) { S->bga = 0; S->bgimg = -1; }         /* icon masks: the shape is the point, a square is worse */
    if ((S->flt || S->pos >= 2) && S->disp != D_NONE) {
        if (S->disp == D_INLINE || S->disp == D_IBLOCK || S->disp == D_CELL || S->disp == D_ROW) S->disp = D_BLOCK;
        else if (S->disp == D_IFLEX) S->disp = D_FLEX;
        else if (S->disp == D_IGRID) S->disp = D_GRID;
        else if (S->disp == D_ITABLE) S->disp = D_TABLE;
    }
    if (S->pos >= 2) S->flt = 0;
    /* <center> and align=center don't leak into table cells (quirks, but every browser does it) */
    if (n >= 0 && S->cblk && !strcmp(nodes[n].tag, "table")) { S->cblk = 0; S->align = 0; }
    /* links on dark pages: the UA blue is unreadable */
    if (n >= 0 && S->fg == RGB(0x1F, 0x5C, 0xB8) && lum(S->pbg) < 1100) S->fg = RGB(0x8A, 0xB4, 0xF8);
}

/* computes the style of element n; bs/as get ::before/::after when they have content */
static void compute(int n, const style_t* P, style_t* S, int cbw, style_t* bs, style_t* as) {
    *S = *P;
    style_reset(S);
    S->cbw = cbw;
    collect(n);
    int saved = n_decls, i0 = n_decls, ni = 0;
    attrs_t A = node_attrs(n);
    if (get_attr(A, "style", NULL, 0) && ga_v1 > ga_v0) {
        n_decls = MAX_DECLS - 200;                           /* scratch area at the end */
        i0 = n_decls;
        ni = css_decls(A.a + ga_v0, ga_v1 - ga_v0, MAX_DECLS);
        n_decls = saved;
    }
    for (int pass = 0; pass < 3; pass++) run_pass(S, P, n, 0, pass, i0, ni);
    n_decls = saved;
    finish_style(S, P, n);
    if (!bs) return;
    bs->cntl = as->cntl = -1;
    bool hb = false, ha = false;
    for (int m = 0; m < n_mt; m++) { if (rules[mt[m]].pel == 1) hb = true; if (rules[mt[m]].pel == 2) ha = true; }
    for (int k = 0; k < 2; k++) {
        style_t* X = k ? as : bs;
        if (!(k ? ha : hb)) continue;
        *X = *S;
        style_reset(X);
        X->cbw = cbw;
        for (int pass = 1; pass < 3; pass++) run_pass(X, S, n, k + 1, pass, 0, 0);
        finish_style(X, S, -1);
    }
}

/* ---------- layout state ---------- */
enum { RF_UL = 1, RF_STRIKE = 2, RF_OVER = 4, RF_ITALIC = 8, RF_HIDE = 16 };

typedef struct { int x, y, w, h; uint32_t bg, bc[4]; uint8_t bga, rad, bw[4], bs[4], bgsz, bgrep; int16_t clip, img; uint32_t pbg; int r_at; } bx_t;
#define MAX_BOXES 6000
static bx_t*  boxes;
static int    n_boxes;
typedef struct { int x, y, w, h, up; } clip_t;
#define MAX_CLIPS 512
static clip_t clips[MAX_CLIPS];
static int    n_clips;
typedef struct { int r0, r1, b0, b1, c0, c1, asc, h; uint8_t va; } atom_t;
#define MAX_ATOMS 3000
static atom_t atoms[MAX_ATOMS];
static int    n_atoms;
typedef struct { uint32_t bg, bc[4]; uint8_t bga, rad, bs[4]; int bw[4], pt, pb, pl, pr, asc, fh, fx, fr, clip; } ibox_t;
static ibox_t ibs[32];
static int    n_ibs;
typedef struct { ibox_t b; int x0, x1; } frag_t;
static frag_t frags[160];
static int    n_frags;
typedef struct { int x0, x1, y0, y1, side; } flt_t;
static flt_t  flts[64];
static int    n_flts;

typedef struct {
    int x0, x1, y;
    int lx, lx0, lx1, a0, f0, ib0;
    int mpos, mneg, pend, last_base;
    bool dirty, space, first;
    const style_t* bs;
    int link, form, clip, js;
    int mk_kind, mk_x, mk_f, mk_sc;
    uint32_t mk_fg;
    char mk_t[12];
} lay_t;

static lay_t L;
static bool  g_meas;
static int   g_maxx, g_minw, g_linum;
static int   cb_x, cb_y, cb_w;             /* containing block of absolute boxes */
static int   view_w_l;

static void pick_font(const style_t* S, int* f, int* sc) {
    int fs = S->fs;
    *sc = 1;
    if (S->mono) { *f = F_MONO; *sc = fs >= 44 ? 3 : fs >= 24 ? 2 : 1; return; }
    if (fs <= 13) *f = UIF_SMALL;
    else if (fs <= 17) *f = S->bold ? UIF_MED : UIF_REG;
    else if (fs <= 22) *f = S->bold ? UIF_BIG : UIF_REG;
    else if (fs <= 27) { *f = UIF_SMALL; *sc = 2; }
    else if (fs <= 35) { *f = S->bold ? UIF_MED : UIF_REG; *sc = 2; }
    else if (fs <= 50) { *f = UIF_BIG; *sc = 2; }
    else { *f = UIF_BIG; *sc = 3; }
}

static int word_w(int f, int sc, int ls, const char* s, int n) { return text_w(f, s, n) * sc + ls * n; }

static run_t* add_run(int kind, int x, int y, int w, int h, int font, uint32_t fg, int off, int len) {
    if (n_runs >= MAX_RUNS) return NULL;
    run_t* r = &runs[n_runs++];
    memset(r, 0, sizeof *r);
    r->kind = (uint8_t)kind; r->x = x; r->y = y; r->w = (uint16_t)(w < 0 ? 0 : w); r->h = (uint16_t)h;
    r->font = (uint8_t)font; r->fg = fg; r->link = (int16_t)L.link; r->field = -1;
    r->off = off; r->len = (uint16_t)len; r->scale = 1; r->clip = (int16_t)L.clip; r->js = L.js;
    return r;
}

static void add_atom(int r0, int b0, int c0, int asc, int h, int va) {
    if (n_atoms >= MAX_ATOMS) return;
    atom_t* a = &atoms[n_atoms++];
    a->r0 = r0; a->r1 = n_runs; a->b0 = b0; a->b1 = n_boxes; a->c0 = c0; a->c1 = n_clips;
    a->asc = asc; a->h = h; a->va = (uint8_t)va;
}

static void shift(int r0, int r1, int b0, int b1, int c0, int c1, int dx, int dy) {
    if (!dx && !dy) return;
    for (int i = r0; i < r1; i++) { runs[i].x += dx; runs[i].y += dy; }
    for (int i = b0; i < b1; i++) { boxes[i].x += dx; boxes[i].y += dy; }
    for (int i = c0; i < c1; i++) { clips[i].x += dx; clips[i].y += dy; }
}

static int add_box(int x, int y, int w, int h, uint32_t bg, uint8_t bga, const int* bw, const uint32_t* bc,
                   const uint8_t* bs, int rad, int r_at) {
    if (n_boxes >= MAX_BOXES) return -1;
    bx_t* b = &boxes[n_boxes++];
    b->x = x; b->y = y; b->w = w; b->h = h; b->bg = bg; b->bga = bga; b->rad = (uint8_t)rad;
    for (int i = 0; i < 4; i++) { b->bw[i] = (uint8_t)bw[i]; b->bc[i] = bc[i]; b->bs[i] = bs[i]; }
    b->clip = (int16_t)L.clip;
    b->r_at = r_at;
    b->img = -1;
    b->bgsz = b->bgrep = 0;
    return n_boxes - 1;
}

static void add_margin(int m) {
    if (m > 0) { if (m > L.mpos) L.mpos = m; }
    else if (m < L.mneg) L.mneg = m;
}
static void flush_margin(void) { L.y += L.mpos + L.mneg; L.mpos = L.mneg = 0; }

static void line_bounds(void) {
    for (int t = 0; t < 12; t++) {
        L.lx0 = L.x0; L.lx1 = L.x1;
        int low = -1;
        for (int i = 0; i < n_flts; i++) {
            flt_t* f = &flts[i];
            if (f->y1 <= L.y || f->y0 >= L.y + 16 || f->x1 <= L.x0 || f->x0 >= L.x1) continue;
            if (f->side == 1 && f->x1 > L.lx0) L.lx0 = f->x1;
            if (f->side == 2 && f->x0 < L.lx1) L.lx1 = f->x0;
            if (low < 0 || f->y1 < low) low = f->y1;
        }
        if (L.lx1 - L.lx0 >= 40 || low < 0) return;
        L.y = low;
    }
}

static void need_line(void) {
    if (L.dirty) return;
    flush_margin();
    line_bounds();
    L.lx = L.lx0 + (L.first && L.bs ? L.bs->indent : 0);
    L.dirty = true;
}

static void strut(const style_t* S, int* asc, int* h) {
    int f, sc;
    pick_font(S, &f, &sc);
    *asc = font_asc(f) * sc;
    *h = font_h(f) * sc;
}

static void end_line(bool br) {
    if (!L.dirty) { if (!br) return; need_line(); }
    for (int i = L.ib0; i < n_ibs; i++)
        if (ibs[i].fx >= 0 && n_frags < 160) {
            frags[n_frags].b = ibs[i]; frags[n_frags].x0 = ibs[i].fx; frags[n_frags].x1 = L.lx;
            n_frags++;
            ibs[i].fx = -1;
        }
    bool empty = n_atoms == L.a0 && n_frags == L.f0;
    if (empty && !br) { L.dirty = false; L.space = false; return; }
    int sa, sh;
    strut(L.bs, &sa, &sh);
    int lead = L.bs->lh ? L.bs->lh - sh : sh / 6;
    int base = sa + lead / 2, below = sh - sa + lead - lead / 2;
    if (base < 0) base = 0;
    if (below < 0) below = 0;
    for (int i = L.a0; i < n_atoms; i++) {
        atom_t* a = &atoms[i];
        if (a->va >= 1 && a->va <= 3) continue;
        int asc = a->asc + (a->va == 4 ? -sh / 5 : a->va == 5 ? sh / 3 : 0);
        if (asc > base) base = asc;
        if (a->h - asc > below) below = a->h - asc;
    }
    int lh = base + below;
    for (int i = L.a0; i < n_atoms; i++) if (atoms[i].va >= 1 && atoms[i].va <= 3 && atoms[i].h > lh) lh = atoms[i].h;
    int top = L.y, dx = 0;
    if (!g_meas) {
        int w = L.lx - L.lx0, av = L.lx1 - L.lx0;
        int al = L.bs->align;
        if (w < av) dx = al == 1 ? (av - w) / 2 : al == 2 ? av - w : 0;
    }
    for (int i = L.a0; i < n_atoms; i++) {
        atom_t* a = &atoms[i];
        int y;
        if (a->va == 1) y = top + (lh - a->h) / 2;
        else if (a->va == 2) y = top;
        else if (a->va == 3) y = top + lh - a->h;
        else y = top + base - (a->asc + (a->va == 4 ? -sh / 5 : a->va == 5 ? sh / 3 : 0));
        shift(a->r0, a->r1, a->b0, a->b1, a->c0, a->c1, dx, y - top);
    }
    for (int i = L.f0; i < n_frags; i++) {
        ibox_t* b = &frags[i].b;
        int x0 = frags[i].x0 - b->pl - b->bw[3] + dx, x1 = frags[i].x1 + b->pr + b->bw[1] + dx;
        int y0 = top + base - b->asc - b->pt - b->bw[0];
        int oc = L.clip;
        L.clip = b->clip;
        add_box(x0, y0, x1 - x0, b->fh + b->pt + b->pb + b->bw[0] + b->bw[2], b->bg, b->bga, b->bw, b->bc, b->bs, b->rad, b->fr);
        L.clip = oc;
    }
    n_frags = L.f0;
    if (L.lx > g_maxx) g_maxx = L.lx;
    L.last_base = top + base;
    L.y = top + lh;
    n_atoms = L.a0;
    L.dirty = false;
    L.space = false;
    L.first = false;
}

/* open inline boxes start their fragment at the first thing placed on a line */
static void touch_ibs(int x) {
    for (int i = L.ib0; i < n_ibs; i++) if (ibs[i].fx < 0) { ibs[i].fx = x; ibs[i].fr = n_runs; }
}

static void roman(int v, char* o, bool up) {
    static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char* const sy[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    int k = 0;
    if (v <= 0 || v > 3999) { itoa(v, o, 10); return; }
    for (int i = 0; i < 13; i++) while (v >= val[i] && k < 9) { for (const char* p = sy[i]; *p; p++) o[k++] = up ? (char)(*p - 32) : *p; v -= val[i]; }
    o[k] = 0;
}

static void place_marker(void) {
    int k = L.mk_kind;
    L.mk_kind = 0;
    int f = L.mk_f, sc = L.mk_sc, fh = font_h(f) * sc, asc = font_asc(f) * sc;
    int r0 = n_runs;
    if (k == LS_DISC || k == LS_CIRCLE || k == LS_SQUARE || k == LS_OPEN) {
        run_t* r = add_run(RK_BULLET, L.mk_x - 14, L.y, 6, fh, 0, L.mk_fg, 0, 0);
        if (r) { r->flags = (uint8_t)k; r->link = -1; }
    } else {
        int nl = (int)strlen(L.mk_t), w = text_w(f, L.mk_t, nl) * sc;
        int off = pool_put(L.mk_t, nl);
        run_t* r = off >= 0 ? add_run(RK_TEXT, L.mk_x - w - 6, L.y, w, fh, f, L.mk_fg, off, nl) : NULL;
        if (r) { r->scale = (uint8_t)sc; r->link = -1; }
    }
    if (n_runs > r0) add_atom(r0, n_boxes, n_clips, asc, fh, 0);
}

static void set_marker(const style_t* S, int x) {
    L.mk_kind = S->lst;
    if (!S->lst) return;
    L.mk_x = x;
    L.mk_fg = S->fg;
    pick_font(S, &L.mk_f, &L.mk_sc);
    int v = g_linum;
    char* o = L.mk_t;
    if (S->lst == LS_DEC) itoa(v, o, 10);
    else if (S->lst == LS_LALPHA || S->lst == LS_UALPHA) { o[0] = (char)((S->lst == LS_LALPHA ? 'a' : 'A') + (v - 1) % 26); o[1] = 0; }
    else if (S->lst == LS_LROMAN || S->lst == LS_UROMAN) roman(v, o, S->lst == LS_UROMAN);
    else return;
    scat(o, 12, ".");
}

static void emit_piece(const style_t* S, int f, int sc, const char* s, int n, int w, int sp) {
    int x = L.lx + sp + L.pend;
    bool padded = L.pend != 0;
    L.pend = 0;
    if (L.mk_kind) place_marker();
    touch_ibs(x);
    uint32_t fg = S->fg;
    uint8_t fl = (uint8_t)(((S->deco & TD_UL) ? RF_UL : 0) | ((S->deco & TD_STRIKE) ? RF_STRIKE : 0) |
                           ((S->deco & TD_OVER) ? RF_OVER : 0) | (S->italic ? RF_ITALIC : 0) | (S->vis ? RF_HIDE : 0));
    int spw = word_w(f, sc, S->ls, " ", 1);
    run_t* p = (n_atoms > L.a0 && atoms[n_atoms - 1].r0 == n_runs - 1 && atoms[n_atoms - 1].r1 == n_runs) ? &runs[n_runs - 1] : NULL;
    if (p && !padded && p->kind == RK_TEXT && p->font == f && p->scale == sc && p->fg == fg && p->link == L.link &&
        p->flags == fl && p->ls == S->ls && p->clip == L.clip && p->off + p->len + 1 == pool_used &&
        p->x + p->w + sp == x && (sp == 0 || sp == spw) && pool_used + n + 2 < POOL_CAP && p->len + n + 1 < 60000) {
        int at = pool_used - 1;
        if (sp) pool[at++] = ' ';
        memcpy(pool + at, s, (size_t)n);
        pool[at + n] = 0;
        pool_used = at + n + 1;
        p->len = (uint16_t)(p->len + (sp ? 1 : 0) + n);
        p->w = (uint16_t)(p->w + sp + w);
    } else {
        int off = pool_put(s, n);
        if (off < 0) return;
        int r0 = n_runs;
        run_t* r = add_run(RK_TEXT, x, L.y, w, font_h(f) * sc, f, fg, off, n);
        if (!r) return;
        r->scale = (uint8_t)sc; r->flags = fl; r->ls = (int8_t)S->ls;
        add_atom(r0, n_boxes, n_clips, font_asc(f) * sc, font_h(f) * sc, S->va);
    }
    L.lx = x + w;
    if (w > g_minw) g_minw = w;
}

static void emit_word(const style_t* S, const char* s, int n) {
    if (n <= 0) return;
    static char tb[1100];
    if (S->tt && n < 1000) {
        bool st = true;
        for (int i = 0; i < n; i++) {
            uint8_t c = (uint8_t)s[i];
            bool up = S->tt == 1 || (S->tt == 3 && st);
            if (up) {
                if (c >= 'a' && c <= 'z') c -= 32;
                else if (c >= 0xA0 && c <= 0xAF) c -= 0x20;
                else if (c >= 0xE0 && c <= 0xEF) c -= 0x50;
                else if (c == 0xF1) c = 0xF0;
            } else if (S->tt == 2) {
                if (c >= 'A' && c <= 'Z') c += 32;
                else if (c >= 0x80 && c <= 0x8F) c += 0x20;
                else if (c >= 0x90 && c <= 0x9F) c += 0x50;
                else if (c == 0xF0) c = 0xF1;
            }
            st = c == ' ' || c == '-';
            tb[i] = (char)c;
        }
        s = tb;
    }
    int f, sc;
    pick_font(S, &f, &sc);
    bool has = n_atoms > L.a0 && L.dirty;
    int sp = (L.space && has) ? word_w(f, sc, S->ls, " ", 1) + S->wsp : 0;
    L.space = false;
    int w = word_w(f, sc, S->ls, s, n);
    bool wrap = S->ws != 1 && S->ws != 2;
    if (wrap && has && L.lx + sp + L.pend + w > L.lx1) { end_line(false); sp = 0; }
    need_line();
    int avail = L.lx1 - L.lx0;
    while (wrap && w > avail && n > 1) {                 /* longer than a line: hard-break it */
        int room = L.lx1 - L.lx - sp - L.pend;
        int k = n;
        while (k > 1 && word_w(f, sc, S->ls, s, k) > room) k = k * 3 / 4 < k - 1 ? k * 3 / 4 : k - 1;
        emit_piece(S, f, sc, s, k, word_w(f, sc, S->ls, s, k), sp);
        end_line(false);
        need_line();
        s += k; n -= k; sp = 0;
        w = word_w(f, sc, S->ls, s, n);
    }
    emit_piece(S, f, sc, s, n, w, sp);
}

/* text already in cp866, entities decoded unless raw */
static void emit_str(const style_t* S, const char* buf, int m) {
    if (S->ws == 2 || S->ws == 3) {
        int a = 0;
        for (int i = 0; i <= m; i++) {
            if (i < m && buf[i] != '\n') continue;
            if (i > a) {
                static char line[1024];
                int o = 0;
                for (int k = a; k < i && o < 1000; k++) {
                    if (buf[k] == '\t') { do line[o++] = ' '; while (o % 8 && o < 1000); }
                    else if (buf[k] != '\r') line[o++] = buf[k];
                }
                L.space = false;
                emit_word(S, line, o);
            }
            if (i < m) end_line(true);
            a = i + 1;
        }
        return;
    }
    int i = 0;
    while (i < m) {
        char c = buf[i];
        if (c == '\n' && S->ws == 4) { end_line(true); i++; continue; }
        if (isws(c)) { L.space = true; i++; continue; }
        int j = i;
        while (j < m && !isws(buf[j])) j++;
        emit_word(S, buf + i, j - i);
        i = j;
    }
}

static void emit_text(const style_t* S, const char* s, int n) {
    static char buf[8192];
    while (n > 0) {
        int chunk = n > 4000 ? 4000 : n;
        if (chunk < n) while (chunk > 1 && s[chunk - 1] != ' ' && s[chunk - 1] != '\n') chunk--;
        int m = decode_entities(s, chunk, buf, sizeof buf);
        s += chunk; n -= chunk;
        emit_str(S, buf, m);
    }
}

/* form controls and buttons are atoms on the line */
static void emit_box(int kind, int w, int h, int field, int off, int len) {
    bool has = n_atoms > L.a0 && L.dirty;
    int sp = has ? (L.space ? 8 : 4) : 0;
    L.space = false;
    if (has && L.lx + sp + w > L.lx1) { end_line(false); sp = 0; }
    need_line();
    if (L.mk_kind) place_marker();
    int x = L.lx + sp + L.pend;
    L.pend = 0;
    touch_ibs(x);
    int r0 = n_runs;
    run_t* r = add_run(kind, x, L.y, w, h, UIF_MED, COL_TEXT, off, len);
    if (!r) return;
    r->field = (int16_t)field;
    add_atom(r0, n_boxes, n_clips, h - 8, h, 0);
    L.lx = x + w;
    if (w > g_minw) g_minw = w;
}

typedef struct { lay_t L; int n_ibs; } save_t;

static void sub_begin(save_t* sv, int x0, int x1, int y) {
    sv->L = L;
    sv->n_ibs = n_ibs;
    L.x0 = L.lx0 = L.lx = x0; L.x1 = L.lx1 = x1; L.y = y;
    L.dirty = L.space = false;
    L.mpos = L.mneg = L.pend = 0;
    L.a0 = n_atoms; L.f0 = n_frags; L.ib0 = n_ibs;
    L.first = true;
    L.mk_kind = 0;
    L.last_base = -1;
}
static void sub_end(save_t* sv) {
    n_atoms = sv->L.a0 > n_atoms ? n_atoms : n_atoms;
    n_atoms = L.a0;
    n_frags = L.f0;
    n_ibs = sv->n_ibs;
    L = sv->L;
}

typedef struct { int runs, pool, boxes, clips, links, url, fields, forms, flts; } snap_t;
static void snap(snap_t* s) {
    s->runs = n_runs; s->pool = pool_used; s->boxes = n_boxes; s->clips = n_clips; s->links = n_links;
    s->url = url_used; s->fields = n_fields; s->forms = n_forms; s->flts = n_flts;
}
static void unsnap(const snap_t* s) {
    n_runs = s->runs; pool_used = s->pool; n_boxes = s->boxes; n_clips = s->clips; n_links = s->links;
    url_used = s->url; n_fields = s->fields; n_forms = s->forms; n_flts = s->flts;
}

static bool is_bfc(const style_t* S) {
    return S->ovf || S->bfc || S->flt || S->pos >= 2 || S->disp == D_FLEX || S->disp == D_GRID || S->disp == D_TABLE ||
           S->disp == D_CELL || S->disp == D_IBLOCK || S->disp == D_IFLEX || S->disp == D_IGRID || S->disp == D_ITABLE;
}

static void do_clear(int side) {
    end_line(false);
    for (int i = 0; i < n_flts; i++)
        if ((flts[i].side & side) && flts[i].y1 > L.y + L.mpos) { L.y = flts[i].y1; L.mpos = L.mneg = 0; }
}

/* ---------- boxes ---------- */
static int  lay_node(int n, const style_t* P);
static void lay_inner(int n, const style_t* S, const style_t* B, const style_t* A);

static int pbw_h(const style_t* S) { return S->p[1] + S->p[3] + S->bw[1] + S->bw[3]; }

/* lays the border box of n at x (border-box width w) from L.y; L.y ends at its bottom */
static void lay_box(int n, const style_t* S, const style_t* B, const style_t* A, int x, int w, int stretch) {
    int bt = S->bw[0], bb = S->bw[2];
    int y0 = L.y;
    int bi = -1;
    if (S->bga || S->bw[0] || S->bw[1] || S->bw[2] || S->bw[3] || S->bgimg >= 0)
        bi = add_box(x, y0, w, 0, S->bg, S->vis ? 0 : S->bga, S->bw, S->bc, S->bs, S->rad, n_runs);
    if (bi >= 0 && S->bgimg >= 0 && !S->vis) {
        boxes[bi].img = (int16_t)S->bgimg; boxes[bi].bgsz = S->bgsz; boxes[bi].bgrep = S->bgrep; boxes[bi].pbg = S->pbg;
    }
    if (bi >= 0 && S->vis) memset(boxes[bi].bw, 0, 4);
    int oc = L.clip, ci = -1;
    if ((S->ovf == 1 || S->hclip) && n_clips < MAX_CLIPS) {
        ci = n_clips++;
        clips[ci] = (clip_t){ x, y0, w, 0, L.clip };
        L.clip = ci;
    }
    int sx0 = L.x0, sx1 = L.x1, sfl = n_flts;
    const style_t* sbs = L.bs;
    int scx = cb_x, scy = cb_y, scw = cb_w;
    if (S->pos) { cb_x = x; cb_y = y0; cb_w = w; }
    L.x0 = x + S->bw[3] + S->p[3];
    L.x1 = x + w - S->bw[1] - S->p[1];
    if (L.x1 < L.x0 + 1) L.x1 = L.x0 + 1;
    L.y = y0 + bt + S->p[0];
    L.mpos = L.mneg = 0;
    L.bs = S;
    L.first = true;
    L.dirty = false;
    int ct = L.y;
    lay_inner(n, S, B, A);
    end_line(false);
    if (S->p[2] || bb || is_bfc(S)) flush_margin();
    if (is_bfc(S)) {
        for (int i = sfl; i < n_flts; i++) if (flts[i].y1 > L.y) L.y = flts[i].y1;
        n_flts = sfl;
    }
    int ch = L.y - ct;
    int h = S->h;
    if (h != LA) {
        if (S->bbox) h -= S->p[0] + S->p[2] + bt + bb;
        if (h < 0) h = 0;
        if (S->ovf == 1 || h > ch) ch = h;
    }
    int mh = S->minh ? S->minh - (S->bbox ? S->p[0] + S->p[2] + bt + bb : 0) : 0;
    if (ch < mh) ch = mh;
    if (ch < stretch) ch = stretch;
    L.y = ct + ch + S->p[2] + bb;
    if (bi >= 0) boxes[bi].h = L.y - y0;
    if (ci >= 0) clips[ci].h = L.y - y0;
    L.clip = oc;
    L.x0 = sx0; L.x1 = sx1; L.bs = sbs;
    cb_x = scx; cb_y = scy; cb_w = scw;
    L.first = false;
    L.dirty = false;
}

/* max-content / min-content border-box widths */
static void measure(int n, const style_t* S, const style_t* B, const style_t* A, int* mx, int* mn) {
    if (n >= 0 && nodes[n].mmax >= 0) { *mx = nodes[n].mmax; *mn = nodes[n].mmin; return; }
    snap_t sn;
    snap(&sn);
    save_t sv;
    bool om = g_meas;
    int ogx = g_maxx, ogm = g_minw, ob = n_boxes;
    g_meas = true;
    g_maxx = 0; g_minw = 0;
    sub_begin(&sv, 0, 100000, 0);
    style_t T = *S;
    T.w = LA; T.minw = 0; T.maxw = LA;
    lay_box(n, &T, B, A, 0, 100000, 0);
    int x = g_maxx + S->p[1] + S->bw[1], m = g_minw + pbw_h(S);
    sub_end(&sv);
    (void)ob;
    unsnap(&sn);
    g_meas = om; g_maxx = ogx; g_minw = ogm;
    if (x < pbw_h(S)) x = pbw_h(S);
    if (m > x) x = m;
    *mx = x; *mn = m;
    if (n >= 0) { nodes[n].mmax = x; nodes[n].mmin = m; }
}

/* border-box width of an auto-sized (shrink to fit) box */
static int fit_width(int n, const style_t* S, const style_t* B, const style_t* A, int avail) {
    int pb = pbw_h(S), w;
    if (S->w != LA && !(g_meas && (S->fixh & 2))) w = S->w + (S->bbox ? 0 : pb);
    else {
        int mx, mn;
        measure(n, S, B, A, &mx, &mn);
        w = mx;
        if (w > avail) w = mn > avail ? mn : avail;
    }
    if (S->maxw != LA) { int m = S->maxw + (S->bbox ? 0 : pb); if (w > m) w = m; }
    if (S->minw) { int m = S->minw + (S->bbox ? 0 : pb); if (w < m) w = m; }
    return w < pb ? pb : w;
}

static int block_width(const style_t* S, int avail, int* ml, int* mr) {
    int pb = pbw_h(S);
    int w = S->w;
    if (w != LA && g_meas && (S->fixh & 2)) w = LA;
    if (w != LA && !S->bbox) w += pb;
    *ml = S->m[3]; *mr = S->m[1];
    if (w == LA) w = avail - *ml - *mr;
    if (S->maxw != LA && !(g_meas && S->maxw > 20000)) { int m = S->maxw + (S->bbox ? 0 : pb); if (w > m) w = m; }
    if (S->minw) { int m = S->minw + (S->bbox ? 0 : pb); if (w < m) w = m; }
    if (w < pb) w = pb;
    int fr = avail - w - *ml - *mr;
    if (fr > 0 && !g_meas) {
        if ((S->mauto & 10) == 10) *ml += fr / 2;
        else if (S->mauto & 8) *ml += fr;
        else if (L.bs && L.bs->cblk) *ml += fr / 2;
    }
    return w;
}

static void rel_shift(const style_t* S, int r0, int b0, int c0) {
    if (S->pos != 1 || !S->offs) return;
    int dx = (S->offs & 8) ? S->off[3] : (S->offs & 2) ? -S->off[1] : 0;
    int dy = (S->offs & 1) ? S->off[0] : (S->offs & 4) ? -S->off[2] : 0;
    shift(r0, n_runs, b0, n_boxes, c0, n_clips, dx, dy);
}

static void lay_flow_block(int n, const style_t* S, const style_t* B, const style_t* A) {
    end_line(false);
    if (S->clr) do_clear(S->clr);
    add_margin(S->m[0]);
    flush_margin();
    int ml, mr;
    int w = block_width(S, L.x1 - L.x0, &ml, &mr);
    int x = L.x0 + ml;
    if (is_bfc(S) && n_flts) {                     /* next to floats: squeeze in */
        line_bounds();
        if (L.lx0 > x) { int d = L.lx0 - x; x += d; w -= d; }
        if (x + w > L.lx1 && L.lx1 > x + 40) w = L.lx1 - x;
    }
    int r0 = n_runs, b0 = n_boxes, c0 = n_clips, gm = g_maxx;
    g_maxx = 0;
    if (S->disp == D_LI) set_marker(S, x + S->bw[3] + S->p[3]);
    lay_box(n, S, B, A, x, w, 0);
    L.mk_kind = 0;
    if (g_maxx) g_maxx += S->p[1] + S->bw[1] + mr;
    if (S->w != LA && !(S->fixh & 2) && x + w + mr > g_maxx) g_maxx = x + w + mr;
    if (gm > g_maxx) g_maxx = gm;
    add_margin(S->m[2]);
    rel_shift(S, r0, b0, c0);
}

static void lay_atomic(int n, const style_t* S, const style_t* B, const style_t* A) {
    int ml = S->m[3], mr = S->m[1];
    int avail = L.x1 - L.x0 - ml - mr;
    int w = fit_width(n, S, B, A, avail > 20 ? avail : 20);
    int tot = ml + w + mr;
    bool has = n_atoms > L.a0 && L.dirty;
    int sp = (L.space && has) ? 4 : 0;
    if (has && S->ws != 1 && L.lx + sp + L.pend + tot > L.lx1) { end_line(false); sp = 0; }
    need_line();
    if (L.mk_kind) place_marker();
    L.space = false;
    int x = L.lx + sp + L.pend + ml;
    L.pend = 0;
    touch_ibs(x - ml);
    int r0 = n_runs, b0 = n_boxes, c0 = n_clips;
    save_t sv;
    int y0 = L.y;
    sub_begin(&sv, x, x + w, y0 + S->m[0]);
    lay_box(n, S, B, A, x, w, 0);
    int h = L.y - y0;
    int lb = L.last_base;
    sub_end(&sv);
    int asc = (lb >= 0 && !S->ovf) ? lb - y0 : h;
    add_atom(r0, b0, c0, asc, h + S->m[2], S->va);
    L.lx = x + w + mr;
    if (tot > g_minw) g_minw = tot;
    rel_shift(S, r0, b0, c0);
}

static void lay_float(int n, const style_t* S, const style_t* B, const style_t* A) {
    if (n_flts >= 64) { lay_flow_block(n, S, B, A); return; }
    if (n_atoms > L.a0) end_line(false);
    flush_margin();
    int ml = S->m[3], mr = S->m[1];
    int w = fit_width(n, S, B, A, L.x1 - L.x0 - ml - mr);
    int tot = ml + w + mr;
    int side = g_meas ? 1 : S->flt;
    int sy = L.y;
    for (int t = 0; t < 12; t++) {
        line_bounds();
        if (L.lx1 - L.lx0 >= tot) break;
        int low = -1;
        for (int i = 0; i < n_flts; i++) if (flts[i].y1 > L.y && (low < 0 || flts[i].y1 < low)) low = flts[i].y1;
        if (low < 0) break;
        L.y = low;
    }
    int x = side == 1 ? L.lx0 + ml : L.lx1 - mr - w;
    int y = L.y;
    L.y = sy;
    int r0 = n_runs, b0 = n_boxes, c0 = n_clips;
    save_t sv;
    sub_begin(&sv, x, x + w, y + S->m[0]);
    lay_box(n, S, B, A, x, w, 0);
    int y1 = L.y + S->m[2];
    sub_end(&sv);
    flts[n_flts++] = (flt_t){ x - ml, x + w + mr, y, y1, side };
    if (x + w + mr > g_maxx) g_maxx = x + w + mr;
    if (tot > g_minw) g_minw = tot;
    rel_shift(S, r0, b0, c0);
}

static void lay_abs(int n, const style_t* S, const style_t* B, const style_t* A) {
    if (S->hclip) return;
    int bx = cb_x, by = cb_y, bw = cb_w;
    if (S->pos == 3) { bx = 0; by = 0; bw = view_w_l; }
    int ml = S->m[3], mr = S->m[1], w;
    if (S->w == LA && (S->offs & 10) == 10) w = bw - S->off[1] - S->off[3] - ml - mr;
    else w = fit_width(n, S, B, A, bw - ml - mr);
    if (S->ovf == 1 && S->w != LA && w <= 2) return;    /* sr-only */
    if (w < 1) w = 1;
    int x = (S->offs & 8) ? bx + S->off[3] + ml : (S->offs & 2) ? bx + bw - S->off[1] - mr - w :
            (L.dirty ? L.lx : L.x0) + ml;
    int y = (S->offs & 1) ? by + S->off[0] + S->m[0] : L.y + L.mpos + S->m[0];
    int gm = g_maxx, gn = g_minw;
    save_t sv;
    sub_begin(&sv, x, x + w, y);
    lay_box(n, S, B, A, x, w, 0);
    sub_end(&sv);
    g_maxx = gm; g_minw = gn;
}

static void lay_pseudo(const style_t* X) {
    if (X->disp == D_NONE || X->cntl < 0) return;
    bool blk = X->disp != D_INLINE && X->disp != D_IBLOCK && X->disp != D_IFLEX;
    if (blk || X->pos >= 2) {
        if (X->clr) do_clear(X->clr);
        if (!X->cntl || X->pos >= 2) return;
        end_line(false);
    }
    if (X->cntl > 0) {
        int pl = X->p[3] + X->m[3];
        L.pend += pl;
        emit_str(X, X->cnt, X->cntl);
        L.lx += X->p[1] + X->m[1];
    }
    if (blk) end_line(false);
}

static void lay_inline(int n, const style_t* S, const style_t* B, const style_t* A);

static void lay_children(int n, const style_t* S, const style_t* B, const style_t* A) {
    if (B && B->cntl >= 0) lay_pseudo(B);
    int num = 1;
    static char v[16];
    if (get_attr(node_attrs(n), "start", v, sizeof v)) num = atoi(v);
    bool first = true;
    for (int c = nodes[n].first; c >= 0; c = nodes[c].next) {
        if (nodes[c].text) {
            const char* t = (const char*)resp + nodes[c].a;
            int len = nodes[c].an;
            if (first && S->ws == 2 && len && t[0] == '\n') { t++; len--; }
            first = false;
            if (!S->vis || 1) emit_text(S, t, len);
            continue;
        }
        first = false;
        if (get_attr(node_attrs(c), "value", v, sizeof v) && is_tag(c, "li")) num = atoi(v);
        g_linum = num;
        if (lay_node(c, S) == D_LI) num++;
    }
    if (A && A->cntl >= 0) lay_pseudo(A);
}

static void lay_inline(int n, const style_t* S, const style_t* B, const style_t* A) {
    bool box = S->bga || S->bw[0] || S->bw[1] || S->bw[2] || S->bw[3];
    int pl = S->p[3] + S->bw[3] + S->m[3], pr = S->p[1] + S->bw[1] + S->m[1];
    L.pend += pl;
    int ib = -1;
    if (box && !S->vis && n_ibs < 32) {
        ib = n_ibs++;
        ibox_t* b = &ibs[ib];
        b->bg = S->bg; b->bga = S->bga; b->rad = (uint8_t)S->rad;
        for (int i = 0; i < 4; i++) { b->bw[i] = S->bw[i]; b->bc[i] = S->bc[i]; b->bs[i] = S->bs[i]; }
        b->pt = S->p[0]; b->pb = S->p[2]; b->pl = S->p[3]; b->pr = S->p[1];
        strut(S, &b->asc, &b->fh);
        b->fx = -1; b->fr = n_runs; b->clip = L.clip;
    }
    int r0 = n_runs, b0 = n_boxes, c0 = n_clips;
    lay_children(n, S, B, A);
    if (ib >= 0) {
        if (ibs[ib].fx >= 0 && n_frags < 160) {
            frags[n_frags].b = ibs[ib]; frags[n_frags].x0 = ibs[ib].fx; frags[n_frags].x1 = L.lx;
            n_frags++;
        }
        n_ibs = ib;
    }
    if (n_atoms > L.a0 && L.dirty) L.lx += pr;
    else if (!L.dirty) L.pend = 0;
    rel_shift(S, r0, b0, c0);
}

/* ---------- flex, grid, tables ---------- */
/* Item styles are computed when needed and dropped right after: a HN page
   has hundreds of cells and a style is ~400 bytes. */
#define MAX_ITEMS 4096
#define SPOOL 480
static style_t spool[SPOOL];
static int     n_spool;

typedef struct { int n, w, mn, mx, h, x, r0, r1, b0, b1, c0, c1, span, grow, ml, mr; uint8_t text, mauto, va, own;
                 uint32_t garea; int16_t gcs, gce, grs, gre; int gx0, gx1, gy0, gy1; } item_t;
static item_t  items[MAX_ITEMS];
static int     n_items;

static style_t* item_st(item_t* it, const style_t* S, int cbw, int* nv) {
    *nv = n_vars;
    if (it->text || n_spool + 3 > SPOOL) return (style_t*)S;
    style_t* X = &spool[n_spool];
    compute(it->n, S, X, cbw, X + 1, X + 2);
    n_spool += 3;
    return X;
}
static void item_rel(const style_t* X, const style_t* S, int nv) {
    if (X != S) n_spool -= 3;
    n_vars = nv;
}

/* children of n as items (loose text too); out-of-flow ones are laid right away */
static int get_items(int n, const style_t* S, int cbw, int* base, bool rows) {
    *base = n_items;
    int k = 0;
    for (int c = nodes[n].first; c >= 0; c = nodes[c].next) {
        if (n_items >= MAX_ITEMS) break;
        item_t* it = &items[n_items];
        memset(it, 0, sizeof *it);
        it->n = c;
        it->span = 1;
        if (nodes[c].text) {
            const char* t = (const char*)resp + nodes[c].a;
            int i = 0;
            while (i < nodes[c].an && isws(t[i])) i++;
            if (i == nodes[c].an || rows) continue;
            it->text = 1;
        } else {
            int nv;
            style_t* X = item_st(it, S, cbw, &nv);
            if (X == S) continue;
            bool skip = X->disp == D_NONE;
            if (!skip && X->pos >= 2) { lay_abs(c, X, X + 1, X + 2); skip = true; }
            it->ml = X->m[3]; it->mr = X->m[1]; it->mauto = X->mauto; it->grow = X->grow; it->va = X->va;
            static char v[8];
            if (get_attr(node_attrs(c), "colspan", v, sizeof v)) { it->span = atoi(v); if (it->span < 1) it->span = 1; }
            if (X->gspan) it->span = X->gspan;
            it->garea = X->garea; it->gcs = X->gcs; it->gce = X->gce; it->grs = X->grs; it->gre = X->gre;
            if (rows && X->disp != D_ROW && !is_tag(c, "tr")) skip = true;
            item_rel(X, S, nv);
            if (skip) continue;
        }
        n_items++;
        k++;
    }
    return k;
}

/* lays one item as a block at (x, y), border width w; returns the height with margins */
static int lay_item(item_t* it, const style_t* S, int cbw, int x, int y, int w) {
    save_t sv;
    it->r0 = n_runs; it->b0 = n_boxes; it->c0 = n_clips;
    sub_begin(&sv, x, x + w, y);
    if (it->text) {
        L.bs = S;
        emit_text(S, (const char*)resp + nodes[it->n].a, nodes[it->n].an);
        end_line(false);
    } else {
        int nv;
        style_t* X = item_st(it, S, cbw, &nv);
        it->own = X->bga || X->bw[0] || X->bw[1] || X->bw[2] || X->bw[3];
        L.y += X->m[0];
        lay_box(it->n, X, X + 1, X + 2, x, w, 0);
        L.y += X->m[2];
        item_rel(X, S, nv);
    }
    int h = L.y - y;
    sub_end(&sv);
    it->r1 = n_runs; it->b1 = n_boxes; it->c1 = n_clips;
    return h;
}

static void item_size(item_t* it, const style_t* S, int cbw) {
    if (it->text) {
        save_t sv;
        snap_t sn;
        snap(&sn);
        bool om = g_meas;
        int ox = g_maxx, on = g_minw;
        g_meas = true; g_maxx = 0; g_minw = 0;
        sub_begin(&sv, 0, 100000, 0);
        L.bs = S;
        emit_text(S, (const char*)resp + nodes[it->n].a, nodes[it->n].an);
        end_line(false);
        it->mx = g_maxx; it->mn = g_minw;
        sub_end(&sv);
        unsnap(&sn);
        g_meas = om; g_maxx = ox; g_minw = on;
        return;
    }
    int nv;
    style_t* X = item_st(it, S, cbw, &nv);
    int mx, mn, pb = pbw_h(X);
    if (X->basis != LA && X->basis > 0) mx = mn = X->basis + (X->bbox ? 0 : pb);
    else if (X->w != LA && !(g_meas && (X->fixh & 2))) mx = mn = X->w + (X->bbox ? 0 : pb);
    else measure(it->n, X, X + 1, X + 2, &mx, &mn);
    if (X->maxw != LA) { int m = X->maxw + (X->bbox ? 0 : pb); if (mx > m) mx = m; if (mn > m) mn = m; }
    if (X->minw) { int m = X->minw + (X->bbox ? 0 : pb); if (mx < m) mx = m; if (mn < m) mn = m; }
    if (X->basis == 0 && X->grow) mx = pb;
    it->mx = mx + it->ml + it->mr;
    it->mn = mn + it->ml + it->mr;
    item_rel(X, S, nv);
}

static void shift_item(item_t* it, int dx, int dy) { shift(it->r0, it->r1, it->b0, it->b1, it->c0, it->c1, dx, dy); }

static void lay_flex(int n, const style_t* S) {
    int base, it0 = n_items;
    int x0 = L.x0, avail = L.x1 - L.x0;
    end_line(false);
    flush_margin();
    int k = get_items(n, S, avail, &base, false);
    item_t* I = &items[base];
    if (S->fdir) {                                /* column: blocks with a gap */
        for (int i = 0; i < k; i++) {
            int x = x0 + I[i].ml, w = avail - I[i].ml - I[i].mr;
            if (!I[i].text) {
                int nv;
                style_t* X = item_st(&I[i], S, avail, &nv);
                int ml, mr;
                if (X->w == LA && S->ai && S->ai != 1) {
                    item_rel(X, S, nv);
                    item_size(&I[i], S, avail);
                    w = I[i].mx - I[i].ml - I[i].mr;
                    if (w > avail - I[i].ml - I[i].mr) w = avail - I[i].ml - I[i].mr;
                    int fr = avail - w - I[i].ml - I[i].mr;
                    if (S->ai == 2) x += fr / 2;
                    if (S->ai == 3) x += fr;
                } else {
                    w = block_width(X, avail, &ml, &mr);
                    x = x0 + ml;
                    item_rel(X, S, nv);
                }
            }
            int h = lay_item(&I[i], S, avail, x, L.y, w);
            L.y += h + (i + 1 < k ? S->rgap : 0);
        }
        n_items = it0;
        return;
    }
    for (int i = 0; i < k; i++) item_size(&I[i], S, avail);
    int gap = S->gap;
    for (int a = 0; a < k; ) {
        int b = a, sum = 0, grow = 0;
        while (b < k) {
            int add = I[b].mx + (b > a ? gap : 0);
            if (S->fwrap && b > a && sum + add > avail) break;
            sum += add;
            grow += I[b].grow;
            b++;
        }
        int fr = avail - sum;
        for (int i = a; i < b; i++) I[i].w = I[i].mx;
        if (fr > 0 && grow && !g_meas) {
            for (int i = a; i < b; i++) I[i].w += fr * I[i].grow / grow;
            fr = 0;
        } else if (fr < 0) {                          /* shrink, but not below min-content */
            int over = -fr, tot = 0;
            for (int i = a; i < b; i++) tot += I[i].mx - I[i].mn;
            for (int i = a; i < b && tot > 0; i++) {
                int d = (int)((long long)over * (I[i].mx - I[i].mn) / tot);
                if (d > I[i].mx - I[i].mn) d = I[i].mx - I[i].mn;
                I[i].w -= d;
            }
            fr = 0;
        }
        int x = x0, sp = gap, cnt = b - a;
        int push = -1;                                /* margin-left:auto item */
        for (int i = a + 1; i < b; i++) if (!I[i].text && (I[i].mauto & 8)) { push = i; break; }
        if (!g_meas && fr > 0 && push < 0) {
            switch (S->jc) {
            case 1: x += fr / 2; break;
            case 2: x += fr; break;
            case 3: if (cnt > 1) sp += fr / (cnt - 1); break;
            case 4: sp += fr / cnt; x += fr / cnt / 2; break;
            case 5: sp += fr / (cnt + 1); x += fr / (cnt + 1); break;
            }
        }
        int y = L.y, lh = 0;
        for (int i = a; i < b; i++) {
            if (i == push && fr > 0 && !g_meas) x += fr;
            int w = I[i].w - I[i].ml - I[i].mr;
            if (w < 1) w = 1;
            I[i].x = x + I[i].ml;
            I[i].h = lay_item(&I[i], S, avail, x + I[i].ml, y, w);
            if (I[i].h > lh) lh = I[i].h;
            x += I[i].w + sp;
        }
        if (x - sp > g_maxx) g_maxx = x - sp;
        for (int i = a; i < b; i++) {                 /* align-items */
            int d = lh - I[i].h;
            if (d <= 0) continue;
            if (S->ai == 2) shift_item(&I[i], 0, d / 2);
            else if (S->ai == 3) shift_item(&I[i], 0, d);
            else if (S->ai == 0 && I[i].own && I[i].b1 > I[i].b0) boxes[I[i].b0].h += d;   /* stretch: the background */
        }
        L.y = y + lh + (b < k ? S->rgap : 0);
        a = b;
    }
    n_items = it0;
}

static void lay_grid(int n, const style_t* S) {
    int base, it0 = n_items;
    int x0 = L.x0, avail = L.x1 - L.x0, gap = S->gap, rgap = S->rgap;
    end_line(false);
    flush_margin();
    int k = get_items(n, S, avail, &base, false);
    item_t* I = &items[base];

    /* named areas: "a a" "b c" */
    static uint32_t amap[16][12];
    int ar = 0, ac = 0;
    if (S->gta) {
        for (const char* q = S->gta; *q && ar < 16; q++) {
            if (*q != '"' && *q != '\'') continue;
            char qq = *q++;
            int c = 0;
            while (*q && *q != qq) {
                while (*q == ' ') q++;
                if (!*q || *q == qq) break;
                const char* s = q;
                while (*q && *q != ' ' && *q != qq) q++;
                if (c < 12) amap[ar][c++] = *s == '.' ? 0 : hsh(s, (int)(q - s));
            }
            if (c > ac) ac = c;
            ar++;
            if (!*q) break;
        }
    }
    int nc = S->ntrk, kind[12], trk[12];
    for (int i = 0; i < nc; i++) { kind[i] = S->trkfr[i]; trk[i] = S->trk[i]; }
    if (S->trkmin) {
        nc = (avail + gap) / (S->trkmin + gap);
        if (nc < 1) nc = 1;
        if (nc > 12) nc = 12;
        for (int i = 0; i < nc; i++) { kind[i] = 1; trk[i] = 100; }
    }
    for (; nc < ac && nc < 12; nc++) { kind[nc] = 1; trk[nc] = 100; }
    if (!nc) { nc = 1; kind[0] = 1; trk[0] = 100; }

    /* placement */
    static uint8_t occ[256][12];
    memset(occ, 0, sizeof occ);
    int cr = 0, cc = 0, nrows = 0;
    for (int i = 0; i < k; i++) {
        item_t* it = &I[i];
        int c0 = -1, c1 = -1, r0 = -1, r1 = -1;
        if (it->garea && S->gta) {
            for (int r = 0; r < ar; r++)
                for (int c = 0; c < ac; c++)
                    if (amap[r][c] == it->garea) {
                        if (r0 < 0 || r < r0) r0 = r;
                        if (r + 1 > r1) r1 = r + 1;
                        if (c0 < 0 || c < c0) c0 = c;
                        if (c + 1 > c1) c1 = c + 1;
                    }
        }
        if (c0 < 0) {
            int span = it->span > 0 ? it->span : 1;
            if (span > nc) span = nc;
            if (it->gcs) c0 = it->gcs > 0 ? it->gcs - 1 : nc + 1 + it->gcs;
            if (it->gce) c1 = it->gce > 0 ? it->gce - 1 : nc + 1 + it->gce;
            if (c0 >= 0 && c1 <= c0) c1 = c0 + span;
            if (c0 < 0 && c1 >= 0) c0 = c1 - span;
            if (it->grs > 0) r0 = it->grs - 1;
            if (it->gre > 0) r1 = it->gre - 1;
            if (r0 >= 0 && r1 <= r0) r1 = r0 + 1;
            if (c0 < 0) {                                    /* auto: next free spot */
                for (;;) {
                    if (cc + span > nc) { cc = 0; cr++; }
                    if (cr >= 254) break;
                    bool ok = true;
                    for (int c = cc; c < cc + span; c++) if (occ[cr][c]) ok = false;
                    if (ok) break;
                    cc++;
                }
                c0 = cc; c1 = cc + span;
                if (r0 < 0) { r0 = cr; r1 = cr + 1; }
                cc = c1;
            }
        }
        if (c0 < 0) c0 = 0;
        if (c0 >= nc) c0 = nc - 1;
        if (c1 > nc) c1 = nc;
        if (c1 <= c0) c1 = c0 + 1;
        if (r0 < 0) {                                        /* column given, row free */
            r0 = cr;
            while (r0 < 254 && occ[r0][c0]) r0++;
            r1 = r0 + 1;
        }
        if (r0 > 254) r0 = 254;
        if (r1 > 255) r1 = 255;
        if (r1 <= r0) r1 = r0 + 1;
        for (int r = r0; r < r1; r++) for (int c = c0; c < c1; c++) occ[r][c] = 1;
        it->gx0 = c0; it->gx1 = c1; it->gy0 = r0; it->gy1 = r1;
        if (r1 > nrows) nrows = r1;
    }

    /* columns: fixed, then content sized, then fr takes the rest */
    int cw[12], cx[12], used = gap * (nc - 1), frs = 0;
    for (int c = 0; c < nc; c++) {
        cw[c] = 0;
        if (kind[c] == 0 || kind[c] == 3) cw[c] = trk[c];
        else if (kind[c] == 2) {
            for (int i = 0; i < k; i++)
                if (I[i].gx0 == c && I[i].gx1 == c + 1) { item_size(&I[i], S, avail); if (I[i].mx > cw[c]) cw[c] = I[i].mx; }
        } else frs += trk[c];
        used += cw[c];
    }
    if (used > avail && !g_meas) {                           /* too wide: squeeze the non-fr ones */
        int nf = used - gap * (nc - 1), room = avail - gap * (nc - 1);
        if (room < 0) room = 0;
        for (int c = 0; c < nc; c++) if (nf) cw[c] = (int)((long long)cw[c] * room / nf);
        used = avail;
    }
    int left = avail - used;
    if (left > 0 && frs) for (int c = 0; c < nc; c++) if (kind[c] == 1) cw[c] = (int)((long long)left * trk[c] / frs);
    for (int c = 0, x = x0; c < nc; c++) { cx[c] = x; x += cw[c] + gap; }

    /* rows, top to bottom */
    static int rowy[257];
    int y = L.y;
    for (int r = 0; r < nrows; r++) {
        rowy[r] = y;
        int lh = 0, any = 0;
        for (int i = 0; i < k; i++) {
            item_t* it = &I[i];
            if (it->gy0 != r) continue;
            int w = cx[it->gx1 - 1] + cw[it->gx1 - 1] - cx[it->gx0];
            int cwid = w - it->ml - it->mr;
            it->h = lay_item(it, S, w, cx[it->gx0] + it->ml, y, cwid > 1 ? cwid : 1);
            any = 1;
            if (it->gy1 == r + 1 && it->h > lh) lh = it->h;
            if (cx[it->gx1 - 1] + cw[it->gx1 - 1] > g_maxx) g_maxx = cx[it->gx1 - 1] + cw[it->gx1 - 1];
        }
        for (int i = 0; i < k; i++)                          /* spanning items that end here */
            if (I[i].gy1 == r + 1 && I[i].gy0 < r && rowy[I[i].gy0] + I[i].h > y + lh) lh = rowy[I[i].gy0] + I[i].h - y;
        for (int i = 0; i < k; i++) {
            item_t* it = &I[i];
            if (it->gy0 != r || it->gy1 != r + 1) continue;
            int d = lh - it->h;
            if (d <= 0) continue;
            if (S->ai == 2) shift_item(it, 0, d / 2);
            else if (S->ai == 3) shift_item(it, 0, d);
            else if (S->ai == 0 && it->own && it->b1 > it->b0) boxes[it->b0].h += d;
        }
        y += lh + (any && r + 1 < nrows ? rgap : 0);
    }
    L.y = y;
    n_items = it0;
}

/* tables: rows of cells, column widths from max/min-content */
#define MAX_TCOLS 40
#define MAX_TROWS 2000
static void lay_table(int n, const style_t* S) {
    int it0 = n_items;
    end_line(false);
    flush_margin();
    int bsp = S->bsp, avail = L.x1 - L.x0;
    /* rows: direct, or inside row groups; loose cells become one row (the table itself) */
    static int rows_s[MAX_TROWS], rowb[MAX_TROWS], rowk[MAX_TROWS];
    static int rtop;
    int r0 = rtop, nr = 0;
    bool loose = false;
    for (int c = nodes[n].first; c >= 0 && r0 + nr < MAX_TROWS; c = nodes[c].next) {
        if (nodes[c].text) continue;
        const char* t = nodes[c].tag;
        if (!strcmp(t, "tr")) rows_s[r0 + nr++] = c;
        else if (!strcmp(t, "tbody") || !strcmp(t, "thead") || !strcmp(t, "tfoot")) {
            for (int r = nodes[c].first; r >= 0 && r0 + nr < MAX_TROWS; r = nodes[r].next) if (is_tag(r, "tr")) rows_s[r0 + nr++] = r;
        } else if (!strcmp(t, "caption")) {
            lay_node(c, S);
            end_line(false);
            flush_margin();
        } else if (!loose) { rows_s[r0 + nr++] = n; loose = true; }
    }
    rtop = r0 + nr;
    int colmx[MAX_TCOLS], colmn[MAX_TCOLS], ncol = 0;
    for (int i = 0; i < MAX_TCOLS; i++) colmx[i] = colmn[i] = 0;
    for (int r = 0; r < nr; r++) {
        int rn = rows_s[r0 + r];
        item_t ri;
        memset(&ri, 0, sizeof ri);
        ri.n = rn;
        int nv = n_vars;
        style_t* RS = rn == n ? (style_t*)S : item_st(&ri, S, avail, &nv);
        rowb[r0 + r] = n_items;
        rowk[r0 + r] = 0;
        if (RS->disp != D_NONE) {
            int b;
            int k = get_items(rn, RS, avail, &b, false);
            if (rn == n) {                                /* keep just the cells */
                int w = b;
                for (int i = b; i < b + k; i++) {
                    if (items[i].text || is_tag(items[i].n, "tr") || is_tag(items[i].n, "tbody") || is_tag(items[i].n, "thead") ||
                        is_tag(items[i].n, "tfoot") || is_tag(items[i].n, "caption")) continue;
                    items[w++] = items[i];
                }
                k = w - b;
                n_items = w;
            }
            rowk[r0 + r] = k;
            int col = 0;
            for (int i = 0; i < k; i++) {
                item_t* it = &items[b + i];
                if (col + it->span > MAX_TCOLS) it->span = MAX_TCOLS - col > 0 ? MAX_TCOLS - col : 1;
                item_size(it, RS, avail);
                if (it->span == 1 && col < MAX_TCOLS) {
                    if (it->mx > colmx[col]) colmx[col] = it->mx;
                    if (it->mn > colmn[col]) colmn[col] = it->mn;
                }
                col += it->span;
            }
            if (col > ncol) ncol = col > MAX_TCOLS ? MAX_TCOLS : col;
        }
        if (rn != n) item_rel(RS, S, nv);
    }
    /* spanning cells: spread what's missing */
    for (int r = 0; r < nr; r++) {
        int col = 0;
        for (int i = 0; i < rowk[r0 + r]; i++) {
            item_t* it = &items[rowb[r0 + r] + i];
            if (it->span > 1 && col + it->span <= ncol) {
                int have = 0, hmn = 0;
                for (int c = col; c < col + it->span; c++) { have += colmx[c]; hmn += colmn[c]; }
                if (it->mx > have) for (int c = col; c < col + it->span; c++) colmx[c] += (it->mx - have) / it->span + 1;
                if (it->mn > hmn) for (int c = col; c < col + it->span; c++) colmn[c] += (it->mn - hmn) / it->span + 1;
            }
            col += it->span;
        }
    }
    int sx = 0, sn = 0;
    for (int c = 0; c < ncol; c++) { sx += colmx[c]; sn += colmn[c]; }
    int inner = avail - bsp * (ncol + 1);
    int tgt = sx < inner ? sx : inner;
    if (S->w != LA && !(g_meas && (S->fixh & 2))) tgt = avail - bsp * (ncol + 1);   /* lay_box already sized us */
    if (tgt < sn) tgt = sn;
    int cw[MAX_TCOLS];
    for (int c = 0; c < ncol; c++) {
        if (tgt >= sx) cw[c] = colmx[c] + (sx ? (int)((long long)(tgt - sx) * colmx[c] / sx) : (tgt - sx) / ncol);
        else if (sx > sn) cw[c] = colmn[c] + (int)((long long)(tgt - sn) * (colmx[c] - colmn[c]) / (sx - sn));
        else cw[c] = colmn[c];
    }
    int tw = bsp * (ncol + 1);
    for (int c = 0; c < ncol; c++) tw += cw[c];
    int x0 = L.x0;
    if (!g_meas && S->w == LA && tw < avail && L.bs && L.bs->cblk) x0 += (avail - tw) / 2;
    int y = L.y + bsp;
    for (int r = 0; r < nr; r++) {
        int rn = rows_s[r0 + r];
        item_t ri;
        memset(&ri, 0, sizeof ri);
        ri.n = rn;
        int nv = n_vars;
        style_t* RS = rn == n ? (style_t*)S : item_st(&ri, S, avail, &nv);
        int x = x0 + bsp, col = 0, lh = 0, rbi = -1;
        if (RS != S && RS->bga) rbi = add_box(x0, y, tw, 0, RS->bg, RS->bga, RS->bw, RS->bc, RS->bs, 0, n_runs);
        for (int i = 0; i < rowk[r0 + r]; i++) {
            item_t* it = &items[rowb[r0 + r] + i];
            int w = 0;
            for (int c = col; c < col + it->span && c < ncol; c++) w += cw[c] + (c > col ? bsp : 0);
            it->h = lay_item(it, RS, w, x, y, w > 1 ? w : 1);
            if (it->h > lh) lh = it->h;
            x += w + bsp;
            col += it->span;
        }
        if (RS != S && RS->h != LA && RS->h > lh) lh = RS->h;
        for (int i = 0; i < rowk[r0 + r]; i++) {
            item_t* it = &items[rowb[r0 + r] + i];
            int d = lh - it->h;
            if (d <= 0) continue;
            int dy = it->va == 3 ? d : (it->va == 2 || it->va == 0) ? 0 : d / 2;
            shift(it->r0, it->r1, it->b0 + (it->own ? 1 : 0), it->b1, it->c0, it->c1, 0, dy);
            if (it->own) boxes[it->b0].h += d;
        }
        if (rbi >= 0) boxes[rbi].h = lh;
        if (rn != n) item_rel(RS, S, nv);
        if (x > g_maxx) g_maxx = x;
        y += lh + bsp;
    }
    L.y = y;
    rtop = r0;
    n_items = it0;
}

static void lay_inner(int n, const style_t* S, const style_t* B, const style_t* A) {
    if (n < 0) return;
    if ((S->disp == D_FLEX || S->disp == D_IFLEX) && n_spool + 30 < SPOOL) { lay_flex(n, S); return; }
    if ((S->disp == D_GRID || S->disp == D_IGRID) && n_spool + 30 < SPOOL) { lay_grid(n, S); return; }
    if ((S->disp == D_TABLE || S->disp == D_ITABLE) && n_spool + 30 < SPOOL) { lay_table(n, S); return; }
    lay_children(n, S, B, A);
}

/* ---------- forms ---------- */
static int add_field(int type, attrs_t A) {
    if (n_fields >= MAX_FIELDS) return -1;
    int i = n_fields++;
    field_t* f = &fields[i];
    char name[64];
    if (!get_attr(A, "name", name, sizeof name)) name[0] = 0;
    bool keep = keep_fields && i < n_fields_prev && !strcmp(f->name, name) && f->type == type;
    f->form = L.form;
    f->js = L.js;
    f->type = (uint8_t)type;
    scpy(f->name, sizeof f->name, name);
    if (!keep) {
        if (!get_attr(A, "value", f->value, sizeof f->value)) f->value[0] = 0;
        f->len = (int)strlen(f->value);
        f->cur = f->len;
    }
    return i;
}

static void do_input(attrs_t A, const style_t* S) {
    char type[16];
    if (!get_attr(A, "type", type, sizeof type)) type[0] = 0;
    for (char* p = type; *p; p++) *p = lower(*p);
    if (!strcmp(type, "hidden")) { add_field(FT_HIDDEN, A); return; }
    if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        if (get_attr(A, "checked", NULL, 0)) {
            int i = add_field(FT_HIDDEN, A);
            if (i >= 0 && !fields[i].value[0]) { scpy(fields[i].value, FIELD_MAX, "on"); fields[i].len = 2; }
        }
        return;
    }
    if (!strcmp(type, "submit") || !strcmp(type, "image")) {
        int i = add_field(FT_SUBMIT, A);
        if (i < 0) return;
        static char lab[64];
        if (fields[i].value[0]) scpy(lab, sizeof lab, fields[i].value);
        else if (!get_attr(A, "title", lab, sizeof lab) && !get_attr(A, "alt", lab, sizeof lab)) lab[0] = 0;
        if (!lab[0]) scpy(lab, sizeof lab, "Submit");
        int n = (int)strlen(lab);
        int off = pool_put(lab, n);
        emit_box(RK_BUTTON, uif_width_n(UIF_MED, lab, n) + 28, FIELD_H, i, off, n);
        return;
    }
    if (type[0] && strcmp(type, "text") && strcmp(type, "search") && strcmp(type, "email") &&
        strcmp(type, "url") && strcmp(type, "password") && strcmp(type, "tel") && strcmp(type, "number"))
        return;                                        /* reset, file, button, ... */
    int i = add_field(!strcmp(type, "password") ? FT_PASSWORD : FT_TEXT, A);
    if (i < 0) return;
    char sz[8];
    int w = 280;
    if (get_attr(A, "size", sz, sizeof sz)) { w = atoi(sz) * 9 + 20; }
    if (S->w != LA && S->w > 40) w = S->w + (S->bbox ? 0 : pbw_h(S));
    if (w < 120) w = 120;
    if (w > 600) w = 600;
    int avail = L.x1 - L.x0;
    if (w > avail) w = avail;
    emit_box(RK_FIELD, w, FIELD_H, i, 0, 0);
}

/* all the text inside n, collapsed (button labels) */
static int node_text(int n, char* out, int cap, int k) {
    for (int c = nodes[n].first; c >= 0 && k < cap - 1; c = nodes[c].next) {
        if (nodes[c].text) {
            static char tmp[512];
            int an = nodes[c].an < 500 ? nodes[c].an : 500;
            int m = decode_entities((const char*)resp + nodes[c].a, an, tmp, sizeof tmp);
            for (int i = 0; i < m && k < cap - 1; i++) {
                char ch = isws(tmp[i]) ? ' ' : tmp[i];
                if (ch == ' ' && (!k || out[k - 1] == ' ')) continue;
                out[k++] = ch;
            }
        } else if (!tag_is(nodes[c].tag, T_RAW)) k = node_text(c, out, cap, k);
    }
    out[k] = 0;
    return k;
}

static int lay_node(int n, const style_t* P) {
    style_t S, B, A;
    static char v[URL_MAX], abs_u[URL_MAX];
    int nv = n_vars;
    const char* t = nodes[n].tag;
    compute(n, P, &S, L.x1 - L.x0, &B, &A);
    int d = S.disp;
    int olink = L.link, oform = L.form, ojs = L.js;
    attrs_t At = node_attrs(n);
    if (get_attr(At, "data-sjs", v, 16)) L.js = atoi(v);
    if (d == D_NONE) {
        if (!strcmp(t, "input")) do_input(At, &S);        /* hidden fields still count */
        goto out;
    }
    if (!strcmp(t, "br")) { bool has = n_atoms > L.a0 && L.dirty; end_line(!has); goto out; }
    if (!strcmp(t, "a") && get_attr(At, "href", v, sizeof v)) {
        if (n_links < MAX_LINKS && resolve_url(base_url, v, abs_u)) {
            int off = url_put(abs_u);
            if (off >= 0) { link_off[n_links] = off; L.link = n_links++; }
        }
    }
    if (!strcmp(t, "form")) {
        L.form = -1;
        if (n_forms < MAX_FORMS) {
            form_t* f = &forms[n_forms];
            f->post = get_attr(At, "method", v, 16) && (v[0] == 'p' || v[0] == 'P');
            if (!get_attr(At, "action", v, sizeof v) || !resolve_url(base_url, v, abs_u)) scpy(abs_u, URL_MAX, base_url);
            f->action = url_put(abs_u);
            L.form = n_forms++;
        }
    }
    if (!strcmp(t, "input")) { do_input(At, &S); goto out; }
    if (!strcmp(t, "button")) {
        if (!get_attr(At, "type", v, 16)) scpy(v, 16, "submit");
        if (lower(v[0]) == 's') {
            int i = add_field(FT_SUBMIT, At);
            if (i >= 0) {
                node_text(n, v, 64, 0);
                if (!v[0] || !strcmp(v, " ")) scpy(v, 64, fields[i].value[0] ? fields[i].value : "Submit");
                int ln = (int)strlen(v), off = pool_put(v, ln);
                emit_box(RK_BUTTON, uif_width_n(UIF_MED, v, ln) + 28, FIELD_H, i, off, ln);
            }
            goto out;
        }
    }
    if (!strcmp(t, "textarea")) {
        int i = add_field(FT_TEXT, At);
        if (i >= 0) {
            if (!(keep_fields && i < n_fields_prev)) {
                node_text(n, fields[i].value, FIELD_MAX, 0);
                fields[i].len = fields[i].cur = (int)strlen(fields[i].value);
            }
            int av = L.x1 - L.x0;
            emit_box(RK_FIELD, av < 480 ? av : 480, FIELD_H, i, 0, 0);
        }
        goto out;
    }
    if (!strcmp(t, "img")) {
        /* src, or what lazy loaders keep the real one in */
        int ii = -1;
        static const char* const srcs[] = { "data-src", "data-lazy-src", "data-original", "src", 0 };
        for (int k = 0; srcs[k] && ii < 0; k++)
            if (get_attr(At, srcs[k], v, sizeof v) && v[0] && !starts_ci(v, "data:") && resolve_url(base_url, v, abs_u))
                ii = img_find(abs_u);
        if (ii < 0 && get_attr(At, "srcset", v, sizeof v) && v[0]) {
            char* e = v;
            while (*e && *e != ' ' && *e != ',') e++;
            *e = 0;
            if (resolve_url(base_url, v, abs_u)) ii = img_find(abs_u);
        }
        int w = S.w, h = S.h;
        bool ok = ii >= 0 && imgs[ii].st == IM_OK;
        if (ok) {
            int nw = imgs[ii].w, nh = imgs[ii].h;
            if (w == LA && h == LA) { w = nw; h = nh; }
            else if (w == LA) w = nh ? h * nw / nh : 0;
            else if (h == LA) h = nw ? w * nh / nw : 0;
        }
        int avail = L.x1 - L.x0 - S.m[1] - S.m[3];
        if (w != LA && h != LA) {
            if (S.maxw != LA && w > S.maxw) { h = h * S.maxw / w; w = S.maxw; }
            if (w > avail && avail > 0 && !g_meas) { h = h * avail / w; w = avail; }
        }
        bool failed = ii < 0 || imgs[ii].st == IM_BAD;
        if (w != LA && h != LA && w > 0 && h > 0 && !(failed && get_attr(At, "alt", NULL, 0) && w > 40)) {
            bool blk = S.disp == D_BLOCK || S.disp == D_FLEX || S.disp == D_TABLE;
            if (blk) { end_line(false); add_margin(S.m[0]); flush_margin(); }
            if (S.flt && n_flts < 64) {
                if (n_atoms > L.a0) end_line(false);
                flush_margin();
                line_bounds();
                int tot = w + S.m[1] + S.m[3];
                int x = S.flt == 1 || g_meas ? L.lx0 + S.m[3] : L.lx1 - S.m[1] - w;
                int y = L.y + S.m[0];
                run_t* r = add_run(RK_IMAGE, x, y, w, h, 0, S.pbg, 0, 0);
                if (r) r->field = (int16_t)ii;
                flts[n_flts++] = (flt_t){ x - S.m[3], x + w + S.m[1], L.y, y + h + S.m[2], S.flt };
                if (x + w > g_maxx) g_maxx = x + w;
                if (tot > g_minw) g_minw = tot;
                goto out;
            }
            L.pend += S.m[3];
            emit_box(RK_IMAGE, w, h, ii, 0, 0);
            if (n_runs) runs[n_runs - 1].fg = S.pbg;
            if (n_atoms > L.a0) { atoms[n_atoms - 1].asc = h; atoms[n_atoms - 1].va = S.va; }
            L.lx += S.m[1];
            if (blk) { end_line(false); add_margin(S.m[2]); }
            goto out;
        }
        if (get_attr(At, "alt", v, 200) && v[0]) {
            style_t T = S;
            if (T.fs > 13) T.fs = 13;
            bool sp = L.space;
            L.space = sp;
            emit_text(&T, v, (int)strlen(v));
        }
        goto out;
    }
    if (!strcmp(t, "hr") && !S.bw[0] && !S.bw[2] && !S.bga) { S.bw[0] = 1; S.bs[0] = 1; S.bc[0] = PAGE_RULE; }
    switch (d) {
    case D_INLINE:
    case D_CONTENTS:
        lay_inline(n, &S, &B, &A);
        break;
    case D_IBLOCK: case D_IFLEX: case D_IGRID: case D_ITABLE:
        lay_atomic(n, &S, &B, &A);
        break;
    default:
        if (S.pos >= 2) lay_abs(n, &S, &B, &A);
        else if (S.flt) lay_float(n, &S, &B, &A);
        else lay_flow_block(n, &S, &B, &A);
        break;
    }
out:
    L.link = olink;
    L.js = ojs;
    if (!strcmp(t, "form")) L.form = oform;
    n_vars = nv;
    return d;
}

/* title, base, meta refresh, and every stylesheet */
static void pre_pass(void) {
    static char v[URL_MAX], abs_u[URL_MAX];
    for (int n = 1; n < n_nodes; n++) {
        node_t* N = &nodes[n];
        if (N->text) continue;
        attrs_t A = node_attrs(n);
        if (!strcmp(N->tag, "title") && !page_title[0] && N->first >= 0) {
            int c = N->first;
            static char tmp[256];
            int an = nodes[c].an < 250 ? nodes[c].an : 250;
            int m = decode_entities((const char*)resp + nodes[c].a, an, tmp, sizeof tmp);
            int o = 0;
            bool sp = false;
            for (int i = 0; i < m && o < (int)sizeof page_title - 1; i++) {
                if (isws(tmp[i])) { sp = o > 0; continue; }
                if (sp) { page_title[o++] = ' '; sp = false; if (o >= (int)sizeof page_title - 1) break; }
                page_title[o++] = tmp[i];
            }
            page_title[o] = 0;
        } else if (!strcmp(N->tag, "base")) {
            if (get_attr(A, "href", v, sizeof v) && resolve_url(base_url, v, abs_u)) scpy(base_url, URL_MAX, abs_u);
        } else if (!strcmp(N->tag, "meta")) {
            if (get_attr(A, "http-equiv", v, 32) && starts_ci(v, "refresh") && get_attr(A, "content", v, sizeof v)) {
                const char* u = find_ci(v, (int)strlen(v), "url=");
                if (atoi(v) <= 5 && u) {
                    u += 4;
                    if (*u == '\'' || *u == '"') u++;
                    scpy(abs_u, URL_MAX, u);
                    int k = (int)strlen(abs_u);
                    if (k && (abs_u[k - 1] == '\'' || abs_u[k - 1] == '"')) abs_u[k - 1] = 0;
                    if (!resolve_url(base_url, abs_u, refresh_url)) refresh_url[0] = 0;
                }
            }
        } else if (!strcmp(N->tag, "style") && N->first >= 0) {
            if (get_attr(A, "media", v, 200) && !media_ok(v, (int)strlen(v))) continue;
            node_t* T = &nodes[N->first];
            css_strip((char*)resp + T->a, T->an);
            css_parse((const char*)resp + T->a, T->an, 0);
        }
    }
}

static void layout(int view_w) {
    n_fields_prev = n_fields;
    n_runs = n_links = n_fields = n_forms = 0;
    pool_used = url_used = 0;
    page_title[0] = 0;
    refresh_url[0] = 0;
    n_boxes = n_clips = n_atoms = n_ibs = n_frags = n_flts = 0;
    n_spool = n_items = 0;
    n_vars = 0;
    page_bg_set = false;
    g_meas = false;
    g_maxx = g_minw = 0;
    view_w_l = view_w;
    g_vw = view_w;
    g_vh = view_box.h > 200 ? view_box.h : 700;

    static style_t R;
    memset(&R, 0, sizeof R);
    R.fg = COL_TEXT; R.pbg = COL_PAPER; R.fs = 16; R.opa = 255; R.lst = LS_DISC;
    style_reset(&R);
    R.disp = D_BLOCK;
    R.cbw = view_w;

    memset(&L, 0, sizeof L);
    L.x0 = 0; L.x1 = view_w; L.y = 0;
    L.link = -1; L.form = -1; L.clip = -1; L.js = -1;
    L.bs = &R; L.first = true;
    cb_x = 0; cb_y = 0; cb_w = view_w;

    const char* s = (const char*)resp;
    int n = resp_len;
    if (plain_text) {
        R.ws = 2; R.mono = 1;
        L.x0 = PAD; L.x1 = view_w - PAD; L.y = 14;
        emit_text(&R, s, n);
        end_line(false);
        doc_h = L.y + PAD;
        keep_fields = false;
        return;
    }
    dom_build(s, n);
    n_rules = n_comps = n_decls = n_aconds = 0;
    gta_used = 0;
    for (int i = 0; i < 1024; i++) bucket[i] = -1;
    ubucket = -1;
    css_parse(UA_CSS, (int)sizeof UA_CSS - 1, 0);
    n_ua = n_rules;
    pre_pass();
    for (int i = 0; i < n_rules; i++) rules[i].stamp = 0;
    g_stamp = 0;

    /* canvas colour: html's background, else body's */
    int html = -1, body = -1;
    for (int c = nodes[0].first; c >= 0; c = nodes[c].next) if (is_tag(c, "html")) html = c;
    if (html >= 0) for (int c = nodes[html].first; c >= 0; c = nodes[c].next) if (is_tag(c, "body")) body = c;
    if (html >= 0) {
        style_t H, Bd;
        compute(html, &R, &H, view_w, NULL, NULL);
        if (H.bga > 128) { page_bg = H.bg; page_bg_set = true; }
        if (body >= 0) {
            compute(body, &H, &Bd, view_w, NULL, NULL);
            if (!page_bg_set && Bd.bga > 128) { page_bg = Bd.bg; page_bg_set = true; }
        }
        n_vars = 0;
    }
    if (page_bg_set) {
        R.pbg = page_bg;
        if (lum(page_bg) < 1100) R.fg = RGB(0xE6, 0xE6, 0xE6);   /* dark page, no colour set: light text */
    }
    L.x0 = PAD / 2; L.x1 = view_w - PAD / 2;
    L.y = 6;
    lay_children(0, &R, NULL, NULL);
    end_line(false);
    flush_margin();
    for (int i = 0; i < n_flts; i++) if (flts[i].y1 > L.y) L.y = flts[i].y1;
    doc_h = L.y + PAD;
    for (int i = 0; i < n_runs; i++) if (runs[i].y + runs[i].h + PAD > doc_h && runs[i].y < 200000) doc_h = runs[i].y + runs[i].h + PAD;
    /* boxes drawn in open order: sort by the run they go under */
    for (int i = 1; i < n_boxes; i++) {
        bx_t b = boxes[i];
        int j = i - 1;
        while (j >= 0 && boxes[j].r_at > b.r_at) { boxes[j + 1] = boxes[j]; j--; }
        boxes[j + 1] = b;
    }
    keep_fields = false;
}
/* ======================================================================
   Pages: fetch through curl, history, forms
   ====================================================================== */

#define DOC_MAX_W 1400
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
static void img_kill(void);
static bool img_dirty;

static void commit(const char* url, bool push) {
    img_kill();
    img_reset();
    img_dirty = false;
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

static void img_kill(void) {
    if (img_pid > 0 && proc_alive(img_pid)) {
        proc_t* p = proc_by_pid(img_pid);
        if (p) proc_send_signal(p, 9);
    }
    img_pid = -1;
    if (img_cur >= 0 && imgs[img_cur].st == IM_LOAD) imgs[img_cur].st = IM_WAIT;
    img_cur = -1;
}


/* one image at a time through curl; the page is laid out again when some arrived */
static void img_pump(void) {
    if (img_pid > 0) {
        if (proc_alive(img_pid)) return;
        img_pid = -1;
        img_t* im = &imgs[img_cur];
        im->st = IM_BAD;
        fs_node_t* n = fs_resolve(fs_root(), IMG_FILE);
        if (n && n->data && n->size > 0 && n->size < 4000000) {
            int w, h;
            uint32_t* px = img_decode((const uint8_t*)n->data, (int)n->size, &w, &h);
            if (px) { im->px = px; im->w = w; im->h = h; im->st = IM_OK; img_dirty = true; }
        }
        // kprintf("img %s: %d\n", im->url, im->st);
        fs_unlink(fs_root(), IMG_FILE);
        img_cur = -1;
    }
    int next = -1;
    for (int i = 0; i < n_imgs; i++) if (imgs[i].st == IM_WAIT) { next = i; break; }
    if (img_dirty && (next < 0 || (int32_t)(now_ms - img_relayout_at) > 700)) {
        img_dirty = false;
        img_relayout_at = now_ms;
        laid_w = layout_w();
        keep_fields = true;
        layout(laid_w);
        if (g_win) g_win->needs_repaint = true;
    }
    if (next < 0) return;
    static char wire[URL_MAX * 3], ref[URL_MAX * 3];
    wire_url(imgs[next].url, wire, sizeof wire);
    wire_url(base_url, ref, sizeof ref);
    char* argv[] = { (char*)"curl", (char*)"-sS", (char*)"-L", (char*)"-m", (char*)"20", (char*)"--max-filesize",
                     (char*)"3500000", (char*)"-A", (char*)USER_AGENT, (char*)"-e", ref, (char*)"-o", (char*)IMG_FILE, wire, NULL };
    char* envp[] = { (char*)"PATH=/bin:/usr/bin", (char*)"HOME=/root",
                     (char*)"CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt", NULL };
    fs_unlink(fs_root(), IMG_FILE);
    img_pid = proc_spawn_detached(CURL, argv, envp);
    if (img_pid < 0) { img_pid = -1; imgs[next].st = IM_BAD; return; }
    imgs[next].st = IM_LOAD;
    img_cur = next;
}

static void js_kill_live(void) {
    if (js_live > 0 && proc_alive(js_live)) {
        proc_t* p = proc_by_pid(js_live);
        if (p) proc_send_signal(p, 9);
    }
    js_live = -1;
}

/* one line for the live domjs: "click 12", "val 7 text", "enter 7" */
static void js_send(const char* kind, int id, const char* val, int vlen) {
    fs_node_t* root = fs_root();
    fs_node_t* f = fs_resolve(root, JS_EV);
    if (!f) f = fs_create(root, JS_EV, FS_FILE);
    if (!f) return;
    static char line[FIELD_MAX * 4 + 32];
    char num[12];
    scpy(line, sizeof line, kind);
    scat(line, sizeof line, " ");
    itoa(id, num, 10);
    scat(line, sizeof line, num);
    if (val) {
        scat(line, sizeof line, " ");
        int p = (int)strlen(line);
        form_encode(val, vlen, line, &p, sizeof line - 2);  /* %XX utf-8, js decodes it */
    }
    scat(line, sizeof line, "\n");
    fs_append(f, line, strlen(line));
}

/* before a click: what the user typed goes to the page first */
static void js_send_fields(void) {
    for (int i = 0; i < n_fields; i++)
        if (fields[i].js > 0 && fields[i].type != FT_HIDDEN) js_send("val", fields[i].js, fields[i].value, fields[i].len);
}

static int  read_file(const char* path, char* out, int cap);
static void go(const char* url, const char* post);
static void clamp_scroll(void);

static void js_poll(void) {
    if (js_live <= 0) return;
    if (!proc_alive(js_live)) { js_live = -1; return; }
    fs_node_t* root = fs_root();
    static char buf[URL_MAX * 3 + 4096];
    int n = read_file(JS_NAV, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        fs_unlink(root, JS_NAV);
        char* nl = strchr(buf, '\n');
        char* post = NULL;
        if (nl) { *nl = 0; if (nl[1]) post = nl + 1; }
        int m = utf8_to_cp866((uint8_t*)buf, (int)strlen(buf));
        buf[m] = 0;
        static char u[URL_MAX];
        scpy(u, URL_MAX, buf);
        go(u, post);
        return;
    }
    char sq[16];
    n = read_file(JS_SEQ, sq, sizeof sq - 1);
    if (n <= 0) return;
    sq[n] = 0;
    int s = atoi(sq);
    if (s <= js_seq) return;
    js_seq = s;
    n = read_file(JS_OUT, (char*)resp, RESP_CAP - 1);
    if (n <= 20) return;
    resp_len = utf8_to_cp866(resp, n);
    plain_text = false;
    keep_fields = true;
    laid_w = layout_w();
    layout(laid_w);
    clamp_scroll();
    if (g_win) g_win->needs_repaint = true;
}

static void kill_fetch(void) {
    img_kill();
    js_kill_live();
    if (js_pending) {
        if (js_pid > 0 && proc_alive(js_pid)) {
            proc_t* jp = proc_by_pid(js_pid);
            if (jp) proc_send_signal(jp, 9);
        }
        js_pid = -1;
        js_pending = false;
    }
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

static void finish_page(int n, const char* eff_in, const char* ctype, int code, bool from_js);

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
    fs_unlink(root, META_FILE);
    fs_unlink(root, ERR_FILE);

    /* pages with scripts go through domjs first; the page file stays until it is done */
    if (n > 0 && (!ctype[0] || find_ci(ctype, (int)strlen(ctype), "html")) &&
        (find_ci((const char*)resp, n, "<script") || find_ci((const char*)resp, n, "stylesheet")) && fs_resolve(root, DOMJS)) {
        char* jargv[] = { (char*)"domjs", (char*)PAGE_FILE, (char*)JS_OUT, (char*)eff, NULL };
        char* jenv[] = { (char*)"PATH=/bin:/usr/bin", (char*)"HOME=/root",
                         (char*)"CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt", NULL };
        fs_unlink(root, JS_OUT);
        fs_unlink(root, JS_SEQ);
        fs_unlink(root, JS_NAV);
        fs_unlink(root, JS_EV);
        js_pid = proc_spawn_detached(DOMJS, jargv, jenv);
        if (js_pid > 0) {
            scpy(js_eff, URL_MAX, eff);
            scpy(js_ctype, sizeof js_ctype, ctype);
            js_code = code;
            js_pending = loading = true;
            set_status("Running scripts...", NULL);
            return;
        }
    }
    fs_unlink(root, PAGE_FILE);
    finish_page(n, eff, ctype, code, false);
}

static void finish_js(void) {
    fs_node_t* root = fs_root();
    js_pending = false;
    loading = false;
    if (js_pid > 0 && proc_alive(js_pid)) {
        if (fs_resolve(root, JS_SEQ)) { js_live = js_pid; js_seq = 1; }   /* it stays for clicks and timers */
        else {
            proc_t* p = proc_by_pid(js_pid);
            if (p) proc_send_signal(p, 9);
        }
    }
    js_pid = -1;
    int n = read_file(JS_OUT, (char*)resp, RESP_CAP - 1);
    bool ok = n > 20;
    if (!ok) n = read_file(PAGE_FILE, (char*)resp, RESP_CAP - 1);
    if (n < 0) n = 0;
    fs_unlink(root, PAGE_FILE);
    fs_unlink(root, JS_OUT);
    static char eff_u[URL_MAX], ct[80];
    scpy(eff_u, URL_MAX, js_eff);
    scpy(ct, sizeof ct, js_ctype);
    finish_page(n, eff_u, ct, js_code, ok);
}

static void finish_page(int n, const char* eff_in, const char* ctype, int code, bool from_js) {
    static char eff_u[URL_MAX];
    scpy(eff_u, URL_MAX, eff_in);
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
    if (from_js) { w1251 = false; cs = ctype; }         /* domjs writes utf-8 */
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
    if (hov) gfx_rect_fill(b.x, b.y, b.w, b.h, bg);
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

/* Text box; caret at `cur` when focused. Text scrolls to keep it visible.
   dark = the address bar up in the chrome, else a field on the page */
#define URL_BG RGB(0x10, 0x11, 0x13)
static void draw_textbox(int x, int y, int w, int h, const char* s, int len, int cur,
                         bool foc, const char* placeholder, bool dark) {
    gfx_rect_fill(x, y, w, h, foc ? C_ACCENT : dark ? C_OUTLINE : PAGE_RULE);
    gfx_rect_fill(x + 1, y + 1, w - 2, h - 2, dark ? URL_BG : C_WHITE);
    uint32_t ink = dark ? C_INK : PAGE_INK, dim = dark ? C_INK_DIM : PAGE_DIM;
    int tx = x + 12, avail = w - 24;
    int cw = uif_width_n(UIF_REG, s, cur);
    int off = cw > avail ? cw - avail : 0;
    clip_to(x + 4, y + 2, w - 8, h - 4);
    if (!len && placeholder) uif_draw_mid(tx, y + h / 2, UIF_REG, placeholder, dim);
    if (foc && s == edit && edit_all)
        gfx_rect_fill(tx - 2 - off, y + 6, uif_width_n(UIF_REG, s, len) + 4, h - 12,
                      dark ? RGB(0x5A, 0x40, 0x18) : RGB(0xF6, 0xDC, 0xB0));
    static char tmp[URL_MAX];
    int n = len < URL_MAX - 1 ? len : URL_MAX - 1;
    memcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    uif_draw_mid(tx - off, y + h / 2, UIF_REG, tmp, ink);
    if (foc && !(s == edit && edit_all) && (now_ms / 530) % 2 == 0) gfx_rect_fill(tx - off + cw, y + 7, 2, h - 14, C_ACCENT);
    clip_restore();
}

/* Address bar while not editing: host dark, the rest dimmed. */
static void draw_url_view(int x, int y, int w, int h) {
    gfx_rect_fill(x, y, w, h, C_OUTLINE);
    gfx_rect_fill(x + 1, y + 1, w - 2, h - 2, URL_BG);
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

static void draw_str(int x, int y, run_t* r, const char* s, uint32_t c) {
    if (r->ls) {
        char ch[2] = { 0, 0 };
        for (int i = 0; i < r->len; i++) {
            ch[0] = s[i];
            uif_draw_scaled(x, y, r->font, ch, c, r->scale);
            x += text_w(r->font, ch, 1) * r->scale + r->ls;
        }
        return;
    }
    uif_draw_scaled(x, y, r->font, s, c, r->scale);
}

/* w x h copy of the image, alpha over bg; cached for the last size asked */
static uint32_t* img_scaled(img_t* im, int w, int h, uint32_t bg) {
    if (w <= 0 || h <= 0 || w > 4000 || h > 4000) return NULL;
    if (im->sc && im->sw == w && im->sh == h && im->sbg == bg) return im->sc;
    if (im->sc) kfree(im->sc);
    im->sc = kmalloc_big((uint32_t)(w * h * 4));
    if (!im->sc) return NULL;
    im->sw = w; im->sh = h; im->sbg = bg;
    bool down = im->w >= 2 * w && im->h >= 2 * h;
    for (int y = 0; y < h; y++) {
        int sy = (int)((long long)y * im->h / h), sy2 = sy + (down ? im->h / h / 2 : 0);
        if (sy2 >= im->h) sy2 = im->h - 1;
        for (int x = 0; x < w; x++) {
            int sx = (int)((long long)x * im->w / w), sx2 = sx + (down ? im->w / w / 2 : 0);
            if (sx2 >= im->w) sx2 = im->w - 1;
            uint32_t p = im->px[sy * im->w + sx];
            if (down) {                                /* 2x2 average when shrinking a lot */
                uint32_t q[4] = { p, im->px[sy * im->w + sx2], im->px[sy2 * im->w + sx], im->px[sy2 * im->w + sx2] };
                uint32_t a = 0, r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) { a += q[k] >> 24; r += (q[k] >> 16) & 255; g += (q[k] >> 8) & 255; b += q[k] & 255; }
                p = (a / 4) << 24 | (r / 4) << 16 | (g / 4) << 8 | (b / 4);
            }
            uint32_t al = p >> 24;
            im->sc[y * w + x] = al == 255 ? p & 0xFFFFFF : mix(bg, p & 0xFFFFFF, (int)al);
        }
    }
    return im->sc;
}

static void draw_run(int idx, int ox, int oy) {
    run_t* r = &runs[idx];
    int x = ox + r->x, y = oy + r->y;
    if (r->flags & RF_HIDE && r->kind == RK_TEXT) return;
    switch (r->kind) {
    case RK_TEXT: {
        bool hov = r->link >= 0 && r->link == hover_link;
        uint32_t c = hov ? COL_LINK_HOV : r->fg;
        const char* s = pool + r->off;
        int asc = font_asc(r->font) * r->scale;
        if (r->flags & RF_ITALIC) {
            /* fake italic: draw it in thin bands, each shifted a bit */
            int kx, ky, kw, kh;
            gfx_get_clip(&kx, &ky, &kw, &kh);
            for (int b = 0; b < r->h; b += 2) {
                int y0 = y + b > ky ? y + b : ky, y1 = y + b + 2 < ky + kh ? y + b + 2 : ky + kh;
                if (y1 <= y0) continue;
                gfx_set_clip(kx, y0, kw, y1 - y0);
                draw_str(x + (asc - b) / 5, y, r, s, c);
            }
            gfx_set_clip(kx, ky, kw, kh);
        } else draw_str(x, y, r, s, c);
        int th = r->scale > 1 ? r->scale : 1;
        if (r->flags & RF_UL) {
            if (r->link >= 0 && !hov) gfx_rect_blend(x, y + asc + 2, r->w, th, c, 120);
            else gfx_rect_fill(x, y + asc + 2, r->w, th, c);
        }
        if (r->flags & RF_STRIKE) gfx_rect_fill(x, y + asc * 2 / 3, r->w, th, c);
        if (r->flags & RF_OVER) gfx_rect_fill(x, y + 1, r->w, th, c);
        break;
    }
    case RK_RULE:
        gfx_rect_fill(x, y, r->w, 1, PAGE_RULE);
        break;
    case RK_BULLET: {
        int my = y + r->h / 2 + 1;
        if (r->flags == LS_CIRCLE) gfx_circle(x + 3, my, 3, r->fg);
        else if (r->flags == LS_SQUARE) gfx_rect_fill(x + 1, my - 2, 5, 5, r->fg);
        else gfx_disc(x + 3, my, r->flags == LS_OPEN ? 2 : 3, r->fg);
        break;
    }
    case RK_FIELD: {
        field_t* f = &fields[r->field];
        static char stars[FIELD_MAX];
        const char* s = f->value;
        if (f->type == FT_PASSWORD) {
            for (int i = 0; i < f->len; i++) stars[i] = '*';
            stars[f->len] = 0;
            s = stars;
        }
        draw_textbox(x, y, r->w, r->h, s, f->len, f->cur, focus == r->field, NULL, false);
        clip_to(view_box.x, view_box.y, view_box.w, view_box.h);
        break;
    }
    case RK_IMAGE: {
        img_t* im = r->field >= 0 && r->field < n_imgs ? &imgs[r->field] : NULL;
        uint32_t* s = im && im->st == IM_OK ? img_scaled(im, r->w, r->h, r->fg) : NULL;
        if (s) gfx_blit_argb(x, y, r->w, r->h, s);
        else if (r->w > 8 && r->h > 8 && im && im->st != IM_BAD) gfx_rect(x, y, r->w, r->h, PAGE_RULE);   /* still coming */
        if (r->link >= 0 && r->link == hover_link) gfx_rect(x, y, r->w, r->h, COL_LINK_HOV);
        break;
    }
    case RK_BUTTON: {
        bool hov = hover_run == idx;
        gfx_rrect_fill(x, y, r->w, r->h, 8, GFX_CORNERS_ALL, hov ? COL_BTN_HOT : C_ACCENT);
        uif_draw_center(x, y, r->w, r->h, UIF_MED, pool + r->off, PAGE_INK);
        break;
    }
    }
}

static void side_line(int x, int y, int w, int h, int st, uint32_t c) {
    if (st <= 1 || (w > h ? w : h) < 8) { gfx_rect_fill(x, y, w, h, c); return; }
    int on = st == 2 ? 6 : 2, off = st == 2 ? 4 : 2;
    if (w >= h) for (int i = 0; i < w; i += on + off) gfx_rect_fill(x + i, y, i + on > w ? w - i : on, h, c);
    else for (int i = 0; i < h; i += on + off) gfx_rect_fill(x, y + i, w, i + on > h ? h - i : on, c);
}

static void draw_box(bx_t* b, int ox, int oy) {
    int x = ox + b->x, y = oy + b->y, w = b->w, h = b->h;
    if (w <= 0 || h <= 0) return;
    int r = b->rad;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    bool bord = b->bw[0] || b->bw[1] || b->bw[2] || b->bw[3];
    bool same = b->bw[0] == b->bw[1] && b->bw[1] == b->bw[2] && b->bw[2] == b->bw[3] &&
                b->bc[0] == b->bc[1] && b->bc[1] == b->bc[2] && b->bc[2] == b->bc[3] && b->bs[0] <= 1;
    if (r >= 2 && bord && same && b->bga == 255) {
        int t = b->bw[0];
        gfx_rrect_fill(x, y, w, h, r, GFX_CORNERS_ALL, b->bc[0]);
        if (w > 2 * t && h > 2 * t) gfx_rrect_fill(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, GFX_CORNERS_ALL, b->bg);
        return;
    }
    if (b->bga == 255) {
        if (r >= 2) gfx_rrect_fill(x, y, w, h, r, GFX_CORNERS_ALL, b->bg);
        else gfx_rect_fill(x, y, w, h, b->bg);
    } else if (b->bga) gfx_rect_blend(x, y, w, h, b->bg, b->bga);
    if (b->img >= 0 && b->img < n_imgs && imgs[b->img].st == IM_OK) {
        img_t* im = &imgs[b->img];
        int kx, ky, kw, kh;
        gfx_get_clip(&kx, &ky, &kw, &kh);
        int x0 = x > kx ? x : kx, y0 = y > ky ? y : ky;
        int x1 = x + w < kx + kw ? x + w : kx + kw, y1 = y + h < ky + kh ? y + h : ky + kh;
        if (x1 > x0 && y1 > y0) {
            gfx_set_clip(x0, y0, x1 - x0, y1 - y0);
            uint32_t bg = b->bga == 255 ? b->bg : b->pbg;
            int iw = im->w, ih = im->h;
            if (b->bgsz) {                               /* cover / contain */
                bool cov = b->bgsz == 1;
                if (cov ? (long long)w * ih > (long long)h * iw : (long long)w * ih < (long long)h * iw) { ih = (int)((long long)ih * w / iw); iw = w; }
                else { iw = (int)((long long)iw * h / ih); ih = h; }
            }
            uint32_t* s = img_scaled(im, iw, ih, bg);
            if (s) {
                if (b->bgsz || (b->bgrep & 1)) {
                    int px = (b->bgsz || (b->bgrep & 2)) ? x + (w - iw) / 2 : x;
                    int py = (b->bgsz || (b->bgrep & 2)) ? y + (h - ih) / 2 : y;
                    gfx_blit_argb(px, py, iw, ih, s);
                } else
                    for (int ty = y; ty < y + h && ty < y1; ty += ih)
                        for (int tx = x; tx < x + w && tx < x1; tx += iw) gfx_blit_argb(tx, ty, iw, ih, s);
            }
            gfx_set_clip(kx, ky, kw, kh);
        }
    }
    if (!bord) return;
    if (b->bw[0]) side_line(x, y, w, b->bw[0], b->bs[0], b->bc[0]);
    if (b->bw[2]) side_line(x, y + h - b->bw[2], w, b->bw[2], b->bs[2], b->bc[2]);
    if (b->bw[3]) side_line(x, y, b->bw[3], h, b->bs[3], b->bc[3]);
    if (b->bw[1]) side_line(x + w - b->bw[1], y, b->bw[1], h, b->bs[1], b->bc[1]);
}

static void use_clip(int id, int ox, int oy) {
    int x0 = view_box.x, y0 = view_box.y, x1 = x0 + view_box.w, y1 = y0 + view_box.h;
    for (int c = id, k = 0; c >= 0 && c < n_clips && k < 40; c = clips[c].up, k++) {
        clip_t* q = &clips[c];
        if (ox + q->x > x0) x0 = ox + q->x;
        if (oy + q->y > y0) y0 = oy + q->y;
        if (ox + q->x + q->w < x1) x1 = ox + q->x + q->w;
        if (oy + q->y + q->h < y1) y1 = oy + q->y + q->h;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    clip_to(x0, y0, x1 - x0, y1 - y0);
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
    if (focus == FOCUS_URL) draw_textbox(ux, by, uw, BTN, edit, edit_len, edit_cur, true, "Search or type an address", true);
    else draw_url_view(ux, by, uw, BTN);
    if (loading) {
        int seg = cw / 4, span = cw + seg;
        int el = (int32_t)(now_ms - load_start);
        int p = (el < 0 ? 0 : el) / 2 % span - seg;
        int x0 = p < 0 ? 0 : p, x1 = p + seg > cw ? cw : p + seg;
        if (x1 > x0) gfx_rect_fill(cx + x0, cy + TB_H - 2, x1 - x0, 2, C_ACCENT);
    }

    /* page */
    gfx_rect_fill(view_box.x, view_box.y, view_box.w, view_box.h, page_bg_set ? page_bg : COL_PAPER);
    clamp_scroll();
    clip_to(view_box.x, view_box.y, view_box.w, view_box.h);
    int ox = doc_ox(), oy = doc_oy();
    /* boxes go under the run they were opened before */
    int cur = -2, bi = 0;
    for (int i = 0; i <= n_runs; i++) {
        for (; bi < n_boxes && boxes[bi].r_at <= i; bi++) {
            bx_t* b = &boxes[bi];
            if (b->y + b->h < scroll_y || b->y > scroll_y + view_box.h) continue;
            if (b->clip != cur) { cur = b->clip; use_clip(cur, ox, oy); }
            draw_box(b, ox, oy);
        }
        if (i == n_runs) break;
        run_t* r = &runs[i];
        if (r->y + r->h < scroll_y || r->y > scroll_y + view_box.h) continue;
        if (r->clip != cur) { cur = r->clip; use_clip(cur, ox, oy); }
        draw_run(i, ox, oy);
        if (r->kind == RK_FIELD) cur = -2;
    }
    clip_to(view_box.x, view_box.y, view_box.w, view_box.h);
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
    for (int i = n_runs - 1; i >= 0; i--) {         /* topmost first */
        run_t* r = &runs[i];
        if (r->link < 0 && r->kind != RK_FIELD && r->kind != RK_BUTTON && !(js_live > 0 && r->js > 0)) continue;
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
        if (js_pending) {
            if (js_pid < 0 || !proc_alive(js_pid) || fs_resolve(fs_root(), JS_SEQ) || (int32_t)(now - load_start) > 40000) finish_js();
        }
        else if (fetch_pid < 0 || !proc_alive(fetch_pid)) finish_fetch();
        else if ((int32_t)(now - load_start) > 60000) {
            set_status("Timed out", NULL);
            kill_fetch();
            show_error(load_url, "The site took too long", "No complete answer in 60 seconds.");
        }
        w->needs_repaint = true;
    }
    if (!loading) { img_pump(); js_poll(); }
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
    if (rr->link >= 0) { go(url_pool + link_off[rr->link], NULL); return; }
    if (js_live > 0 && rr->js > 0) { js_send_fields(); js_send("click", rr->js, NULL, 0); return; }
    if (rr->kind == RK_BUTTON) { submit_form(fields[rr->field].form, rr->field); return; }
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
        if (c == '\n' || c == '\r') {
            if (js_live > 0 && f->js > 0) { js_send_fields(); js_send("enter", f->js, NULL, 0); return; }
            submit_form(f->form, -1);
            return;
        }
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
        nodes    = kmalloc_big(sizeof(node_t) * MAX_NODES);
        clsh     = kmalloc_big(sizeof(uint32_t) * MAX_CLS);
        comps    = kmalloc_big(sizeof(comp_t) * MAX_COMPS);
        rules    = kmalloc_big(sizeof(rule_t) * MAX_RULES);
        decls    = kmalloc_big(sizeof(decl_t) * MAX_DECLS);
        aconds   = kmalloc_big(sizeof(acond_t) * MAX_ACONDS);
        boxes    = kmalloc_big(sizeof(bx_t) * MAX_BOXES);
        if (!resp || !pool || !url_pool || !runs || !nodes || !clsh || !comps || !rules || !decls || !aconds || !boxes) {
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
    int ww = 1320, wh = 800;
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
