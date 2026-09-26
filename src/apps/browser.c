/* SamaraOS Web Browser — HTTP-only, numeric IPs only, tiny HTML renderer.
   Single-window WM app. Fetches with net_http_get(), strips HTML tags,
   wraps text, draws clickable links. URL bar at top, status bar at bottom. */

#include "apps/browser.h"
#include "gui/wm.h"
#include "gfx/gfx.h"
#include "core/string.h"
#include "core/heap.h"
#include "drivers/keyboard.h"
#include "boot/pit.h"
#include "net/net.h"
#include "gfx/font.h"
#include "drivers/mouse.h"

/* ---------- tunables ---------- */
#define RESP_CAP        (256 * 1024)   /* HTTP response cap */
#define TEXT_POOL_CAP   (128 * 1024)   /* doc word storage */
#define MAX_RUNS        4096           /* drawable text runs */
#define MAX_LINKS       512            /* anchor targets */
#define URL_MAX         256
#define HIST_MAX        32

#define LINE_H          18             /* gap between text lines (font is 16) */
#define HEAD_H          24
#define SCROLL_STEP     LINE_H
#define PAGE_STEP       (LINE_H * 10)

/* ---------- colours ---------- */
#define COL_PAGE_BG     RGB(0xFA, 0xFB, 0xFE)
#define COL_TEXT        RGB(0x10, 0x14, 0x1E)
#define COL_LINK        RGB(0x16, 0x52, 0xC8)
#define COL_LINK_HOV    RGB(0xC0, 0x30, 0x30)
#define COL_HEAD        RGB(0x1A, 0x1E, 0x40)
#define COL_CHROME      RGB(0x20, 0x26, 0x3E)
#define COL_CHROME_HI   RGB(0x6A, 0x82, 0xFB)
#define COL_CHROME_FG   RGB(0xF0, 0xF2, 0xFA)
#define COL_BTN         RGB(0x33, 0x3C, 0x5C)
#define COL_BTN_DOWN    RGB(0x6E, 0xA8, 0xFE)
#define COL_BTN_DIM     RGB(0x40, 0x46, 0x5A)
#define COL_URL_BG      RGB(0xFF, 0xFF, 0xFF)
#define COL_URL_FG      RGB(0x18, 0x1A, 0x28)
#define COL_URL_BORDER  RGB(0x6E, 0xA8, 0xFE)
#define COL_STATUS_BG   RGB(0x18, 0x1C, 0x30)
#define COL_STATUS_FG   RGB(0xC8, 0xCC, 0xE0)
#define COL_DIM         RGB(0x88, 0x8E, 0xA8)
#define COL_RULE        RGB(0xCC, 0xCE, 0xD8)

/* ---------- data ---------- */

typedef struct {
    int      x, y;        /* document-space pixels */
    int      w;
    int      text_off;    /* offset into doc_text_pool */
    int      text_len;
    uint32_t fg;
    int      link_idx;
    uint8_t  underline;
} run_t;

typedef struct {
    char url[URL_MAX];
} link_t;

static uint8_t  resp_buf[RESP_CAP];
static char     doc_text_pool[TEXT_POOL_CAP];
static int      doc_text_used = 0;
static run_t    runs[MAX_RUNS];
static int      n_runs = 0;
static link_t   links[MAX_LINKS];
static int      n_links = 0;

static int      doc_height = 0;        /* total document pixel height */
static int      scroll_y   = 0;

/* URL bar / status */
static char     cur_url[URL_MAX];      /* loaded page */
static char     edit_url[URL_MAX];     /* what's in the URL field */
static int      edit_len   = 0;
static int      edit_cur   = 0;
static bool     url_focused = true;
static uint32_t blink_ms   = 0;

static char     status_text[160];
static bool     loading    = false;

/* navigation history (back stack) */
static char     hist[HIST_MAX][URL_MAX];
static int      hist_top = 0;

/* click debounce / interactive hit testing */
static bool     click_consumed = false;
static int      hover_link = -1;       /* updated on hover redraws */

/* button hit rects (filled by paint) */
static struct {
    int x, y, w, h;
} btn_back, btn_reload, btn_home, btn_go;
static struct {
    int x, y, w, h;
} url_rect;
static struct {
    int x, y, w, h;
} content_rect;

static window_t* g_browser_win = NULL;

/* ---------- string helpers ---------- */

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static void str_set(char* dst, int cap, const char* src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int int_to_str(int v, char* buf) {
    char t[12]; int p = 0;
    if (v == 0) { t[p++] = '0'; }
    else {
        bool neg = v < 0;
        unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
        while (u) { t[p++] = (char)('0' + u % 10); u /= 10; }
        if (neg) t[p++] = '-';
    }
    int n = p;
    for (int i = 0; i < p; i++) buf[i] = t[p - 1 - i];
    buf[p] = 0;
    return n;
}

static void set_status(const char* a, const char* b) {
    int p = 0;
    if (a) for (int i = 0; a[i] && p < (int)sizeof(status_text) - 1; i++) status_text[p++] = a[i];
    if (b) for (int i = 0; b[i] && p < (int)sizeof(status_text) - 1; i++) status_text[p++] = b[i];
    status_text[p] = 0;
}

static void set_status_num(const char* a, int v, const char* b) {
    int p = 0;
    if (a) for (int i = 0; a[i] && p < (int)sizeof(status_text) - 1; i++) status_text[p++] = a[i];
    char num[16];
    int n = int_to_str(v, num);
    for (int i = 0; i < n && p < (int)sizeof(status_text) - 1; i++) status_text[p++] = num[i];
    if (b) for (int i = 0; b[i] && p < (int)sizeof(status_text) - 1; i++) status_text[p++] = b[i];
    status_text[p] = 0;
}

/* ---------- URL parsing ---------- */

static int parse_octet(const char** pp) {
    const char* p = *pp;
    int v = 0, n = 0;
    while (*p >= '0' && *p <= '9' && n < 3) { v = v*10 + (*p - '0'); p++; n++; }
    if (n == 0 || v > 255) return -1;
    *pp = p;
    return v;
}

/* Tiny static "/etc/hosts" — we have no DNS, but most things people type
   are these two names anyway. `host` and `localhost` mean the QEMU SLIRP
   gateway (= the machine the user is running QEMU on). */
typedef struct { const char* name; uint32_t ip; } host_alias_t;

static const host_alias_t host_aliases[] = {
    {"host",      IP4(10,0,2,2)},
    {"localhost", IP4(10,0,2,2)},
    {"gateway",   IP4(10,0,2,2)},
    {"samara",    IP4(10,0,2,15)},
    {"self",      IP4(10,0,2,15)},
};
#define HOST_ALIAS_N (int)(sizeof(host_aliases) / sizeof(host_aliases[0]))

static int parse_url(const char* url, uint32_t* ip, uint16_t* port,
                     char* path_out, int path_cap) {
    const char* p = url;
    while (*p == ' ' || *p == '\t') p++;
    /* case-insensitive http:// */
    if ((p[0]|0x20) == 'h' && (p[1]|0x20) == 't' && (p[2]|0x20) == 't' &&
        (p[3]|0x20) == 'p' && p[4] == ':' && p[5] == '/' && p[6] == '/')
        p += 7;
    /* explicitly reject https:// — no TLS */
    if ((p[0]|0x20) == 'h' && (p[1]|0x20) == 't' && (p[2]|0x20) == 't' &&
        (p[3]|0x20) == 'p' && (p[4]|0x20) == 's' && p[5] == ':')
        return 0;

    bool resolved = false;
    if (!(*p >= '0' && *p <= '9')) {
        for (int i = 0; i < HOST_ALIAS_N; i++) {
            const char* name = host_aliases[i].name;
            int nl = 0; while (name[nl]) nl++;
            bool eq = true;
            for (int k = 0; k < nl; k++)
                if (to_lower(p[k]) != name[k]) { eq = false; break; }
            if (eq && (p[nl] == 0 || p[nl] == '/' || p[nl] == ':')) {
                *ip = host_aliases[i].ip;
                p += nl;
                resolved = true;
                break;
            }
        }
        if (!resolved) return 0;
    } else {
        int a = parse_octet(&p); if (a < 0 || *p != '.') return 0; p++;
        int b = parse_octet(&p); if (b < 0 || *p != '.') return 0; p++;
        int c = parse_octet(&p); if (c < 0 || *p != '.') return 0; p++;
        int d = parse_octet(&p); if (d < 0) return 0;
        *ip = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    }

    *port = 80;
    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
        if (v < 1 || v > 65535) return 0;
        *port = (uint16_t)v;
    }
    int pi = 0;
    if (*p == 0) {
        if (path_cap > 1) { path_out[pi++] = '/'; path_out[pi] = 0; }
    } else if (*p == '/') {
        while (*p && pi < path_cap - 1) path_out[pi++] = *p++;
        path_out[pi] = 0;
    } else {
        return 0;
    }
    return 1;
}

