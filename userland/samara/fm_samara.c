#include "samara.h"
#include "fm_be.h"

static SmWin *W;

int be_open(int w, int h, const char *title) {
    W = sm_open_ex(w, h, 1, SM_F_RAW | SM_F_RESIZE, title);
    if (!W) W = sm_open_ex(800, 500, 1, SM_F_RAW | SM_F_RESIZE, title);
    if (!W) W = sm_open_ex(640, 400, 1, SM_F_RAW | SM_F_RESIZE, title);
    return W ? 0 : -1;
}

void be_close(void) { sm_close(W); W = 0; }
int be_w(void) { return W->w; }
int be_h(void) { return W->h; }
void be_title(const char *t) { sm_title(W, t); }
void be_fill(int x, int y, int w, int h, uint32_t c) { sm_rect(W, x, y, w, h, c); }
void be_line(int x0, int y0, int x1, int y1, uint32_t c) { sm_line(W, x0, y0, x1, y1, c); }
int be_text_w(const char *s, int font) { return sm_text_w(s, font == F_MONO ? SM_FONT_MONO : font); }
int be_font_h(int font) { return sm_font_h(font == F_MONO ? SM_FONT_MONO : font); }
void be_clip_set(const char *s, int n) { sm_clip_set(s, n); }
int be_flip(void) { return sm_present(W); }

void be_text(int x, int y, const char *s, uint32_t c, int font) {
    sm_text(W, x, y, s, c, font == F_MONO ? SM_FONT_MONO : font);
}

void be_blit(int x, int y, int w, int h, const uint32_t *src, int sw, int sh) {
    int i, j;
    for (j = 0; j < h; j++) {
        int dy = y + j;
        const uint32_t *row = src + (long)(j * sh / h) * sw;
        if (dy < 0 || dy >= W->h) continue;
        for (i = 0; i < w; i++) {
            int dx = x + i;
            if (dx < 0 || dx >= W->w) continue;
            W->pix[dy * W->w + dx] = row[i * sw / w] & 0xFFFFFF;
        }
    }
}

static int mods_now(void) {
    uint8_t k[32];
    int m = 0;
    sm_keys(W, k);
    if (sm_key_down(k, 42) || sm_key_down(k, 54)) m |= MOD_SHIFT;
    if (sm_key_down(k, 29) || sm_key_down(k, 97)) m |= MOD_CTRL;
    if (sm_key_down(k, 56) || sm_key_down(k, 100)) m |= MOD_ALT;
    return m;
}

/* the kernel sends arrows as 0x81.. which is also Б, В.. in cp866 (ru layout).
   if the real key isn't held down it's a letter */
static int key_held(int ch) {
    uint8_t k[32];
    static const uint8_t code[] = { 103, 108, 105, 106, 102, 107, 111, 104, 109 };
    sm_keys(W, k);
    if (ch >= 0x81 && ch <= 0x89) return sm_key_down(k, code[ch - 0x81]);
    if (ch >= 0x90 && ch <= 0x99) return sm_key_down(k, 59 + ch - 0x90);
    return 0;
}

static int cp866_uni(int c) {
    if (c >= 0x80 && c <= 0xAF) return 0x410 + c - 0x80;
    if (c >= 0xE0 && c <= 0xEF) return 0x440 + c - 0xE0;
    if (c == 0xF0) return 0x401;
    if (c == 0xF1) return 0x451;
    return 0;
}

static int xlat_key(int ch, int mods) {
    static const int sp[] = { FK_UP, FK_DOWN, FK_LEFT, FK_RIGHT, FK_HOME, FK_END, FK_DEL, FK_PGUP, FK_PGDN };
    if (ch >= 0x80 && key_held(ch)) {
        if (ch <= 0x89) return sp[ch - 0x81];
        if (ch == 0x91) return FK_F2;
        if (ch == 0x94) return FK_F5;
        return 0;
    }
    if (ch >= 0x80) {
        int u = cp866_uni(ch);
        // ctrl+cyrillic -> same physical key on latin layout, so hotkeys work on ru
        if ((mods & MOD_CTRL) && u >= 0x430 && u <= 0x44F) return "f,dult;pbqrkvyjghcnea[wxio]sm'.z"[u - 0x430];
        return u;
    }
    if (ch == '\n') return FK_ENTER;
    if (ch == 0x1B) return FK_ESC;
    if (ch == '\t') return FK_TAB;
    if (ch == '\b') return (mods & MOD_CTRL) ? 'h' : FK_BACK;
    if (ch == 127) return FK_DEL;
    if ((mods & MOD_CTRL) && ch >= 1 && ch <= 26) return 'a' + ch - 1;
    return ch;
}

int be_wait(FmEv *e, int ms) {
    SmEvent s;
    for (;;) {
        int r = sm_event(W, &s, ms);
        if (r <= 0) return r;
        ms = 0;
        e->a = s.a; e->b = s.b; e->c = s.c;
        e->mods = 0;
        switch (s.type) {
        case SM_EV_KEY:
            e->type = EV_KEY;
            e->mods = s.b;
            e->a = xlat_key(s.a, s.b);
            e->b = s.b;
            if (!e->a) continue;
            return 1;
        case SM_EV_MOUSE_DOWN: e->type = EV_DOWN; e->c = 1; e->mods = mods_now(); return 1;
        case SM_EV_RDOWN: e->type = EV_DOWN; e->c = 2; e->mods = mods_now(); return 1;
        case SM_EV_MOUSE_UP: e->type = EV_UP; e->c = 1; return 1;
        case SM_EV_MOUSE_MOVE: e->type = EV_MOVE; return 1;
        case SM_EV_WHEEL: e->type = EV_WHEEL; return 1;
        case SM_EV_RESIZE:
            if (sm_resize(W, s.a, s.b) < 0) continue;
            e->type = EV_RESIZE;
            return 1;
        case SM_EV_CLOSE: e->type = EV_CLOSE; return 1;
        }
    }
}
