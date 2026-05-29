/* SamaraOS Media Player — plays built-in tunes through the PC speaker
   (PIT channel 2). Runs as a WM_APP window with on_paint / on_tick / on_click /
   on_key callbacks. The window manager paces it via wm_run's frame pacer. */

#include "mediaplayer.h"
#include "wm.h"
#include "gfx.h"
#include "pit.h"
#include "io.h"
#include "string.h"
#include "keyboard.h"

/* ------------------------------------------------------------------ */
/*  PC speaker driver (PIT channel 2)                                  */
/* ------------------------------------------------------------------ */

static void spk_off(void) {
    uint8_t t = inb(0x61);
    if (t & 3) outb(0x61, t & 0xFC);
}
static void spk_on(uint16_t freq) {
    if (freq == 0) { spk_off(); return; }
    if (freq < 30)    freq = 30;
    if (freq > 12000) freq = 12000;
    uint32_t div = 1193180U / (uint32_t)freq;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t t = inb(0x61);
    if (!(t & 3)) outb(0x61, t | 3);
}

/* ------------------------------------------------------------------ */
/*  Score data                                                         */
/* ------------------------------------------------------------------ */

typedef struct { uint16_t freq; uint16_t ms; } note_t;

#define R(d) { 0, (d) }
/* Equal-tempered, A4 = 440 */
#define A3  220
#define AS3 233
#define B3  247
#define C4  262
#define CS4 277
#define D4  294
#define DS4 311
#define E4  330
#define F4  349
#define FS4 370
#define G4  392
#define GS4 415
#define A4  440
#define AS4 466
#define B4  494
#define C5  523
#define CS5 554
#define D5  587
#define DS5 622
#define E5  659
#define F5  698
#define FS5 740
#define G5  784
#define GS5 831
#define A5  880
#define AS5 932
#define B5  988
#define C6  1047

/* ---- Tetris (Korobeiniki) ---- */
#define Q  300
#define E_ 150
#define H_ 600
static const note_t tetris_notes[] = {
    {E5, Q}, {B4, E_}, {C5, E_}, {D5, Q}, {C5, E_}, {B4, E_},
    {A4, Q}, {A4, E_}, {C5, E_}, {E5, Q}, {D5, E_}, {C5, E_},
    {B4, Q + E_}, {C5, E_}, {D5, Q}, {E5, Q},
    {C5, Q}, {A4, Q}, {A4, H_}, R(E_),

    {D5, Q + E_}, {F5, E_}, {A5, Q}, {G5, E_}, {F5, E_},
    {E5, Q + E_}, {C5, E_}, {E5, Q}, {D5, E_}, {C5, E_},
    {B4, Q}, {B4, E_}, {C5, E_}, {D5, Q}, {E5, Q},
    {C5, Q}, {A4, Q}, {A4, H_}, R(Q),
};

/* ---- Ode to Joy (Beethoven) ---- */
#define O_Q 350
#define O_E 175
#define O_H 700
static const note_t joy_notes[] = {
    {E4, O_Q}, {E4, O_Q}, {F4, O_Q}, {G4, O_Q},
    {G4, O_Q}, {F4, O_Q}, {E4, O_Q}, {D4, O_Q},
    {C4, O_Q}, {C4, O_Q}, {D4, O_Q}, {E4, O_Q},
    {E4, O_Q + O_E}, {D4, O_E}, {D4, O_H},

    {E4, O_Q}, {E4, O_Q}, {F4, O_Q}, {G4, O_Q},
    {G4, O_Q}, {F4, O_Q}, {E4, O_Q}, {D4, O_Q},
    {C4, O_Q}, {C4, O_Q}, {D4, O_Q}, {E4, O_Q},
    {D4, O_Q + O_E}, {C4, O_E}, {C4, O_H},
};