/* Resolve a possibly-relative href against the current page URL. Writes a
   fully qualified http:// URL into out. Returns 1 on success, 0 if not
   reachable (e.g. mailto:, https://). */
static int resolve_href(const char* href, char* out, int cap) {
    if (!href || !*href) return 0;
    /* Already absolute http? */
    if (strncmp(href, "http://", 7) == 0) {
        str_set(out, cap, href);
        return 1;
    }
    /* Anything else with a scheme — give up. */
    for (int i = 0; href[i]; i++) {
        if (href[i] == ':') return 0;
        if (href[i] == '/' || href[i] == '#' || href[i] == '?') break;
    }
    if (href[0] == '#') return 0;       /* same-page fragment, skip */

    /* Need the base from cur_url. Parse it. */
    uint32_t ip; uint16_t port; char base_path[URL_MAX];
    if (!parse_url(cur_url, &ip, &port, base_path, sizeof(base_path))) return 0;

    /* Compose http://IP[:port] */
    int p = 0;
    const char* pfx = "http://";
    for (int i = 0; pfx[i] && p < cap - 1; i++) out[p++] = pfx[i];
    char tmp[8];
    for (int oct = 0; oct < 4; oct++) {
        int n = int_to_str((ip >> (24 - oct*8)) & 0xFF, tmp);
        for (int i = 0; i < n && p < cap - 1; i++) out[p++] = tmp[i];
        if (oct < 3 && p < cap - 1) out[p++] = '.';
    }
    if (port != 80 && p < cap - 1) {
        out[p++] = ':';
        int n = int_to_str(port, tmp);
        for (int i = 0; i < n && p < cap - 1; i++) out[p++] = tmp[i];
    }

    if (href[0] == '/') {
        for (int i = 0; href[i] && p < cap - 1; i++) out[p++] = href[i];
    } else {
        /* Strip filename from base_path, then append href */
        int last_slash = 0;
        for (int i = 0; base_path[i]; i++) if (base_path[i] == '/') last_slash = i;
        for (int i = 0; i <= last_slash && p < cap - 1; i++) out[p++] = base_path[i];
        for (int i = 0; href[i] && p < cap - 1; i++) out[p++] = href[i];
    }
    out[p] = 0;
    return 1;
}

/* ---------- UTF-8 -> CP866 ---------- */

/* Walks the buffer in place, converting UTF-8 byte sequences (mainly the
   Russian Cyrillic block) into CP866 single-byte codes that our 8x16 font
   can render. Returns the new (possibly shorter) length. */
static int utf8_to_cp866_inplace(uint8_t* buf, int len) {
    int rd = 0, wr = 0;
    while (rd < len) {
        uint8_t c = buf[rd];
        if (c < 0x80) { buf[wr++] = c; rd++; continue; }

        if (c == 0xD0 && rd + 1 < len) {
            uint8_t c2 = buf[rd + 1];
            uint8_t out = '?';
            if      (c2 == 0x81) out = 0xF0;                      /* Ё */
            else if (c2 >= 0x90 && c2 <= 0xAF) out = 0x80 + (c2 - 0x90);   /* А..Я (incl. И, Й, ...) */
            else if (c2 >= 0xB0 && c2 <= 0xBF) out = 0xA0 + (c2 - 0xB0);   /* а..п */
            buf[wr++] = out; rd += 2; continue;
        }
        if (c == 0xD1 && rd + 1 < len) {
            uint8_t c2 = buf[rd + 1];
            uint8_t out = '?';
            if      (c2 == 0x91) out = 0xF1;                      /* ё */
            else if (c2 >= 0x80 && c2 <= 0x8F) out = 0xE0 + (c2 - 0x80);   /* р..я */
            buf[wr++] = out; rd += 2; continue;
        }
        if ((c & 0xE0) == 0xC0 && rd + 1 < len) {
            /* Other 2-byte UTF-8 (Latin extended, etc) — squash to '?' */
            uint8_t c2 = buf[rd + 1];
            uint32_t cp = ((c & 0x1F) << 6) | (c2 & 0x3F);
            uint8_t out;
            if (cp == 0x00A0) out = ' ';        /* nbsp */
            else if (cp == 0x00AB) out = '<';
            else if (cp == 0x00BB) out = '>';
            else if (cp == 0x00A9 || cp == 0x00AE) out = 'C';
            else out = '?';
            buf[wr++] = out; rd += 2; continue;
        }
        if ((c & 0xF0) == 0xE0 && rd + 2 < len) {
            uint8_t c2 = buf[rd + 1], c3 = buf[rd + 2];
            uint32_t cp = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            if (cp == 0x2026) {     /* ellipsis "..." — expand if room */
                if (wr + 3 <= rd + 3) { buf[wr++]='.'; buf[wr++]='.'; buf[wr++]='.'; rd += 3; continue; }
            }
            uint8_t out = '?';
            if      (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) out = '-';
            else if (cp == 0x2018 || cp == 0x2019) out = '\'';
            else if (cp == 0x201C || cp == 0x201D || cp == 0x00AB) out = '"';
            else if (cp == 0x2022 || cp == 0x00B7 || cp == 0x25CF) out = '*';
            else if (cp == 0x00A0) out = ' ';
            buf[wr++] = out; rd += 3; continue;
        }
        if ((c & 0xF8) == 0xF0 && rd + 3 < len) {
            buf[wr++] = '?';
            rd += 4; continue;
        }
        /* stray byte — pass through */
        buf[wr++] = c; rd++;
    }
    return wr;
}

