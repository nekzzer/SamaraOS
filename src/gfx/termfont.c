#include "gfx/termfont.h"
#include "gfx/gfx.h"
#include "gfx/font.h"
#include "core/string.h"

extern const uint8_t _binary_src_gfx_termfont_bin_start[], _binary_src_gfx_termfont_bin_end[];

typedef struct { uint8_t cw, ch, base, pad; uint32_t off_reg, off_bold; } __attribute__((packed)) tf_size_t;

static const uint8_t*   blob;
static const uint32_t*  cps_reg;
static const uint32_t*  cps_bold;
static uint32_t         nreg, nbold, nsizes;
static const tf_size_t* sizes;
static int              cur;
static bool             ok;

void termfont_init(void) {
    blob = _binary_src_gfx_termfont_bin_start;
    uint32_t len = (uint32_t)(_binary_src_gfx_termfont_bin_end - _binary_src_gfx_termfont_bin_start);
    if (len < 16 || memcmp(blob, "SMTF", 4)) return;
    const uint32_t* h = (const uint32_t*)blob;
    nsizes = h[1]; nreg = h[2]; nbold = h[3];
    cps_reg = h + 4;
    cps_bold = cps_reg + nreg;
    sizes = (const tf_size_t*)(cps_bold + nbold);
    ok = nsizes > 0;
    cur = 0;
}

int  termfont_sizes(void)      { return ok ? (int)nsizes : 1; }
int  termfont_get(void)        { return cur; }
void termfont_set(int i)       { if (ok && i >= 0 && i < (int)nsizes) cur = i; }
int  termfont_cw_of(int i)     { return ok ? sizes[i].cw : 8; }
int  termfont_ch_of(int i)     { return ok ? sizes[i].ch : 16; }
int  termfont_cw(void)         { return termfont_cw_of(cur); }
int  termfont_ch(void)         { return termfont_ch_of(cur); }