/* ---- Imperial March (very short hook) ---- */
#define IM_Q 420
#define IM_E 200
#define IM_T 140
static const note_t imperial_notes[] = {
    {A4, IM_Q}, {A4, IM_Q}, {A4, IM_Q},
    {F4, IM_Q - IM_T}, {C5, IM_T}, {A4, IM_Q},
    {F4, IM_Q - IM_T}, {C5, IM_T}, {A4, IM_Q + IM_Q},
    R(IM_E),
    {E5, IM_Q}, {E5, IM_Q}, {E5, IM_Q},
    {F5, IM_Q - IM_T}, {C5, IM_T}, {GS4, IM_Q},
    {F4, IM_Q - IM_T}, {C5, IM_T}, {A4, IM_Q + IM_Q},
};

/* ---- Mary Had a Little Lamb ---- */
#define M_Q 380
#define M_H 760
static const note_t mary_notes[] = {
    {E4, M_Q}, {D4, M_Q}, {C4, M_Q}, {D4, M_Q},
    {E4, M_Q}, {E4, M_Q}, {E4, M_H},
    {D4, M_Q}, {D4, M_Q}, {D4, M_H},
    {E4, M_Q}, {G4, M_Q}, {G4, M_H},
    {E4, M_Q}, {D4, M_Q}, {C4, M_Q}, {D4, M_Q},
    {E4, M_Q}, {E4, M_Q}, {E4, M_Q}, {E4, M_Q},
    {D4, M_Q}, {D4, M_Q}, {E4, M_Q}, {D4, M_Q}, {C4, M_H},
};

/* ---- Twinkle Twinkle Little Star ---- */
#define T_Q 380
#define T_H 760
static const note_t twinkle_notes[] = {
    {C4, T_Q}, {C4, T_Q}, {G4, T_Q}, {G4, T_Q},
    {A4, T_Q}, {A4, T_Q}, {G4, T_H},
    {F4, T_Q}, {F4, T_Q}, {E4, T_Q}, {E4, T_Q},
    {D4, T_Q}, {D4, T_Q}, {C4, T_H},
    {G4, T_Q}, {G4, T_Q}, {F4, T_Q}, {F4, T_Q},
    {E4, T_Q}, {E4, T_Q}, {D4, T_H},
    {G4, T_Q}, {G4, T_Q}, {F4, T_Q}, {F4, T_Q},
    {E4, T_Q}, {E4, T_Q}, {D4, T_H},
    {C4, T_Q}, {C4, T_Q}, {G4, T_Q}, {G4, T_Q},
    {A4, T_Q}, {A4, T_Q}, {G4, T_H},
    {F4, T_Q}, {F4, T_Q}, {E4, T_Q}, {E4, T_Q},
    {D4, T_Q}, {D4, T_Q}, {C4, T_H},
};

typedef struct {
    const char* name;
    const char* artist;
    const note_t* notes;
    int count;
} track_t;

static track_t g_tracks[] = {
    { "Korobeiniki",       "Russian folk (Tetris)",   tetris_notes,   (int)(sizeof tetris_notes / sizeof tetris_notes[0]) },
    { "Ode to Joy",        "L. van Beethoven",        joy_notes,      (int)(sizeof joy_notes / sizeof joy_notes[0])       },
    { "Imperial March",    "J. Williams (Star Wars)", imperial_notes, (int)(sizeof imperial_notes / sizeof imperial_notes[0]) },
    { "Mary Had a Lamb",   "Traditional",             mary_notes,     (int)(sizeof mary_notes / sizeof mary_notes[0])     },
    { "Twinkle Twinkle",   "Traditional",             twinkle_notes,  (int)(sizeof twinkle_notes / sizeof twinkle_notes[0])},
};
static const int N_TRACKS = (int)(sizeof g_tracks / sizeof g_tracks[0]);

static uint32_t track_total_ms(int t) {
    uint32_t s = 0;
    for (int i = 0; i < g_tracks[t].count; i++) s += g_tracks[t].notes[i].ms;
    return s;
}