/* ---------- HTML parser ---------- */

static bool tag_eq(const char* tag, int tlen, const char* name) {
    int nlen = 0; while (name[nlen]) nlen++;
    if (tlen != nlen) return false;
    for (int i = 0; i < tlen; i++) if (to_lower(tag[i]) != name[i]) return false;
    return true;
}

/* Decode a single HTML entity starting at body[*i] (which is '&').
   Writes the decoded char(s) to out (up to *out_cap); on success, advances
   *i past the ';' and returns the number of bytes written. On failure
   (unrecognized), returns 0 and *i is unchanged. */
static int decode_entity(const char* body, int body_len, int* i, char* out, int out_cap) {
    int start = *i;
    if (start >= body_len || body[start] != '&') return 0;
    int j = start + 1;
    int max_j = j + 8;
    if (max_j > body_len) max_j = body_len;
    int end = -1;
    for (int k = j; k < max_j; k++) {
        if (body[k] == ';') { end = k; break; }
        /* numeric refs allow #, x, digits, hex digits; named refs only alnum */
        char c = body[k];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '#' || c == 'x'))
            return 0;
    }
    if (end < 0) return 0;
    int len = end - j;
    if (len <= 0) return 0;

    char buf[12];
    if (len > 11) return 0;
    for (int k = 0; k < len; k++) buf[k] = body[j + k];
    buf[len] = 0;

    /* numeric */
    if (buf[0] == '#') {
        int v = 0;
        if (buf[1] == 'x' || buf[1] == 'X') {
            for (int k = 2; k < len; k++) {
                char c = buf[k]; int d;
                if      (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else return 0;
                v = v*16 + d;
            }
        } else {
            for (int k = 1; k < len; k++) {
                char c = buf[k];
                if (c < '0' || c > '9') return 0;
                v = v*10 + (c - '0');
            }
        }
        uint8_t mapped = 0;
        if (v > 0 && v < 128) {
            mapped = (uint8_t)v;
        } else if (v >= 0x0410 && v <= 0x042F) {       /* А..Я */
            mapped = (uint8_t)(0x80 + (v - 0x0410));
        } else if (v >= 0x0430 && v <= 0x043F) {       /* а..п */
            mapped = (uint8_t)(0xA0 + (v - 0x0430));
        } else if (v >= 0x0440 && v <= 0x044F) {       /* р..я */
            mapped = (uint8_t)(0xE0 + (v - 0x0440));
        } else if (v == 0x0401) {                      /* Ё */
            mapped = 0xF0;
        } else if (v == 0x0451) {                      /* ё */
            mapped = 0xF1;
        } else if (v == 0x00A0) {                      /* nbsp */
            mapped = ' ';
        } else if (v == 0x2013 || v == 0x2014 || v == 0x2212) {
            mapped = '-';
        } else {
            mapped = '?';
        }
        if (out_cap >= 1) { out[0] = (char)mapped; *i = end + 1; return 1; }
        return 0;
    }
    /* named */
    struct { const char* name; const char* repl; } table[] = {
        {"amp", "&"},  {"lt", "<"},   {"gt", ">"},
        {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"copy", "(c)"}, {"reg", "(R)"}, {"mdash", "--"}, {"ndash", "-"},
        {"hellip", "..."}, {"laquo", "<<"}, {"raquo", ">>"},
        {"middot", "*"}, {"bull", "*"}, {"trade", "(TM)"},
        {NULL, NULL}
    };
    for (int t = 0; table[t].name; t++) {
        int nlen = 0; while (table[t].name[nlen]) nlen++;
        if (nlen == len) {
            bool eq = true;
            for (int k = 0; k < len; k++)
                if (to_lower(buf[k]) != table[t].name[k]) { eq = false; break; }
            if (eq) {
                int rl = 0; while (table[t].repl[rl]) rl++;
                if (rl > out_cap) rl = out_cap;
                for (int k = 0; k < rl; k++) out[k] = table[t].repl[k];
                *i = end + 1;
                return rl;
            }
        }
    }
    /* Unknown entity — pass through as '?' */
    if (out_cap >= 1) { out[0] = '?'; *i = end + 1; return 1; }
    return 0;
}

/* Add `len` chars of text into the doc_text_pool; returns offset or -1 if full.
   The terminator at [off+len] must survive subsequent calls, so we reserve
   len+1 bytes — otherwise the next pool_add would overwrite it and gfx_string
   would print run-on garbage. */
static int pool_add(const char* s, int len) {
    if (doc_text_used + len + 1 > TEXT_POOL_CAP) return -1;
    int off = doc_text_used;
    for (int i = 0; i < len; i++) doc_text_pool[off + i] = s[i];
    doc_text_pool[off + len] = 0;
    doc_text_used += len + 1;
    return off;
}

/* ---------- Parser state, exposed to handle_tag ---------- */

typedef struct {
    int      view_w;       /* pixel width available */
    int      cx, cy;       /* doc cursor */
    int      line_h;       /* current line spacing */
    uint32_t fg;
    int      link_idx;
    bool     underline;
    bool     pre;
    int      indent;       /* x indent (e.g., for <li>) */
    bool     pending_space;
    bool     line_dirty;   /* something on current line */
    int      skip_until_tag; /* 0=none, 1=until </script>, 2=until </style> */
    int      h_level;      /* >0 if inside <hN> */
    bool     bold;
} pstate_t;

static void emit_text_chunk(pstate_t* st, const char* s, int len) {
    if (len <= 0) return;
    if (n_runs >= MAX_RUNS) return;
    int w = len * 8;
    int off = pool_add(s, len);
    if (off < 0) return;
    run_t* r = &runs[n_runs++];
    r->x = st->cx + st->indent;
    r->y = st->cy;
    r->w = w;
    r->text_off = off;
    r->text_len = len;
    r->fg = st->fg;
    r->link_idx = st->link_idx;
    r->underline = (st->underline || st->link_idx >= 0) ? 1 : 0;
    st->cx += w;
    st->line_dirty = true;
}

static void parser_newline(pstate_t* st, int extra) {
    st->cy += st->line_h + extra;
    st->cx = 0;
    st->pending_space = false;
    st->line_dirty = false;
}

static void word_break(pstate_t* st) {
    /* about to add a word; nothing more to do here, emit_word handles wrap */
}

static void emit_word(pstate_t* st, const char* word, int len) {
    if (len <= 0) return;
    int word_w = len * 8;
    int space_w = (st->pending_space && st->line_dirty) ? 8 : 0;
    if (st->cx + space_w + word_w > st->view_w) {
        parser_newline(st, 0);
    } else if (space_w) {
        st->cx += space_w;
    }
    emit_text_chunk(st, word, len);
    st->pending_space = false;
}

