/* Classic snake in a WM window. Arrows/WASD = direction, R = restart. */

#include "apps/snake.h"
#include "gui/wm.h"
#include "gfx/gfx.h"
#include "core/string.h"
#include "drivers/keyboard.h"
#include "boot/pit.h"

#define GRID_W  24
#define GRID_H  18
#define CELL    20
#define BORDER  6
#define HDR_H   24
#define TICK_MS 120

#define COL_BG    RGB(0x10, 0x14, 0x20)
#define COL_GRID  RGB(0x18, 0x1D, 0x2C)
#define COL_SNAKE RGB(0x50, 0xD0, 0x60)
#define COL_HEAD  RGB(0x80, 0xF0, 0x90)
#define COL_FOOD  RGB(0xE0, 0x50, 0x60)
#define COL_TEXT  RGB(0xE6, 0xEA, 0xF4)
#define COL_DIM   RGB(0x80, 0x88, 0xA0)

typedef struct { int x, y; } pt_t;

static struct {
    pt_t body[GRID_W * GRID_H];
    int  len;
    int  dx, dy, ndx, ndy;
    pt_t food;
    int  score, best;
    bool dead, paused;
    uint32_t next_tick;
    uint32_t rng;
} S;

static window_t* g_win = NULL;

static uint32_t xorshift(void) {
    uint32_t x = S.rng;
    if (!x) x = 0xCAFEBABE;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    S.rng = x;
    return x;
}

static bool cell_in_snake(int x, int y) {
    for (int i = 0; i < S.len; i++)
        if (S.body[i].x == x && S.body[i].y == y) return true;
    return false;
}

static void place_food(void) {
    for (int t = 0; t < 256; t++) {
        int x = xorshift() % GRID_W;
        int y = xorshift() % GRID_H;
        if (!cell_in_snake(x, y)) { S.food.x = x; S.food.y = y; return; }
    }
    S.food.x = 0; S.food.y = 0;
}

static void snake_reset(void) {
    S.len = 4;
    int cx = GRID_W / 2, cy = GRID_H / 2;
    for (int i = 0; i < S.len; i++) { S.body[i].x = cx - i; S.body[i].y = cy; }
    S.dx = 1; S.dy = 0; S.ndx = 1; S.ndy = 0;
    S.score = 0;
    S.dead = false;
    S.paused = false;
    S.next_tick = pit_uptime_ms() + TICK_MS;
    place_food();
}

static void step(void) {
    if (S.dead || S.paused) return;
    S.dx = S.ndx; S.dy = S.ndy;
    int nx = S.body[0].x + S.dx;
    int ny = S.body[0].y + S.dy;
    if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) { S.dead = true; return; }
    /* hit self (exclude tail because tail will move) */
    for (int i = 0; i < S.len - 1; i++) {
        if (S.body[i].x == nx && S.body[i].y == ny) { S.dead = true; return; }
    }
    bool grew = (nx == S.food.x && ny == S.food.y);
    if (grew && S.len < GRID_W * GRID_H) {
        S.len++;
    }
    for (int i = S.len - 1; i > 0; i--) S.body[i] = S.body[i-1];
    S.body[0].x = nx; S.body[0].y = ny;
    if (grew) {
        S.score++;
        if (S.score > S.best) S.best = S.score;
        place_food();
    }
}

static void on_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_rect_fill(cx, cy, cw, ch, COL_BG);

    /* header */
    char info[64];
    int n = 0;
    const char* s1 = "score: ";
    while (*s1) info[n++] = *s1++;
    char tmp[16]; itoa(S.score, tmp, 10);
    for (char* p = tmp; *p; p++) info[n++] = *p;
    const char* s2 = "   best: ";
    while (*s2) info[n++] = *s2++;
    itoa(S.best, tmp, 10);
    for (char* p = tmp; *p; p++) info[n++] = *p;
    info[n] = 0;
    gfx_string(cx + 8, cy + 6, info, COL_TEXT, 0, false);

    /* Grid centred in whatever room the (resizable) window gives it. */
    int gx = cx + (cw - GRID_W * CELL) / 2;
    int gy = cy + HDR_H + (ch - HDR_H - BORDER - 24 - GRID_H * CELL) / 2;
    if (gx < cx + BORDER) gx = cx + BORDER;
    if (gy < cy + HDR_H) gy = cy + HDR_H;

    /* grid backdrop */
    gfx_rect_fill(gx - 2, gy - 2, GRID_W * CELL + 4, GRID_H * CELL + 4, COL_GRID);

    /* food */
    int fx = gx + S.food.x * CELL;
    int fy = gy + S.food.y * CELL;
    gfx_rect_fill(fx + 3, fy + 3, CELL - 6, CELL - 6, COL_FOOD);

    /* snake */
    for (int i = 0; i < S.len; i++) {
        int x = gx + S.body[i].x * CELL;
        int y = gy + S.body[i].y * CELL;
        uint32_t col = (i == 0) ? COL_HEAD : COL_SNAKE;
        gfx_rect_fill(x + 1, y + 1, CELL - 2, CELL - 2, col);
    }

    if (S.dead) {
        gfx_string(cx + cw/2 - 60, cy + ch/2 - 10, "GAME OVER (R = restart)",
                   RGB(0xFF, 0x90, 0x90), 0, false);
    } else if (S.paused) {
        gfx_string(cx + cw/2 - 30, cy + ch/2 - 10, "PAUSED", COL_TEXT, 0, false);
    }

    gfx_string(cx + 8, cy + ch - 14,
               "arrows/WASD move  P pause  R restart  Esc close",
               COL_DIM, 0, false);
}

static void on_key(window_t* w, char c) {
    (void)w;
    if (c == 0) return;
    if (c == 'r' || c == 'R') { snake_reset(); return; }
    if (c == 'p' || c == 'P') { if (!S.dead) S.paused = !S.paused; return; }
    int ndx = S.dx, ndy = S.dy;
    if      (c == (char)K_UP    || c == 'w' || c == 'W') { ndx =  0; ndy = -1; }
    else if (c == (char)K_DOWN  || c == 's' || c == 'S') { ndx =  0; ndy =  1; }
    else if (c == (char)K_LEFT  || c == 'a' || c == 'A') { ndx = -1; ndy =  0; }
    else if (c == (char)K_RIGHT || c == 'd' || c == 'D') { ndx =  1; ndy =  0; }
    else return;
    /* prevent 180° turn */
    if (ndx == -S.dx && ndy == -S.dy && S.len > 1) return;
    S.ndx = ndx; S.ndy = ndy;
}

static void on_tick(window_t* w, uint32_t now) {
    (void)w;
    if (S.dead || S.paused) return;
    if ((int32_t)(now - S.next_tick) >= 0) {
        step();
        S.next_tick = now + TICK_MS;
    }
}

static void on_close(window_t* w) {
    (void)w;
    g_win = NULL;
}

int snake_open(void) {
    if (g_win) return 0;
    S.rng = pit_uptime_ms() ^ 0xC0FFEEu;
    if (!S.best) S.best = 0;
    snake_reset();
    int w = GRID_W * CELL + 2 * BORDER;
    int h = GRID_H * CELL + HDR_H + BORDER + 24;
    g_win = wm_open_app_ex(60, 40, w, h, "Snake",
                            on_paint, on_key, NULL, on_tick, true, NULL);
    if (!g_win) return -1;
    g_win->on_close = on_close;
    g_win->min_w = w;
    g_win->min_h = h;
    g_win->opaque = true;                     /* on_paint fills the client */
    return 0;
}