/* ------------------------------------------------------------------ */
/*  Player state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int      cur;
    bool     playing;
    bool     repeat;
    int      note_idx;
    uint32_t note_elapsed_ms;
    uint32_t elapsed_ms;
    uint32_t last_tick_ms;

    /* Visualizer */
    int      bars[14];
    uint32_t vis_last_ms;
    uint32_t vis_seed;

    /* Button rectangles (filled during paint, used by click) */
    int      btn_prev_x, btn_play_x, btn_stop_x, btn_next_x, btn_loop_x;
    int      btn_y, btn_w, btn_h;
    int      list_x, list_y, list_w, list_row_h;
} player_t;

static player_t P;
static window_t* g_player_win = NULL;

static void load_track(int idx) {
    if (idx < 0) idx = N_TRACKS - 1;
    if (idx >= N_TRACKS) idx = 0;
    P.cur = idx;
    P.note_idx = 0;
    P.note_elapsed_ms = 0;
    P.elapsed_ms = 0;
}

static void player_stop(void) {
    P.playing = false;
    spk_off();
}
static void player_play_pause(void) {
    P.playing = !P.playing;
    if (P.playing) {
        const note_t* n = &g_tracks[P.cur].notes[P.note_idx];
        spk_on(n->freq);
    } else {
        spk_off();
    }
}
static void player_next(void) {
    int t = (P.cur + 1) % N_TRACKS;
    bool was = P.playing;
    player_stop();
    load_track(t);
    if (was) { P.playing = true; spk_on(g_tracks[P.cur].notes[0].freq); }
}
static void player_prev(void) {
    int t = (P.cur - 1 + N_TRACKS) % N_TRACKS;
    bool was = P.playing;
    player_stop();
    load_track(t);
    if (was) { P.playing = true; spk_on(g_tracks[P.cur].notes[0].freq); }
}

/* ------------------------------------------------------------------ */
/*  Colours                                                            */
/* ------------------------------------------------------------------ */
#define COL_BG       RGB(0x18, 0x1B, 0x2A)
#define COL_PANEL    RGB(0x22, 0x26, 0x3A)
#define COL_PANEL_HI RGB(0x2C, 0x32, 0x4A)
#define COL_FG       RGB(0xE6, 0xE8, 0xF2)
#define COL_DIM      RGB(0x88, 0x90, 0xB0)
#define COL_ACCENT   RGB(0x6E, 0xA8, 0xFE)
#define COL_ACCENT2  RGB(0x9B, 0x6E, 0xFE)
#define COL_PROG_BG  RGB(0x10, 0x12, 0x1E)
#define COL_PROG_FG  RGB(0x44, 0xC8, 0x9A)
#define COL_BTN      RGB(0x30, 0x38, 0x58)
#define COL_BTN_HOT  RGB(0x44, 0x52, 0x80)
#define COL_OK       RGB(0x44, 0xC8, 0x9A)
#define COL_BAR_LO   RGB(0x3C, 0xC0, 0x90)
#define COL_BAR_HI   RGB(0xFF, 0xC8, 0x40)
#define COL_RED      RGB(0xE0, 0x60, 0x60)

/* ------------------------------------------------------------------ */
/*  Drawing helpers                                                    */
/* ------------------------------------------------------------------ */

static void fmt_mmss(uint32_t ms, char* out) {
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    s = s % 60;
    if (m > 99) m = 99;
    out[0] = '0' + (char)((m / 10) % 10);
    out[1] = '0' + (char)(m % 10);
    out[2] = ':';
    out[3] = '0' + (char)((s / 10) % 10);
    out[4] = '0' + (char)(s % 10);
    out[5] = 0;
}

static void draw_button(int x, int y, int w, int h, const char* label,
                        bool hot, uint32_t face) {
    gfx_rect_fill(x, y, w, h, hot ? COL_BTN_HOT : face);
    gfx_rect(x, y, w, h, COL_ACCENT);
    int lw = (int)strlen(label) * 8;
    gfx_string(x + (w - lw) / 2, y + (h - 16) / 2, label, COL_FG,
               hot ? COL_BTN_HOT : face, false);
}

/* ------------------------------------------------------------------ */
/*  WM callbacks                                                       */
/* ------------------------------------------------------------------ */