static int find(const uint32_t* a, uint32_t n, uint32_t cp) {
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (a[m] == cp) return m;
        if (a[m] < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

/* ---------------- CP866 ---------------- */

static const uint16_t cp866[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
    0x0401, 0x0451, 0x0404, 0x0454, 0x0407, 0x0457, 0x040E, 0x045E,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x2116, 0x00A4, 0x25A0, 0x00A0,
};

uint32_t cp866_to_uni(uint8_t c) { return cp866[c]; }

int uni_to_cp866(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    for (int i = 0x80; i < 256; i++) if (cp866[i] == cp) return i;
    return -1;
}

/* ---------------- drawing helpers ---------------- */

static uint8_t mask[32 * 64];                        /* cell-sized alpha scratch */

static void mask_clear(int cw, int ch) { memset(mask, 0, (size_t)cw * ch); }
static void mask_rect(int cw, int ch, int x0, int y0, int x1, int y1, int a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (mask[y * cw + x] < a) mask[y * cw + x] = (uint8_t)a;
}

static int lw(int cw) { int t = cw / 8; return t < 1 ? 1 : t; }        /* light line width */

/* One arm of a box-drawing line from the cell centre towards an edge.
   w: 1 light, 2 heavy, 3 double. dir: 0 up, 1 right, 2 down, 3 left. */
static void arm(int cw, int ch, int dir, int w) {
    if (!w) return;
    int t = lw(cw), T = w == 2 ? t * 2 + (cw >= 12) : t;
    int cx = cw / 2, cy = ch / 2;
    if (w == 3) {                                     /* two light lines */
        int g = t;                                    /* gap */
        if (dir == 0 || dir == 2) {
            int y0 = dir == 0 ? 0 : cy - t - g / 2, y1 = dir == 0 ? cy + t + g / 2 + 1 : ch;
            mask_rect(cw, ch, cx - g - t, y0, cx - g, y1, 255);
            mask_rect(cw, ch, cx + g, y0, cx + g + t, y1, 255);
        } else {
            int x0 = dir == 3 ? 0 : cx - t - g / 2, x1 = dir == 3 ? cx + t + g / 2 + 1 : cw;
            mask_rect(cw, ch, x0, cy - g - t, x1, cy - g, 255);
            mask_rect(cw, ch, x0, cy + g, x1, cy + g + t, 255);
        }
        return;
    }
    int a = T / 2, b = T - a;                          /* thickness split around the centre */
    switch (dir) {
        case 0: mask_rect(cw, ch, cx - a, 0, cx + b, cy + b, 255); break;
        case 2: mask_rect(cw, ch, cx - a, cy - a, cx + b, ch, 255); break;
        case 3: mask_rect(cw, ch, 0, cy - a, cx + b, cy + b, 255); break;
        case 1: mask_rect(cw, ch, cx - a, cy - a, cw, cy + b, 255); break;
    }
}

/* U+2500..U+257F: arm weights up,right,down,left (0 none 1 light 2 heavy 3 double) */
static const char box[128][5] = {
    "0101","0202","1010","2020","0101","0202","1010","2020","0101","0202","1010","2020",
    "0110","0210","0120","0220","0011","0012","0021","0022","1100","1200","2100","2200",
    "1001","1002","2001","2002","1110","1210","2110","1120","2120","2210","1220","2220",
    "1011","1012","2011","1021","2021","2012","1022","2022","0111","0112","0211","0212",
    "0121","0122","0221","0222","1101","1102","1201","1202","2101","2102","2201","2202",
    "1111","1112","1211","1212","2111","1121","2121","2112","2211","1122","1221","2212",
    "2221","1222","2122","2222","0101","0202","1010","2020","0303","3030","0310","0130",
    "0330","0013","0031","0033","1300","3100","3300","1003","3001","3003","1310","3130",
    "3330","1013","3031","3033","0313","0131","0333","1303","3101","3303","1313","3131",
    "3333","----","----","----","----","----","----","----","0001","1000","0100","0010",
    "0002","2000","0200","0020","0201","2010","0102","1020",
};

static int isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

static void mask_max(int cw, int x, int y, int v) {
    if (v > 255) v = 255;
    if (v > 0 && mask[y * cw + x] < v) mask[y * cw + x] = (uint8_t)v;
}

/* Anti-aliased segment, coordinates in 1/16 px, thickness th (px). */
static void aa_line(int cw, int ch, int x0, int y0, int x1, int y1, int th) {
    int dx = x1 - x0, dy = y1 - y0;
    int len2 = dx * dx + dy * dy;
    int r = th * 8 + 8;                                  /* half width + 1/2 px, 1/16 units */
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            int px = x * 16 + 8 - x0, py = y * 16 + 8 - y0;
            int dot = px * dx + py * dy;
            int ex, ey;
            if (dot <= 0) { ex = px; ey = py; }
            else if (dot >= len2) { ex = px - dx; ey = py - dy; }
            else { ex = px - (int)((int64_t)dx * dot / len2); ey = py - (int)((int64_t)dy * dot / len2); }
            int d = isqrt((uint32_t)(ex * ex + ey * ey));
            if (d < r) mask_max(cw, x, y, (r - d) * 255 / 16);
        }
}

/* Rounded corner (U+256D..2570): a quarter circle whose centre sits dx,dy
   (+1/-1) half-cells away from the cell centre, plus straight runs. */
static void arc(int cw, int ch, int dx, int dy) {
    int t = lw(cw);
    int R = (cw < ch ? cw : ch) * 8;                     /* radius, 1/16 px */
    int ax = cw * 8 + dx * R, ay = ch * 8 + dy * R;      /* circle centre, 1/16 px */
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            int px = x * 16 + 8, py = y * 16 + 8;
            bool quad = (dx > 0 ? px <= ax : px >= ax) && (dy > 0 ? py <= ay : py >= ay);
            if (!quad) continue;
            int d = isqrt((uint32_t)((px - ax) * (px - ax) + (py - ay) * (py - ay)));
            int e = t * 8 + 8 - (d > R ? d - R : R - d);
            if (e > 0) mask_max(cw, x, y, e * 255 / 16);
        }
    int a = t / 2, b = t - a, icx = cw / 2, icy = ch / 2;
    if (dy > 0) mask_rect(cw, ch, icx - a, ay / 16, icx + b, ch, 255);
    else        mask_rect(cw, ch, icx - a, 0, icx + b, ay / 16 + 1, 255);
    if (dx > 0) mask_rect(cw, ch, ax / 16, icy - a, cw, icy + b, 255);
    else        mask_rect(cw, ch, 0, icy - a, ax / 16 + 1, icy + b, 255);
}