/* Find an attribute value within a tag's body (between < and >).
   tag_body points just past the tag-name. Returns 1 on success and writes
   the value into out. Strips surrounding quotes. */
static int find_attr(const char* tag_body, int tlen, const char* attr,
                     char* out, int cap) {
    int alen = 0; while (attr[alen]) alen++;
    int i = 0;
    while (i < tlen) {
        while (i < tlen && (tag_body[i] == ' ' || tag_body[i] == '\t' ||
                             tag_body[i] == '\r' || tag_body[i] == '\n')) i++;
        if (i >= tlen) return 0;
        int ns = i;
        while (i < tlen && tag_body[i] != '=' && tag_body[i] != ' ' &&
               tag_body[i] != '\t' && tag_body[i] != '>') i++;
        int nlen = i - ns;
        bool match = (nlen == alen);
        if (match) {
            for (int k = 0; k < nlen; k++)
                if (to_lower(tag_body[ns + k]) != attr[k]) { match = false; break; }
        }
        /* skip ws */
        while (i < tlen && (tag_body[i] == ' ' || tag_body[i] == '\t')) i++;
        if (i < tlen && tag_body[i] == '=') {
            i++;
            while (i < tlen && (tag_body[i] == ' ' || tag_body[i] == '\t')) i++;
            char quote = 0;
            if (i < tlen && (tag_body[i] == '"' || tag_body[i] == '\'')) {
                quote = tag_body[i]; i++;
            }
            int vs = i;
            while (i < tlen) {
                char c = tag_body[i];
                if (quote && c == quote) break;
                if (!quote && (c == ' ' || c == '\t' || c == '>')) break;
                i++;
            }
            int vlen = i - vs;
            if (match) {
                if (vlen > cap - 1) vlen = cap - 1;
                for (int k = 0; k < vlen; k++) out[k] = tag_body[vs + k];
                out[vlen] = 0;
                return 1;
            }
            if (quote && i < tlen && tag_body[i] == quote) i++;
        } else if (match) {
            /* attribute without value */
            if (cap > 0) out[0] = 0;
            return 1;
        }
    }
    return 0;
}

static void handle_tag(pstate_t* st, const char* tag, int tlen) {
    if (tlen <= 0) return;
    bool closing = false;
    int t = 0;
    if (tag[0] == '/') { closing = true; t = 1; }
    int ns = t;
    while (t < tlen && tag[t] != ' ' && tag[t] != '\t' && tag[t] != '/' &&
           tag[t] != '\r' && tag[t] != '\n') t++;
    int nlen = t - ns;
    const char* name = tag + ns;
    const char* body = tag + t;
    int blen = tlen - t;

    /* If we're inside a <script>/<style>/<title>, only react to its closer.
       These have to be FIRST or else </title> falls through to the title
       handler below — which is a no-op for closers — and skip_until_tag=3
       stays set forever, swallowing the entire page body. */
    if (st->skip_until_tag == 1) {
        if (closing && tag_eq(name, nlen, "script")) st->skip_until_tag = 0;
        return;
    }
    if (st->skip_until_tag == 2) {
        if (closing && tag_eq(name, nlen, "style")) st->skip_until_tag = 0;
        return;
    }
    if (st->skip_until_tag == 3) {
        if (closing && tag_eq(name, nlen, "title")) st->skip_until_tag = 0;
        return;
    }

    if (!closing && tag_eq(name, nlen, "script")) { st->skip_until_tag = 1; return; }
    if (!closing && tag_eq(name, nlen, "style"))  { st->skip_until_tag = 2; return; }

    if (tag_eq(name, nlen, "br")) { parser_newline(st, 0); return; }
    if (tag_eq(name, nlen, "hr")) {
        if (st->line_dirty) parser_newline(st, 0);
        st->cy += 4;
        /* emit a fake "rule" using a thin string of spaces with underline trick — skip */
        return;
    }
    if (tag_eq(name, nlen, "p")) {
        if (st->line_dirty) parser_newline(st, 8);
        else st->cy += 8;
        return;
    }
    if (tag_eq(name, nlen, "div") || tag_eq(name, nlen, "section") ||
        tag_eq(name, nlen, "article") || tag_eq(name, nlen, "header") ||
        tag_eq(name, nlen, "footer") || tag_eq(name, nlen, "main") ||
        tag_eq(name, nlen, "nav")    || tag_eq(name, nlen, "aside")) {
        if (st->line_dirty) parser_newline(st, 0);
        return;
    }
    if (tag_eq(name, nlen, "li")) {
        if (!closing) {
            if (st->line_dirty) parser_newline(st, 0);
            emit_word(st, "* ", 2);
        }
        return;
    }
    if (tag_eq(name, nlen, "ul") || tag_eq(name, nlen, "ol")) {
        if (st->line_dirty) parser_newline(st, 0);
        return;
    }
    if (tag_eq(name, nlen, "h1") || tag_eq(name, nlen, "h2") ||
        tag_eq(name, nlen, "h3") || tag_eq(name, nlen, "h4") ||
        tag_eq(name, nlen, "h5") || tag_eq(name, nlen, "h6")) {
        if (!closing) {
            if (st->line_dirty) parser_newline(st, 6);
            else st->cy += 6;
            st->fg = COL_HEAD;
            st->h_level = (name[1] - '0');
            st->line_h = HEAD_H;
            st->bold = true;
        } else {
            parser_newline(st, 4);
            st->fg = COL_TEXT;
            st->h_level = 0;
            st->line_h = LINE_H;
            st->bold = false;
        }
        return;
    }
    if (tag_eq(name, nlen, "title")) {
        /* Skip <title> content — it goes in window title, not body. The
           matching close is intercepted by the skip_until_tag==3 guard at
           the top of this function. */
        if (!closing) st->skip_until_tag = 3;
        return;
    }
    if (tag_eq(name, nlen, "a")) {
        if (!closing) {
            char href[URL_MAX]; href[0] = 0;
            if (find_attr(body, blen, "href", href, sizeof(href)) && href[0]) {
                char abs_url[URL_MAX];
                if (resolve_href(href, abs_url, sizeof(abs_url)) && n_links < MAX_LINKS) {
                    int li = n_links++;
                    str_set(links[li].url, URL_MAX, abs_url);
                    st->link_idx = li;
                    st->fg = COL_LINK;
                    st->underline = true;
                }
            }
        } else {
            st->link_idx = -1;
            st->fg = (st->h_level > 0) ? COL_HEAD : COL_TEXT;
            st->underline = false;
        }
        return;
    }
    if (tag_eq(name, nlen, "img")) {
        char alt[64]; alt[0] = 0;
        if (find_attr(body, blen, "alt", alt, sizeof(alt)) && alt[0]) {
            emit_word(st, "[img:", 5);
            int al = 0; while (alt[al]) al++;
            if (al > 40) al = 40;
            emit_word(st, alt, al);
            emit_word(st, "]", 1);
        } else {
            emit_word(st, "[img]", 5);
        }
        return;
    }
    if (tag_eq(name, nlen, "tr") || tag_eq(name, nlen, "table")) {
        if (st->line_dirty) parser_newline(st, 0);
        return;
    }
    if (tag_eq(name, nlen, "td") || tag_eq(name, nlen, "th")) {
        st->pending_space = true;
        return;
    }
    if (tag_eq(name, nlen, "pre")) { st->pre = !closing; if (st->line_dirty) parser_newline(st, 0); return; }
    if (tag_eq(name, nlen, "b") || tag_eq(name, nlen, "strong")) { st->bold = !closing; return; }
    /* otherwise ignore tag */
    (void)word_break;
}