static void mp_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);

    gfx_rect_fill(cx, cy, cw, ch, COL_BG);

    /* === Header === */
    int hx = cx + 16, hy = cy + 14;
    gfx_string(hx, hy, "SamaraOS Music", COL_FG, COL_BG, false);
    gfx_string(hx, hy + 18, "PC-speaker chiptune player",  COL_DIM, COL_BG, false);

    /* === Now playing card === */
    int ncx = cx + 16, ncy = cy + 56, ncw = cw - 32, nch = 96;
    gfx_rect_fill(ncx, ncy, ncw, nch, COL_PANEL);
    gfx_rect(ncx, ncy, ncw, nch, COL_PANEL_HI);

    gfx_string(ncx + 14, ncy + 8, "NOW PLAYING", COL_ACCENT, COL_PANEL, false);
    gfx_string(ncx + 14, ncy + 26, g_tracks[P.cur].name,   COL_FG,  COL_PANEL, false);
    gfx_string(ncx + 14, ncy + 44, g_tracks[P.cur].artist, COL_DIM, COL_PANEL, false);

    /* progress bar */
    int pbx = ncx + 14, pby = ncy + 70, pbw = ncw - 28, pbh = 10;
    gfx_rect_fill(pbx, pby, pbw, pbh, COL_PROG_BG);
    uint32_t tot = track_total_ms(P.cur);
    uint32_t cur_ms = P.elapsed_ms;
    if (cur_ms > tot) cur_ms = tot;
    int fw = (tot > 0) ? (int)((uint64_t)pbw * cur_ms / tot) : 0;
    if (fw > pbw) fw = pbw;
    gfx_rect_fill(pbx, pby, fw, pbh, COL_PROG_FG);
    gfx_rect(pbx, pby, pbw, pbh, COL_DIM);

    char tbuf[6], tbuf2[6], line[24];
    fmt_mmss(cur_ms, tbuf);
    fmt_mmss(tot,    tbuf2);
    int j = 0;
    for (int i = 0; tbuf[i]; i++)  line[j++] = tbuf[i];
    line[j++] = ' '; line[j++] = '/'; line[j++] = ' ';
    for (int i = 0; tbuf2[i]; i++) line[j++] = tbuf2[i];
    line[j] = 0;
    int tw = (int)strlen(line) * 8;
    gfx_string(pbx + pbw - tw, ncy + 26, line, COL_DIM, COL_PANEL, false);

    /* === Visualizer === */
    int vx = cx + 16, vy = ncy + nch + 14, vw = cw - 32, vh = 56;
    gfx_rect_fill(vx, vy, vw, vh, COL_PROG_BG);
    gfx_rect(vx, vy, vw, vh, COL_PANEL_HI);
    int nb = (int)(sizeof P.bars / sizeof P.bars[0]);
    int bw = (vw - 16) / nb;
    if (bw < 4) bw = 4;
    int gap = 2;
    for (int i = 0; i < nb; i++) {
        int h = P.bars[i];
        if (h < 1) h = 1;
        if (h > vh - 6) h = vh - 6;
        int bx = vx + 8 + i * (bw + gap);
        if (bx + bw > vx + vw - 4) break;
        /* gradient: low part green, top part yellow */
        int low_h = h * 6 / 10;
        gfx_rect_fill(bx, vy + vh - 3 - low_h, bw, low_h, COL_BAR_LO);
        gfx_rect_fill(bx, vy + vh - 3 - h, bw, h - low_h, COL_BAR_HI);
    }

    /* === Buttons === */
    int by = vy + vh + 14;
    int bh = 32;
    int btnw = 64;
    int total = btnw * 5 + 16 * 4;
    int bx0 = cx + (cw - total) / 2;
    P.btn_y = by; P.btn_h = bh; P.btn_w = btnw;
    P.btn_prev_x = bx0;
    P.btn_play_x = bx0 + (btnw + 16) * 1;
    P.btn_stop_x = bx0 + (btnw + 16) * 2;
    P.btn_next_x = bx0 + (btnw + 16) * 3;
    P.btn_loop_x = bx0 + (btnw + 16) * 4;

    draw_button(P.btn_prev_x, by, btnw, bh, "|<<", false, COL_BTN);
    draw_button(P.btn_play_x, by, btnw, bh, P.playing ? "||" : ">", P.playing, COL_BTN);
    draw_button(P.btn_stop_x, by, btnw, bh, "[]",  false, COL_BTN);
    draw_button(P.btn_next_x, by, btnw, bh, ">>|", false, COL_BTN);
    draw_button(P.btn_loop_x, by, btnw, bh, "LOOP", P.repeat, P.repeat ? COL_OK : COL_BTN);

    /* === Playlist === */
    int ly = by + bh + 18;
    int row_h = 22;
    int lh = ch - (ly - cy) - 14;
    int lx = cx + 16, lw = cw - 32;
    P.list_x = lx; P.list_y = ly; P.list_w = lw; P.list_row_h = row_h;

    gfx_rect_fill(lx, ly, lw, lh, COL_PANEL);
    gfx_rect(lx, ly, lw, lh, COL_PANEL_HI);
    gfx_string(lx + 12, ly + 6, "PLAYLIST", COL_ACCENT, COL_PANEL, false);

    for (int i = 0; i < N_TRACKS; i++) {
        int ry = ly + 26 + i * row_h;
        if (ry + row_h > ly + lh) break;
        bool sel = (i == P.cur);
        if (sel) gfx_rect_fill(lx + 6, ry, lw - 12, row_h - 2, COL_PANEL_HI);

        char marker[3] = { sel && P.playing ? '>' : sel ? '*' : ' ', ' ', 0 };
        char ix[3]     = { (char)('1' + i), '.', 0 };
        gfx_string(lx + 12, ry + 3, marker, sel ? COL_ACCENT : COL_DIM,
                   sel ? COL_PANEL_HI : COL_PANEL, false);
        gfx_string(lx + 28, ry + 3, ix, COL_DIM, sel ? COL_PANEL_HI : COL_PANEL, false);
        gfx_string(lx + 50, ry + 3, g_tracks[i].name, COL_FG,
                   sel ? COL_PANEL_HI : COL_PANEL, false);
        char dur[6];
        fmt_mmss(track_total_ms(i), dur);
        gfx_string(lx + lw - 12 - 5*8, ry + 3, dur, COL_DIM,
                   sel ? COL_PANEL_HI : COL_PANEL, false);
    }
}