/* Draws code points we render ourselves into `mask`; false if not ours. */
static bool procedural(uint32_t cp, int cw, int ch, uint32_t fg, uint32_t bg, int x, int y) {
    if (cp >= 0x2500 && cp <= 0x257F) {
        mask_clear(cw, ch);
        int i = (int)(cp - 0x2500);
        if (cp >= 0x256D && cp <= 0x2570) {
            static const int8_t d[4][2] = { {1, 1}, {-1, 1}, {-1, -1}, {1, -1} };   /* ╭ ╮ ╯ ╰ */
            arc(cw, ch, d[cp - 0x256D][0], d[cp - 0x256D][1]);
        } else if (cp >= 0x2571 && cp <= 0x2573) {
            int t = lw(cw);
            if (cp != 0x2572) aa_line(cw, ch, 0, ch * 16, cw * 16, 0, t);
            if (cp != 0x2571) aa_line(cw, ch, 0, 0, cw * 16, ch * 16, t);
        } else {
            const char* s = box[i];
            for (int k = 0; k < 4; k++) arm(cw, ch, k, s[k] - '0');
        }
        gfx_alpha_mask(x, y, cw, ch, mask, cw, fg);
        return true;
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        int hx = cw / 2, hy = ch / 2;
        mask_clear(cw, ch);
        if (cp == 0x2580) mask_rect(cw, ch, 0, 0, cw, hy, 255);
        else if (cp <= 0x2588) mask_rect(cw, ch, 0, ch - ch * (int)(cp - 0x2580) / 8, cw, ch, 255);
        else if (cp <= 0x258F) mask_rect(cw, ch, 0, 0, cw * (int)(0x2590 - cp) / 8, ch, 255);
        else if (cp == 0x2590) mask_rect(cw, ch, hx, 0, cw, ch, 255);
        else if (cp <= 0x2593) mask_rect(cw, ch, 0, 0, cw, ch, 64 * (int)(cp - 0x2590));   /* shades */
        else if (cp == 0x2594) mask_rect(cw, ch, 0, 0, cw, ch / 8 ? ch / 8 : 1, 255);
        else if (cp == 0x2595) mask_rect(cw, ch, cw - (cw / 8 ? cw / 8 : 1), 0, cw, ch, 255);
        else {
            static const uint8_t q[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };   /* ▖..▟ as UL1 UR2 LL4 LR8 */
            int m = q[cp - 0x2596];
            if (m & 1) mask_rect(cw, ch, 0, 0, hx, hy, 255);
            if (m & 2) mask_rect(cw, ch, hx, 0, cw, hy, 255);
            if (m & 4) mask_rect(cw, ch, 0, hy, hx, ch, 255);
            if (m & 8) mask_rect(cw, ch, hx, hy, cw, ch, 255);
        }
        gfx_alpha_mask(x, y, cw, ch, mask, cw, fg);
        return true;
    }
    if (cp >= 0x2800 && cp <= 0x28FF) {                /* braille: 2x4 dots */
        static const uint8_t bit[4][2] = { {0, 3}, {1, 4}, {2, 5}, {6, 7} };
        int m = (int)(cp - 0x2800);
        mask_clear(cw, ch);
        int ds = cw / 4 ? cw / 4 : 1;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 2; c++)
                if (m & (1 << bit[r][c])) {
                    int px = cw * (1 + 2 * c) / 4 - ds / 2, py = ch * (1 + 2 * r) / 8 - ds / 2;
                    mask_rect(cw, ch, px, py, px + ds, py + ds, 255);
                }
        gfx_alpha_mask(x, y, cw, ch, mask, cw, fg);
        return true;
    }
    if (cp >= 0xE0B0 && cp <= 0xE0B3) {                /* powerline arrows */
        mask_clear(cw, ch);
        bool right = cp == 0xE0B0 || cp == 0xE0B1, solid = cp == 0xE0B0 || cp == 0xE0B2;
        /* 4x4 supersampling in integers: half-height of the triangle at
           column u is half*(cw-u)/cw (from its base) */
        int tw = lw(cw) * 4 * 4 / 5 + 1;                 /* outline half-width, 1/4 px */
        for (int yy = 0; yy < ch; yy++)
            for (int xx = 0; xx < cw; xx++) {
                int acc = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        int px = xx * 4 + sx, py = yy * 4 + sy;          /* 1/4 px, sample at +1/8 ~ */
                        int u = right ? px : cw * 4 - 1 - px;
                        int half = ch * 2, dist = py * 2 + 1 > half * 2 ? py - half : half - py - 1;
                        if (dist < 0) dist = -dist;
                        int edge = half * (cw * 4 - u) / (cw * 4);
                        bool in = solid ? dist <= edge : (dist <= edge + tw && dist >= edge - tw);
                        acc += in;
                    }
                mask[yy * cw + xx] = (uint8_t)(acc * 255 / 16);
            }
        gfx_alpha_mask(x, y, cw, ch, mask, cw, fg);
        return true;
    }
    (void)bg;
    return false;
}