/* Parse HTML body and lay out runs. The page title (from <title>) is captured
   into title_out (cap chars). */
static void parse_html(const char* body, int body_len, int view_w,
                       char* title_out, int title_cap) {
    n_runs = 0; n_links = 0; doc_text_used = 0; doc_height = 0;
    if (title_out && title_cap > 0) title_out[0] = 0;

    pstate_t st;
    st.view_w = view_w;
    st.cx = 0; st.cy = 0;
    st.line_h = LINE_H;
    st.fg = COL_TEXT;
    st.link_idx = -1;
    st.underline = false;
    st.pre = false;
    st.indent = 0;
    st.pending_space = false;
    st.line_dirty = false;
    st.skip_until_tag = 0;
    st.h_level = 0;
    st.bold = false;

    char word[256];
    int  word_len = 0;
    int  title_len = 0;
    bool capturing_title = false;

    int i = 0;
    while (i < body_len) {
        char c = body[i];
        if (c == '<') {
            /* flush pending word */
            if (word_len > 0) {
                if (capturing_title && title_out && title_len < title_cap - 1) {
                    int can = word_len;
                    if (title_len + can > title_cap - 1) can = title_cap - 1 - title_len;
                    for (int k = 0; k < can; k++) title_out[title_len++] = word[k];
                    title_out[title_len] = 0;
                } else if (st.skip_until_tag == 0) {
                    emit_word(&st, word, word_len);
                }
                word_len = 0;
            }
            /* read tag */
            int j = i + 1;
            /* HTML comment */
            if (j + 2 < body_len && body[j] == '!' && body[j+1] == '-' && body[j+2] == '-') {
                int k = j + 3;
                while (k + 2 < body_len && !(body[k]=='-' && body[k+1]=='-' && body[k+2]=='>')) k++;
                i = (k + 3 <= body_len) ? (k + 3) : body_len;
                continue;
            }
            /* doctype or processing instruction */
            if (j < body_len && (body[j] == '!' || body[j] == '?')) {
                while (j < body_len && body[j] != '>') j++;
                i = (j < body_len) ? (j + 1) : body_len;
                continue;
            }
            int ts = j;
            while (j < body_len && body[j] != '>') j++;
            int tlen = j - ts;
            /* self-closing slash at end? leave it — handle_tag tolerates */
            /* detect <title>/</title> for capture mode */
            int t0 = ts;
            bool closing = false;
            if (t0 < ts + tlen && body[t0] == '/') { closing = true; t0++; }
            int nlen = 0;
            while (t0 + nlen < ts + tlen && body[t0+nlen] != ' ' && body[t0+nlen] != '\t' &&
                   body[t0+nlen] != '/' && body[t0+nlen] != '>') nlen++;
            if (nlen == 5) {
                bool is_title = true;
                const char* tn = "title";
                for (int k = 0; k < 5; k++)
                    if (to_lower(body[t0+k]) != tn[k]) { is_title = false; break; }
                if (is_title) {
                    capturing_title = !closing;
                }
            }
            handle_tag(&st, body + ts, tlen);
            i = (j < body_len) ? (j + 1) : body_len;
            continue;
        }
        if (c == '&') {
            char ent[8];
            int wrote = decode_entity(body, body_len, &i, ent, sizeof(ent));
            if (wrote > 0) {
                for (int k = 0; k < wrote; k++) {
                    char ch = ent[k];
                    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                        if (word_len > 0) {
                            if (capturing_title && title_out) {
                                if (title_len < title_cap - 1) {
                                    int can = word_len;
                                    if (title_len + can > title_cap - 1) can = title_cap - 1 - title_len;
                                    for (int q = 0; q < can; q++) title_out[title_len++] = word[q];
                                    title_out[title_len] = 0;
                                }
                            } else if (st.skip_until_tag == 0) {
                                emit_word(&st, word, word_len);
                            }
                            word_len = 0;
                        }
                        if (st.skip_until_tag == 0) st.pending_space = true;
                    } else if (word_len < (int)sizeof(word) - 1) {
                        word[word_len++] = ch;
                    }
                }
                continue;
            }
            /* malformed entity — treat & as regular char */
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (word_len > 0) {
                if (capturing_title && title_out) {
                    if (title_len < title_cap - 1) {
                        int can = word_len;
                        if (title_len + can > title_cap - 1) can = title_cap - 1 - title_len;
                        for (int k = 0; k < can; k++) title_out[title_len++] = word[k];
                        title_out[title_len] = 0;
                    }
                    if (c == ' ' && title_len < title_cap - 1) {
                        title_out[title_len++] = ' ';
                        title_out[title_len] = 0;
                    }
                } else if (st.skip_until_tag == 0) {
                    emit_word(&st, word, word_len);
                }
                word_len = 0;
            }
            if (st.skip_until_tag == 0) {
                if (st.pre && (c == '\n' || c == '\r')) {
                    parser_newline(&st, 0);
                } else {
                    st.pending_space = true;
                }
            }
            i++;
            continue;
        }
        /* normal char */
        if (word_len < (int)sizeof(word) - 1) word[word_len++] = c;
        i++;
    }
    if (word_len > 0 && st.skip_until_tag == 0) emit_word(&st, word, word_len);

    if (st.line_dirty) st.cy += st.line_h;
    doc_height = st.cy + 20;
}

/* ---------- Networking ---------- */

static int do_fetch(const char* url) {
    uint32_t ip; uint16_t port; char path[URL_MAX];
    if (!parse_url(url, &ip, &port, path, sizeof(path))) {
        set_status("error: bad URL: ", url);
        int p = 0; while (status_text[p]) p++;
        const char* hint = "  (use http://host/  or  http://10.0.2.2:8080/)";
        for (int i = 0; hint[i] && p + 1 < (int)sizeof(status_text); i++)
            status_text[p++] = hint[i];
        status_text[p] = 0;
        return -1;
    }
    if (!net_ready()) {
        if (net_init() != 0) {
            set_status("error: net init failed", NULL);
            return -1;
        }
    }

    /* compose a host string from the IP literal so the server gets a Host */
    char host[32]; int hp = 0; char num[8];
    for (int oct = 0; oct < 4; oct++) {
        int n = int_to_str((ip >> (24 - oct*8)) & 0xFF, num);
        for (int i = 0; i < n; i++) host[hp++] = num[i];
        if (oct < 3) host[hp++] = '.';
    }
    if (port != 80) {
        host[hp++] = ':';
        int n = int_to_str(port, num);
        for (int i = 0; i < n; i++) host[hp++] = num[i];
    }
    host[hp] = 0;

    set_status("loading ", url);
    loading = true;

    int n = net_http_get(ip, port, host, path, resp_buf, RESP_CAP);
    loading = false;
    if (n < 0) {
        set_status_num("error: net_http_get returned ", n, NULL);
        return -1;
    }
    if (n == 0) {
        set_status("error: empty response (0 bytes)", NULL);
        return -1;
    }
    return n;
}


