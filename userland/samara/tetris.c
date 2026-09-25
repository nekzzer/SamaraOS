/* Tetris for SamaraOS on samara.h.
   Build inside the OS:  tcc /usr/src/games/tetris.c -o /usr/bin/tetris
   Keys: left/right move, up rotate, down soft drop, space hard drop,
         P pause, R restart, Esc quit. */
#include <stdio.h>
#include <samara.h>

#define COLS 10
#define ROWS 20
#define CELL 8                       /* logical px; scale 2 -> 16 on screen */
#define FX 6
#define FY 6
#define WW 160
#define WH 220

static const uint16_t SHAPES[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },   /* I */
    { 0x44C0, 0x8E00, 0x6440, 0x0E20 },   /* J */
    { 0x4460, 0x0E80, 0xC440, 0x2E00 },   /* L */
    { 0xCC00, 0xCC00, 0xCC00, 0xCC00 },   /* O */
    { 0x06C0, 0x8C40, 0x6C00, 0x4620 },   /* S */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 },   /* T */
    { 0x0C60, 0x4C80, 0xC600, 0x2640 },   /* Z */
};
static const uint32_t COLORS[8] = {
    SM_RGB(0x23, 0x26, 0x2C),
    SM_RGB(0x4C, 0xC2, 0xD2), SM_RGB(0x3E, 0x7C, 0xCF), SM_RGB(0xE3, 0x9B, 0x32),
    SM_RGB(0xF2, 0xC9, 0x4C), SM_RGB(0x74, 0xB0, 0x5E), SM_RGB(0xB0, 0x5E, 0xC8),
    SM_RGB(0xCF, 0x4A, 0x3E),
};

static uint8_t field[ROWS][COLS];
static int cur, rot, px, py, next_piece;
static int score, lines, level, over, paused;

static int cell_of(int piece, int r, int y, int x) {
    return (SHAPES[piece][r & 3] >> (15 - (y * 4 + x))) & 1;
}

static int fits(int piece, int r, int x0, int y0) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cell_of(piece, r, y, x)) continue;
            int fx = x0 + x, fy = y0 + y;
            if (fx < 0 || fx >= COLS || fy >= ROWS) return 0;
            if (fy >= 0 && field[fy][fx]) return 0;
        }
    return 1;
}

static void spawn(void) {
    cur = next_piece;
    next_piece = rand() % 7;
    rot = 0; px = 3; py = cur == 0 ? -1 : 0;
    if (!fits(cur, rot, px, py)) over = 1;
}

static void reset(void) {
    memset(field, 0, sizeof field);
    score = lines = 0; level = 1; over = paused = 0;
    next_piece = rand() % 7;
    spawn();
}

static void lock_piece(void) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell_of(cur, rot, y, x) && py + y >= 0) field[py + y][px + x] = (uint8_t)(cur + 1);
    int cleared = 0;
    for (int y = ROWS - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < COLS; x++) if (!field[y][x]) { full = 0; break; }
        if (!full) continue;
        memmove(field[1], field[0], (size_t)y * COLS);
        memset(field[0], 0, COLS);
        cleared++;
        y++;                                   /* re-check the row that fell in */
    }
    static const int pts[5] = { 0, 100, 300, 500, 800 };
    score += pts[cleared] * level;
    lines += cleared;
    level = 1 + lines / 10;
    spawn();
}

static int step_down(void) {
    if (fits(cur, rot, px, py + 1)) { py++; return 1; }
    lock_piece();
    return 0;
}

static void rotate(void) {
    static const int kicks[] = { 0, -1, 1, -2, 2 };
    for (int i = 0; i < 5; i++)
        if (fits(cur, rot + 1, px + kicks[i], py)) { rot = (rot + 1) & 3; px += kicks[i]; return; }
}

static void block(SmWin *w, int x, int y, uint32_t c) {
    sm_rect(w, x, y, CELL, CELL, c);
    sm_hline(w, x, y, CELL, c | 0x404040);
    sm_vline(w, x, y, CELL, c | 0x404040);
    sm_hline(w, x, y + CELL - 1, CELL, (c >> 1) & 0x7F7F7F);
    sm_vline(w, x + CELL - 1, y, CELL, (c >> 1) & 0x7F7F7F);
}

