/* Anti-aliased desktop text. Glyph atlases come from tools/mkfont.py; pen
   positions run in 1/64 px so proportional spacing doesn't drift. */
#include "gfx/uifont.h"
#include "gfx/gfx.h"
#include "gfx/font.h"
#include "core/string.h"
#include "gfx/uifont_data.h"

static const uif_face_t* face(uif_t f) {
    return &uif_faces[(unsigned)f < UIF_COUNT ? f : UIF_REG];
}

/* Glyph for a byte, falling back to '?' for codes the face lacks. */
static const uif_glyph_t* glyph(const uif_face_t* fc, uint8_t c) {
    const uif_glyph_t* g = &fc->gl[c];
    if (g->adv) return g;
    g = &fc->gl['?'];
    return g->adv ? g : &fc->gl[' '];
}

int uif_height(uif_t f) { const uif_face_t* fc = face(f); return fc->ascent + fc->descent; }

int uif_width_n(uif_t f, const char* s, int n) {
    const uif_face_t* fc = face(f);
    int pen = 0;
    for (int i = 0; i < n && s[i]; i++) pen += glyph(fc, (uint8_t)s[i])->adv;
    return (pen + 32) >> 6;
}

int uif_width(uif_t f, const char* s) { return uif_width_n(f, s, 0x7FFFFFFF); }

static int draw_n(int x, int y, uif_t f, const char* s, int n, uint32_t color) {
    const uif_face_t* fc = face(f);
    int pen = x << 6;
    for (int i = 0; i < n && s[i]; i++) {
        const uif_glyph_t* g = glyph(fc, (uint8_t)s[i]);
        if (g->w)
            gfx_alpha_mask(((pen + 32) >> 6) + g->x, y + g->y, g->w, g->h,
                           fc->px + g->off, g->w, color);
        pen += g->adv;
    }
    return (pen + 32) >> 6;
}

int uif_draw(int x, int y, uif_t f, const char* s, uint32_t color) {
    return draw_n(x, y, f, s, 0x7FFFFFFF, color);
}

void uif_draw_fit(int x, int y, uif_t f, const char* s, int max_w, uint32_t color) {
    if (uif_width(f, s) <= max_w) { uif_draw(x, y, f, s, color); return; }
    int dots = uif_width(f, "...");
    int n = (int)strlen(s);
    while (n > 0 && uif_width_n(f, s, n) + dots > max_w) n--;
    while (n > 0 && s[n - 1] == ' ') n--;
    int end = draw_n(x, y, f, s, n, color);
    uif_draw(end, y, f, "...", color);
}

const uif_face_t* uif_face(uif_t f) { return face(f); }

int uif_top_for_mid(uif_t f, int mid_y) {
    const uif_face_t* fc = face(f);
    const uif_glyph_t* H = glyph(fc, f == UIF_HUGE ? 'a' : 'H');
    int cap_top = H->w ? H->y : fc->ascent * 3 / 10;
    return mid_y - (cap_top + fc->ascent + 1) / 2;
}

int uif_draw_mid(int x, int mid_y, uif_t f, const char* s, uint32_t color) {
    return uif_draw(x, uif_top_for_mid(f, mid_y), f, s, color);
}

void uif_draw_center(int x, int y, int w, int h, uif_t f, const char* s, uint32_t color) {
    uif_draw_mid(x + (w - uif_width(f, s)) / 2, y + h / 2, f, s, color);
}

int uif_draw_wrap(int x, int y, int max_w, int line_h, uif_t f, const char* s, uint32_t color) {
    while (*s) {
        /* longest run of whole words that fits */
        int n = 0, fit = 0;
        while (s[n] && s[n] != '\n') {
            int e = n;
            while (s[e] == ' ') e++;
            while (s[e] && s[e] != ' ' && s[e] != '\n') e++;
            if (uif_width_n(f, s, e) > max_w && fit) break;
            fit = n = e;
            if (uif_width_n(f, s, e) > max_w) break;   /* one over-long word */
        }
        draw_n(x, y, f, s, fit, color);
        y += line_h;
        s += fit;
        if (*s == '\n') s++;
        else while (*s == ' ') s++;
    }
    return y;
}

static inline uint32_t mixc(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t na = 256 - a;
    uint32_t rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * na) >> 8) & 0xFF00FF;
    uint32_t g  = (((s & 0x00FF00) * a + (d & 0x00FF00) * na) >> 8) & 0x00FF00;
    return rb | g;
}

static void mask_mem(uint32_t* buf, int bw, int bh, int x, int y, int w, int h,
                     const uint8_t* a, uint32_t c) {
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if ((unsigned)py >= (unsigned)bh) continue;
        uint32_t* row = buf + py * bw;
        for (int k = 0; k < w; k++) {
            int px = x + k;
            uint32_t al = a[r * w + k];
            if (!al || (unsigned)px >= (unsigned)bw) continue;
            row[px] = al == 255 ? c : mixc(row[px], c, al + (al >> 7));
        }
    }
}

int uif_draw_mem(uint32_t* buf, int bw, int bh, int x, int y, int font,
                 const char* s, uint32_t color) {
    if (font == UIF_MONO) {
        for (; *s; s++, x += UIF_MONO_W) {
            uint8_t c = (uint8_t)*s;
            if (uif_mono_have[c]) {
                mask_mem(buf, bw, bh, x, y, UIF_MONO_W, UIF_MONO_H,
                         uif_mono_px + (uint32_t)c * UIF_MONO_W * UIF_MONO_H, color);
                continue;
            }
            const uint8_t* g = font_glyph(c);
            for (int r = 0; r < FONT_H; r++)
                for (int b = 0; b < FONT_W; b++)
                    if ((g[r] & (0x80 >> b)) && (unsigned)(x + b) < (unsigned)bw &&
                        (unsigned)(y + r) < (unsigned)bh)
                        buf[(y + r) * bw + x + b] = color;
        }
        return x;
    }
    const uif_face_t* fc = face((uif_t)font);
    int pen = x << 6;
    for (; *s; s++) {
        const uif_glyph_t* g = glyph(fc, (uint8_t)*s);
        if (g->w)
            mask_mem(buf, bw, bh, ((pen + 32) >> 6) + g->x, y + g->y, g->w, g->h,
                     fc->px + g->off, color);
        pen += g->adv;
    }
    return (pen + 32) >> 6;
}

int uif_width_any(int font, const char* s) {
    return font == UIF_MONO ? (int)strlen(s) * UIF_MONO_W : uif_width((uif_t)font, s);
}

int uif_height_any(int font) { return font == UIF_MONO ? UIF_MONO_H : uif_height((uif_t)font); }

void uif_mono_cell(int x, int y, uint8_t c, uint32_t fg, uint32_t bg) {
    if (!uif_mono_have[c]) { gfx_glyph(x, y, (char)c, fg, bg, true); return; }
    gfx_rect_fill(x, y, UIF_MONO_W, UIF_MONO_H, bg);
    gfx_alpha_mask(x, y, UIF_MONO_W, UIF_MONO_H,
                   uif_mono_px + (uint32_t)c * UIF_MONO_W * UIF_MONO_H, UIF_MONO_W, fg);
}