/* The parsed document stays in resp_buf (CP866 already), so a window
   resize can re-flow it at the new width without refetching. */
static int laid_off, laid_len, laid_w;
static uint32_t relayout_since;

/* Derive view_w from the live window size — content_rect is only valid
   once br_paint has run at least once, and the first navigate happens
   during browser_open before any paint. */
static int live_view_w(void) {
    int view_w = 800;
    if (g_browser_win) {
        int cx, cy, cw, ch;
        wm_client_rect(g_browser_win, &cx, &cy, &cw, &ch);
        view_w = cw - 8 - 24 - 14;     /* scrollbar + margins */
    }
    if (view_w < 240) view_w = 240;
    return view_w;
}

static void browser_navigate(const char* url, bool push_history) {
    if (!url || !*url) return;
    if (push_history && cur_url[0]) {
        if (hist_top < HIST_MAX) {
            str_set(hist[hist_top++], URL_MAX, cur_url);
        } else {
            for (int i = 1; i < HIST_MAX; i++)
                memcpy(hist[i-1], hist[i], URL_MAX);
            str_set(hist[HIST_MAX - 1], URL_MAX, cur_url);
        }
    }
    str_set(cur_url, URL_MAX, url);
    str_set(edit_url, URL_MAX, url);
    edit_len = 0; while (edit_url[edit_len]) edit_len++;
    edit_cur = edit_len;

    int n = do_fetch(url);
    if (n <= 0) { n_runs = 0; n_links = 0; doc_height = 0; scroll_y = 0; laid_len = 0; return; }

    /* Find the body (skip headers up to \r\n\r\n) */
    int body_off = 0;
    for (int i = 0; i + 3 < n; i++) {
        if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
            resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
            body_off = i + 4; break;
        }
    }
    int body_len = n - body_off;

    /* Convert UTF-8 to CP866 in place — our 8x16 font is single-byte. */
    body_len = utf8_to_cp866_inplace(resp_buf + body_off, body_len);

    /* Inspect HTTP status line for non-200 responses (informational) */
    int status_code = 0;
    if (n >= 12 && resp_buf[0]=='H' && resp_buf[1]=='T' && resp_buf[2]=='T' && resp_buf[3]=='P') {
        int sp = 0; while (sp < n && resp_buf[sp] != ' ') sp++;
        sp++;
        while (sp < n && resp_buf[sp] >= '0' && resp_buf[sp] <= '9') {
            status_code = status_code*10 + (resp_buf[sp] - '0');
            sp++;
        }
    }

    int view_w = live_view_w();
    char page_title[96];
    parse_html((const char*)(resp_buf + body_off), body_len, view_w,
               page_title, sizeof(page_title));
    scroll_y = 0;
    laid_off = body_off;
    laid_len = body_len;
    laid_w = view_w;
    relayout_since = 0;

    if (g_browser_win) {
        const char* base = "Browser - ";
        int p = 0;
        for (int i = 0; base[i] && p < 63; i++) g_browser_win->title[p++] = base[i];
        for (int i = 0; page_title[i] && p < 63; i++) g_browser_win->title[p++] = page_title[i];
        if (p == 10) {
            const char* fb = "SamaraOS";
            for (int i = 0; fb[i] && p < 63; i++) g_browser_win->title[p++] = fb[i];
        }
        g_browser_win->title[p] = 0;
    }

    if (status_code && status_code != 200) {
        set_status_num("loaded (HTTP ", status_code, "), bytes:");
        int p = 0; while (status_text[p]) p++;
        char num[16]; int nn = int_to_str(body_len, num);
        if (p + 1 < (int)sizeof(status_text)) status_text[p++] = ' ';
        for (int i = 0; i < nn && p + 1 < (int)sizeof(status_text); i++) status_text[p++] = num[i];
        status_text[p] = 0;
    } else {
        set_status_num("loaded ", body_len, " bytes");
    }
}

/* ---------- Drawing ---------- */

static void draw_button(const char* label, int x, int y, int w, int h, bool active, bool enabled) {
    uint32_t bg = enabled ? (active ? COL_BTN_DOWN : COL_BTN) : COL_BTN_DIM;
    gfx_rect_fill(x, y, w, h, bg);
    gfx_rect(x, y, w, h, COL_CHROME_HI);
    int lw = 0; while (label[lw]) lw++;
    int tx = x + (w - lw * 8) / 2;
    int ty = y + (h - 16) / 2;
    gfx_string(tx, ty, label, enabled ? COL_CHROME_FG : COL_DIM, bg, false);
}

static void draw_url_bar(int x, int y, int w, int h) {
    url_rect.x = x; url_rect.y = y; url_rect.w = w; url_rect.h = h;
    gfx_rect_fill(x, y, w, h, COL_URL_BG);
    uint32_t border = url_focused ? COL_URL_BORDER : COL_BTN_DIM;
    gfx_rect(x, y, w, h, border);
    gfx_rect(x - 1, y - 1, w + 2, h + 2, border);

    /* Render the editable URL. If too long, scroll so the cursor is visible. */
    int max_chars = (w - 12) / 8;
    if (max_chars < 1) max_chars = 1;
    int off = 0;
    if (edit_cur > max_chars - 2) off = edit_cur - (max_chars - 2);
    int visible = edit_len - off;
    if (visible > max_chars) visible = max_chars;
    char tmp[URL_MAX];
    for (int i = 0; i < visible; i++) tmp[i] = edit_url[off + i];
    tmp[visible] = 0;
    gfx_string(x + 6, y + (h - 16) / 2, tmp, COL_URL_FG, COL_URL_BG, false);

    if (url_focused) {
        bool blink_on = ((blink_ms / 500) & 1) == 0;
        if (blink_on) {
            int cx = x + 6 + (edit_cur - off) * 8;
            gfx_rect_fill(cx, y + 4, 2, h - 8, COL_URL_FG);
        }
    }
}