static void mp_tick(window_t* w, uint32_t now) {
    (void)w;
    uint32_t dt = 0;
    if (P.last_tick_ms != 0) {
        uint32_t d = now - P.last_tick_ms;
        if (d < 250) dt = d;
    }
    P.last_tick_ms = now;

    if (P.playing) {
        P.elapsed_ms += dt;
        P.note_elapsed_ms += dt;

        const track_t* t = &g_tracks[P.cur];
        while (P.note_idx < t->count && P.note_elapsed_ms >= t->notes[P.note_idx].ms) {
            P.note_elapsed_ms -= t->notes[P.note_idx].ms;
            P.note_idx++;
            if (P.note_idx >= t->count) {
                if (P.repeat) {
                    P.note_idx = 0;
                    P.elapsed_ms = P.note_elapsed_ms;
                    t = &g_tracks[P.cur];
                } else {
                    P.cur = (P.cur + 1) % N_TRACKS;
                    P.note_idx = 0;
                    P.elapsed_ms = P.note_elapsed_ms;
                    t = &g_tracks[P.cur];
                }
            }
            uint16_t f = t->notes[P.note_idx].freq;
            spk_on(f);
        }
    }

    /* Visualizer: animated bars (decay + random surge). Updated ~ every 50 ms. */
    if (now - P.vis_last_ms >= 50) {
        P.vis_last_ms = now;
        int nb = (int)(sizeof P.bars / sizeof P.bars[0]);
        for (int i = 0; i < nb; i++) {
            P.vis_seed = P.vis_seed * 1103515245u + 12345u;
            int target = P.playing ? (int)((P.vis_seed >> 16) % 48 + 6) : 0;
            int cur = P.bars[i];
            if (target > cur) cur += (target - cur) / 2 + 1;          /* attack */
            else              cur -= (cur - target) / 3 + (P.playing ? 0 : 2);   /* decay */
            if (cur < 0) cur = 0;
            if (cur > 60) cur = 60;
            P.bars[i] = cur;
        }
    }
}