static void draw(SmWin *w) {
    char buf[32];
    sm_clear(w, SM_RGB(0x17, 0x19, 0x1D));
    sm_frame(w, FX - 2, FY - 2, COLS * CELL + 4, ROWS * CELL + 4, SM_RGB(0x40, 0x44, 0x4C));
    sm_rect(w, FX, FY, COLS * CELL, ROWS * CELL, COLORS[0]);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (field[y][x]) block(w, FX + x * CELL, FY + y * CELL, COLORS[field[y][x]]);
    if (!over) {
        int gy = py;                                   /* ghost */
        while (fits(cur, rot, px, gy + 1)) gy++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                if (!cell_of(cur, rot, y, x)) continue;
                if (gy + y >= 0) sm_frame(w, FX + (px + x) * CELL, FY + (gy + y) * CELL, CELL, CELL, SM_RGB(0x55, 0x5A, 0x64));
                if (py + y >= 0) block(w, FX + (px + x) * CELL, FY + (py + y) * CELL, COLORS[cur + 1]);
            }
    }
    int ix = FX + COLS * CELL + 8;
    sm_text(w, ix, 4, "NEXT", SM_GRAY, SM_FONT_MONO);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell_of(next_piece, 0, y, x)) block(w, ix + 4 + x * CELL, 24 + y * CELL, COLORS[next_piece + 1]);
    sm_text(w, ix, 64, "SCORE", SM_GRAY, SM_FONT_MONO);
    snprintf(buf, sizeof buf, "%d", score);
    sm_text(w, ix, 80, buf, SM_WHITE, SM_FONT_MONO);
    sm_text(w, ix, 104, "LINES", SM_GRAY, SM_FONT_MONO);
    snprintf(buf, sizeof buf, "%d", lines);
    sm_text(w, ix, 120, buf, SM_WHITE, SM_FONT_MONO);
    sm_text(w, ix, 144, "LEVEL", SM_GRAY, SM_FONT_MONO);
    snprintf(buf, sizeof buf, "%d", level);
    sm_text(w, ix, 160, buf, SM_WHITE, SM_FONT_MONO);
    sm_text(w, 6, 196, "\x18 rotate  spc drop", SM_RGB(0x60, 0x66, 0x70), SM_FONT_MONO);
    if (over || paused) {
        sm_rect(w, FX, FY + 64, COLS * CELL, 34, SM_RGB(0x0F, 0x10, 0x13));
        sm_text(w, FX + (over ? 4 : 16), FY + 68, over ? "GAME OVER" : "PAUSED", SM_ORANGE, SM_FONT_MONO);
        sm_text(w, FX + 4, FY + 82, over ? "R = again" : "P = go", SM_GRAY, SM_FONT_MONO);
    }
}

int main(void) {
    SmWin *w = sm_open(WW, WH, 2, "Tetris");
    if (!w) { fprintf(stderr, "tetris: no desktop (run `desktop` first)\n"); return 1; }
    srand(sm_ticks());
    reset();

    uint8_t keys[32], prev[32];
    memset(prev, 0, sizeof prev);
    uint32_t next_fall = sm_ticks(), rep_at[3] = { 0, 0, 0 };
    int shown_score = -1, quit = 0;
    static const int rep_key[3] = { SMK_LEFT, SMK_RIGHT, SMK_DOWN };

    while (!quit) {
        SmEvent e;
        while (sm_event(w, &e, 0) > 0) {
            if (e.type == SM_EV_CLOSE) quit = 1;
            if (e.type != SM_EV_KEY) continue;
            if (e.a == SM_CH_ESC) quit = 1;
            else if (e.a == 'p' || e.a == 'P') { if (!over) paused = !paused; }
            else if (e.a == 'r' || e.a == 'R') { reset(); next_fall = sm_ticks(); }
        }
        uint32_t now = sm_ticks();
        sm_keys(w, keys);
        if (!over && !paused) {
            for (int i = 0; i < 3; i++) {
                int k = rep_key[i];
                if (!sm_key_down(keys, k)) continue;
                int fire = 0;
                if (!sm_key_down(prev, k)) { fire = 1; rep_at[i] = now + (i == 2 ? 40 : 170); }
                else if ((int32_t)(now - rep_at[i]) >= 0) { fire = 1; rep_at[i] = now + (i == 2 ? 40 : 50); }
                if (!fire) continue;
                if (i == 0 && fits(cur, rot, px - 1, py)) px--;
                if (i == 1 && fits(cur, rot, px + 1, py)) px++;
                if (i == 2) { if (step_down()) score += 1; next_fall = now + 800; }
            }
            if (sm_key_down(keys, SMK_UP) && !sm_key_down(prev, SMK_UP)) rotate();
            if (sm_key_down(keys, SMK_SPACE) && !sm_key_down(prev, SMK_SPACE)) {
                while (fits(cur, rot, px, py + 1)) { py++; score += 2; }
                lock_piece();
                next_fall = now;
            }
            int interval = 800 - (level - 1) * 70;
            if (interval < 90) interval = 90;
            if ((int32_t)(now - next_fall) >= 0) { step_down(); next_fall = now + interval; }
        }
        memcpy(prev, keys, sizeof prev);

        if (score != shown_score) {
            char t[48];
            snprintf(t, sizeof t, "Tetris - %d", score);
            sm_title(w, t);
            shown_score = score;
        }
        draw(w);
        if (sm_present(w) < 0) break;
        sm_sleep_ms(16);
    }
    sm_close(w);
    return 0;
}