static void draw_chrome(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);

    int top_h = 44;
    int status_h = 24;

    /* top chrome */
    gfx_rect_fill(cx, cy, cw, top_h, COL_CHROME);
    gfx_rect_fill(cx, cy + top_h - 2, cw, 2, COL_CHROME_HI);

    int bx = cx + 8, by = cy + 8;
    int bh = top_h - 16;
    int bw = 60;

    btn_back.x = bx;        btn_back.y = by; btn_back.w = bw; btn_back.h = bh;
    bx += bw + 4;
    btn_reload.x = bx;      btn_reload.y = by; btn_reload.w = bw + 14; btn_reload.h = bh;
    bx += bw + 14 + 4;
    btn_home.x = bx;        btn_home.y = by; btn_home.w = bw; btn_home.h = bh;
    bx += bw + 4;

    int go_w = 50;
    int url_x = bx + 6;
    int url_w = (cx + cw) - url_x - go_w - 16;
    if (url_w < 100) url_w = 100;

    btn_go.x = url_x + url_w + 6;
    btn_go.y = by; btn_go.w = go_w; btn_go.h = bh;

    draw_button("Back",   btn_back.x,   btn_back.y,   btn_back.w,   btn_back.h, false, hist_top > 0);
    draw_button("Reload", btn_reload.x, btn_reload.y, btn_reload.w, btn_reload.h, false, cur_url[0] != 0);
    draw_button("Home",   btn_home.x,   btn_home.y,   btn_home.w,   btn_home.h, false, true);
    draw_url_bar(url_x, by, url_w, bh);
    draw_button("Go",     btn_go.x,     btn_go.y,     btn_go.w,     btn_go.h, false, edit_len > 0);

    /* content area */
    content_rect.x = cx + 4;
    content_rect.y = cy + top_h + 4;
    content_rect.w = cw - 8;
    content_rect.h = ch - top_h - status_h - 8;

    gfx_rect_fill(content_rect.x, content_rect.y, content_rect.w, content_rect.h, COL_PAGE_BG);
    gfx_rect(content_rect.x, content_rect.y, content_rect.w, content_rect.h, COL_RULE);

    /* render runs */
    int doc_left = content_rect.x + 12;
    int doc_top  = content_rect.y + 8;
    int view_h   = content_rect.h - 16;
    for (int i = 0; i < n_runs; i++) {
        run_t* r = &runs[i];
        int sy = doc_top + r->y - scroll_y;
        if (sy + LINE_H < content_rect.y) continue;
        if (sy > content_rect.y + content_rect.h) continue;
        uint32_t fg = r->fg;
        if (r->link_idx >= 0 && hover_link == r->link_idx) fg = COL_LINK_HOV;
        gfx_string(doc_left + r->x, sy, doc_text_pool + r->text_off, fg, COL_PAGE_BG, false);
        if (r->underline) {
            gfx_rect_fill(doc_left + r->x, sy + 15, r->w, 1, fg);
        }
    }
    (void)view_h;

    /* scrollbar */
    int sb_w = 8;
    int sb_x = content_rect.x + content_rect.w - sb_w - 2;
    int sb_y = content_rect.y + 2;
    int sb_h = content_rect.h - 4;
    gfx_rect_fill(sb_x, sb_y, sb_w, sb_h, RGB(0xEE, 0xEE, 0xF4));
    if (doc_height > 0) {
        int thumb_h = sb_h * (sb_h) / (doc_height > sb_h ? doc_height : sb_h);
        if (thumb_h < 16) thumb_h = 16;
        int max_scroll = doc_height - sb_h;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = sb_y + (sb_h - thumb_h) * scroll_y / max_scroll;
        gfx_rect_fill(sb_x + 1, thumb_y, sb_w - 2, thumb_h, COL_CHROME_HI);
    }

    /* status bar */
    int statx = cx;
    int staty = cy + ch - status_h;
    gfx_rect_fill(statx, staty, cw, status_h, COL_STATUS_BG);
    gfx_rect_fill(statx, staty, cw, 1, COL_CHROME_HI);
    const char* st = status_text[0] ? status_text : "Ready";
    gfx_string(statx + 8, staty + 4, st, COL_STATUS_FG, COL_STATUS_BG, false);
    if (loading) {
        gfx_string(statx + cw - 80, staty + 4, "(loading...)", COL_CHROME_HI, COL_STATUS_BG, false);
    }
}

/* ---------- Hit testing helpers ---------- */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int hit_link(int sx, int sy) {
    int doc_left = content_rect.x + 12;
    int doc_top  = content_rect.y + 8;
    if (!in_rect(sx, sy, content_rect.x, content_rect.y, content_rect.w, content_rect.h))
        return -1;
    for (int i = 0; i < n_runs; i++) {
        run_t* r = &runs[i];
        if (r->link_idx < 0) continue;
        int x0 = doc_left + r->x;
        int y0 = doc_top + r->y - scroll_y;
        if (sx >= x0 && sx < x0 + r->w && sy >= y0 && sy < y0 + LINE_H) {
            return r->link_idx;
        }
    }
    return -1;
}

/* ---------- WM callbacks ---------- */

static void br_paint(window_t* w) {
    draw_chrome(w);
}

static void clamp_scroll(window_t* w);

/* Re-flow once the width has been stable for a moment (not on every
   pixel of a drag), keeping the reading position proportionally. */
static void relayout_if_resized(window_t* w, uint32_t now) {
    if (laid_len <= 0) return;
    int vw = live_view_w();
    if (vw == laid_w) { relayout_since = 0; return; }
    if (!relayout_since) { relayout_since = now | 1; return; }
    if (now - relayout_since < 150) return;
    int old_h = doc_height;
    char title[96];
    parse_html((const char*)(resp_buf + laid_off), laid_len, vw, title, sizeof(title));
    if (old_h > 0) scroll_y = (int)((int64_t)scroll_y * doc_height / old_h);
    laid_w = vw;
    relayout_since = 0;
    hover_link = -1;
    clamp_scroll(w);
    w->needs_repaint = true;
}

static void br_scroll(window_t* w, int dz) {
    scroll_y += dz * SCROLL_STEP * 3;
    clamp_scroll(w);
    w->needs_repaint = true;
}

static void br_tick(window_t* w, uint32_t now) {
    relayout_if_resized(w, now);
    if (url_focused && (now / 500) != (blink_ms / 500)) {
        w->needs_repaint = true;
    }
    blink_ms = now;

    /* Cheap hover detection: poll mouse every tick. The WM doesn't deliver
       move-without-press events to apps so this is how we get hover. */
    int mx, my; uint8_t btn;
    mouse_get(&mx, &my, &btn);
    int hl = hit_link(mx, my);
    if (hl != hover_link) { hover_link = hl; w->needs_repaint = true; }
}

static void br_release(window_t* w) {
    (void)w;
    click_consumed = false;
}

static void clamp_scroll(window_t* w) {
    int max_s = doc_height - content_rect.h + 16;
    if (max_s < 0) max_s = 0;
    if (scroll_y > max_s) scroll_y = max_s;
    if (scroll_y < 0) scroll_y = 0;
    (void)w;
}

static void browser_go_edit(void) {
    edit_url[edit_len] = 0;
    browser_navigate(edit_url, true);
}

static void browser_back(void) {
    if (hist_top <= 0) return;
    char prev[URL_MAX];
    str_set(prev, URL_MAX, hist[--hist_top]);
    /* navigate without pushing the current page back onto the stack */
    browser_navigate(prev, false);
}

static void browser_home(void) {
    browser_navigate("http://host:8080/", true);
}