static uint32_t mixc(uint32_t a, uint32_t b, int t) {        /* t/256 of the way a->b */
    uint32_t r = 0;
    for (int s = 0; s <= 16; s += 8) {
        int ca = (int)((a >> s) & 255), cb = (int)((b >> s) & 255);
        r |= (uint32_t)(ca + (cb - ca) * t / 256) << s;
    }
    return r;
}

void termfont_cell(int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, uint8_t attr) {
    int cw = termfont_cw(), ch = termfont_ch();
    if (attr & TF_DIM) fg = mixc(fg, bg, 110);
    gfx_rect_fill(x, y, cw, ch, bg);
    if (cp > ' ' && cp != 0xA0 && !procedural(cp, cw, ch, fg, bg, x, y)) {
        if (!ok) {
            int c = uni_to_cp866(cp);
            gfx_glyph(x, y, (char)(c < 0 ? '?' : c), fg, bg, false);
        } else {
            const tf_size_t* sz = &sizes[cur];
            const uint8_t* g = NULL;
            int k;
            if ((attr & TF_BOLD) && (k = find(cps_bold, nbold, cp)) >= 0)
                g = blob + sz->off_bold + (uint32_t)k * cw * ch;
            else if ((k = find(cps_reg, nreg, cp)) >= 0 || (k = find(cps_reg, nreg, 0xFFFD)) >= 0)
                g = blob + sz->off_reg + (uint32_t)k * cw * ch;
            if (g) gfx_alpha_mask(x, y, cw, ch, g, cw, fg);
        }
    }
    int t = ch / 16 ? ch / 16 : 1;
    if (attr & TF_UNDERLINE) {
        int uy = ok ? sizes[cur].base + 1 + t / 2 : ch - 2;
        if (uy + t > ch) uy = ch - t;
        gfx_rect_fill(x, y + uy, cw, t, fg);
    }
    if (attr & TF_STRIKE) gfx_rect_fill(x, y + ch * 9 / 16, cw, t, fg);
    if (attr & TF_CURSOR) gfx_rect_fill(x, y + ch - (ch / 8 ? ch / 8 : 2), cw, ch / 8 ? ch / 8 : 2, 0xE39B32);
}