static bool inside(int x, int y, int bx, int by, int bw, int bh) {
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static void mp_click(window_t* w, int rx, int ry) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    int ax = rx + cx, ay = ry + cy;     /* convert client-rel → absolute */

    /* Transport buttons */
    if (inside(ax, ay, P.btn_prev_x, P.btn_y, P.btn_w, P.btn_h)) { player_prev();        return; }
    if (inside(ax, ay, P.btn_play_x, P.btn_y, P.btn_w, P.btn_h)) { player_play_pause();  return; }
    if (inside(ax, ay, P.btn_stop_x, P.btn_y, P.btn_w, P.btn_h)) { player_stop(); load_track(P.cur); return; }
    if (inside(ax, ay, P.btn_next_x, P.btn_y, P.btn_w, P.btn_h)) { player_next();        return; }
    if (inside(ax, ay, P.btn_loop_x, P.btn_y, P.btn_w, P.btn_h)) { P.repeat = !P.repeat; return; }

    /* Playlist row */
    if (ax >= P.list_x && ax < P.list_x + P.list_w &&
        ay >= P.list_y + 26 && ay < P.list_y + 26 + N_TRACKS * P.list_row_h) {
        int row = (ay - P.list_y - 26) / P.list_row_h;
        if (row >= 0 && row < N_TRACKS) {
            bool was = P.playing;
            player_stop();
            load_track(row);
            if (was) { P.playing = true; spk_on(g_tracks[P.cur].notes[0].freq); }
        }
    }
}

static void mp_close(window_t* w) {
    (void)w;
    spk_off();
    P.playing = false;
    g_player_win = NULL;
}

static void mp_key(window_t* w, char c) {
    (void)w;
    if (c == ' ')                player_play_pause();
    else if (c == (char)K_RIGHT) player_next();
    else if (c == (char)K_LEFT)  player_prev();
    else if (c == (char)K_UP) {
        bool was = P.playing;
        player_stop();
        load_track((P.cur - 1 + N_TRACKS) % N_TRACKS);
        if (was) { P.playing = true; spk_on(g_tracks[P.cur].notes[0].freq); }
    } else if (c == (char)K_DOWN) {
        bool was = P.playing;
        player_stop();
        load_track((P.cur + 1) % N_TRACKS);
        if (was) { P.playing = true; spk_on(g_tracks[P.cur].notes[0].freq); }
    } else if (c == 'l' || c == 'L') {
        P.repeat = !P.repeat;
    } else if (c == 's' || c == 'S') {
        player_stop();
        load_track(P.cur);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int mediaplayer_open(void) {
    if (!gfx_ready()) return -1;
    if (g_player_win && g_player_win->open) return 0;       /* already up */

    /* Init state once per launch */
    memset(&P, 0, sizeof P);
    P.cur = 0;
    P.repeat = false;
    P.playing = false;
    P.vis_seed = 0xC0FFEEu ^ pit_uptime_ms();

    int W = gfx_w(), H = gfx_h();
    int ww = 640, wh = 460;
    if (ww > W - 40) ww = W - 40;
    if (wh > H - 80) wh = H - 80;
    int wx = (W - ww) / 2;
    int wy = (H - wh) / 2 - 20;
    if (wy < 30) wy = 30;

    g_player_win = wm_open_app_ex(wx, wy, ww, wh, "Music - SamaraOS",
                                   mp_paint, mp_key, mp_click, mp_tick,
                                   true, &P);
    if (g_player_win) g_player_win->on_close = mp_close;
    return g_player_win ? 0 : -1;
}

void mediaplayer_force_stop(void) {
    spk_off();
    P.playing = false;
    g_player_win = NULL;
}