static void br_click(window_t* w, int rx, int ry) {
    if (click_consumed) {
        /* Still update hover on drag so links highlight, but don't act */
        int sx = rx + w->x + 4;
        int sy = ry + w->y + WM_TITLE_H + 2;
        int hl = hit_link(sx, sy);
        if (hl != hover_link) { hover_link = hl; w->needs_repaint = true; }
        return;
    }
    click_consumed = true;

    /* relative coords are inside client area; convert to absolute screen coords */
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    int sx = rx + cx;
    int sy = ry + cy;

    if (in_rect(sx, sy, btn_back.x, btn_back.y, btn_back.w, btn_back.h)) {
        browser_back(); return;
    }
    if (in_rect(sx, sy, btn_reload.x, btn_reload.y, btn_reload.w, btn_reload.h)) {
        if (cur_url[0]) browser_navigate(cur_url, false);
        return;
    }
    if (in_rect(sx, sy, btn_home.x, btn_home.y, btn_home.w, btn_home.h)) {
        browser_home(); return;
    }
    if (in_rect(sx, sy, btn_go.x, btn_go.y, btn_go.w, btn_go.h)) {
        browser_go_edit(); return;
    }
    if (in_rect(sx, sy, url_rect.x, url_rect.y, url_rect.w, url_rect.h)) {
        url_focused = true;
        /* set cursor based on click x */
        int rx_in = sx - url_rect.x - 6;
        if (rx_in < 0) rx_in = 0;
        int new_cur = rx_in / 8;
        if (new_cur > edit_len) new_cur = edit_len;
        edit_cur = new_cur;
        return;
    }

    /* content area? */
    int hl = hit_link(sx, sy);
    if (hl >= 0) {
        url_focused = false;
        browser_navigate(links[hl].url, true);
        return;
    }
    url_focused = false;
}

static void br_key(window_t* w, char ch) {
    unsigned char c = (unsigned char)ch;     /* K_* codes live in 0x81..0x9F */
    if (c == 0x1B) {
        if (url_focused) { url_focused = false; w->needs_repaint = true; return; }
        wm_close(w); return;
    }
    /* navigation keys are not stolen by URL bar */
    if (c == K_PGUP) { scroll_y -= PAGE_STEP; clamp_scroll(w); w->needs_repaint = true; return; }
    if (c == K_PGDN) { scroll_y += PAGE_STEP; clamp_scroll(w); w->needs_repaint = true; return; }
    if (c == K_HOME && !url_focused) { scroll_y = 0; w->needs_repaint = true; return; }
    if (c == K_END  && !url_focused) { scroll_y = 1 << 30; clamp_scroll(w); w->needs_repaint = true; return; }
    if (!url_focused) {
        if (c == K_UP)    { scroll_y -= SCROLL_STEP;   clamp_scroll(w); w->needs_repaint = true; return; }
        if (c == K_DOWN)  { scroll_y += SCROLL_STEP;   clamp_scroll(w); w->needs_repaint = true; return; }
        if (c == ' ')     { scroll_y += PAGE_STEP;     clamp_scroll(w); w->needs_repaint = true; return; }
        /* press '/' or 'l' to jump to URL bar */
        if (c == '/' || c == 'l' || c == 'L') { url_focused = true; w->needs_repaint = true; return; }
        if (c == 'r' || c == 'R') { if (cur_url[0]) browser_navigate(cur_url, false); return; }
        if (c == '\b') { browser_back(); return; }
        return;
    }
    /* URL bar editing */
    if (c == '\n') {
        url_focused = false;
        browser_go_edit();
        return;
    }
    if (c == '\b') {
        if (edit_cur > 0) {
            for (int i = edit_cur - 1; i < edit_len; i++) edit_url[i] = edit_url[i + 1];
            edit_len--; edit_cur--;
            edit_url[edit_len] = 0;
            w->needs_repaint = true;
        }
        return;
    }
    if (c == K_DEL) {
        if (edit_cur < edit_len) {
            for (int i = edit_cur; i < edit_len; i++) edit_url[i] = edit_url[i + 1];
            edit_len--;
            edit_url[edit_len] = 0;
            w->needs_repaint = true;
        }
        return;
    }
    if (c == K_LEFT)  { if (edit_cur > 0)        edit_cur--; w->needs_repaint = true; return; }
    if (c == K_RIGHT) { if (edit_cur < edit_len) edit_cur++; w->needs_repaint = true; return; }
    if (c == K_HOME)  { edit_cur = 0;            w->needs_repaint = true; return; }
    if (c == K_END)   { edit_cur = edit_len;     w->needs_repaint = true; return; }
    if (c == K_UP || c == K_DOWN || c == K_PGUP || c == K_PGDN) return;

    /* Accept any printable byte including CP866 high bytes so the user can
       SEE that the RU layout produced Cyrillic. The URL itself must end up
       ASCII to be parseable, but the visible feedback matters. */
    if (c >= 0x20) {
        if (edit_len < URL_MAX - 1) {
            for (int i = edit_len; i > edit_cur; i--) edit_url[i] = edit_url[i - 1];
            edit_url[edit_cur++] = (char)c;
            edit_len++;
            edit_url[edit_len] = 0;
            w->needs_repaint = true;
        }
    }
}

static void br_close(window_t* w) {
    (void)w;
    g_browser_win = NULL;
}

/* ---------- public entry ---------- */

int browser_open(const char* url) {
    if (!gfx_ready()) return -1;
    if (g_browser_win && g_browser_win->open) {
        if (url && *url) {
            str_set(edit_url, URL_MAX, url);
            edit_len = 0; while (edit_url[edit_len]) edit_len++;
            edit_cur = edit_len;
            browser_navigate(url, true);
        }
        return 0;
    }

    /* one-time init */
    cur_url[0] = 0;
    edit_url[0] = 0;
    edit_len = edit_cur = 0;
    url_focused = true;
    status_text[0] = 0;
    n_runs = n_links = doc_text_used = 0;
    doc_height = 0; scroll_y = 0;
    hist_top = 0;
    hover_link = -1;
    click_consumed = false;

    int W = gfx_w(), H = gfx_h();
    int ww = 1200, wh = 800;
    if (ww > W - 80) ww = W - 80;
    if (wh > H - 120) wh = H - 120;
    int wx = (W - ww) / 2;
    int wy = (H - wh) / 2 - 24;
    if (wy < 30) wy = 30;

    g_browser_win = wm_open_app_ex(wx, wy, ww, wh, "Browser - SamaraOS",
                                    br_paint, br_key, br_click, br_tick,
                                    true, NULL);
    if (!g_browser_win) return -1;
    g_browser_win->on_release = br_release;
    g_browser_win->on_close   = br_close;
    g_browser_win->on_scroll  = br_scroll;
    g_browser_win->min_w = 480;
    g_browser_win->min_h = 240;

    set_status("ready - type http://host:8080/ then Enter (or click Home)", NULL);
    /* prefill URL bar with the default home so user just hits Enter */
    str_set(edit_url, URL_MAX, "http://host:8080/");
    edit_len = 0; while (edit_url[edit_len]) edit_len++;
    edit_cur = edit_len;

    if (url && *url) {
        str_set(edit_url, URL_MAX, url);
        edit_len = 0; while (edit_url[edit_len]) edit_len++;
        edit_cur = edit_len;
        browser_navigate(url, true);
    }
    return 0;
}
