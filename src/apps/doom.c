#include "apps/doom.h"
#include "drivers/ata.h"
#include "core/heap.h"
#include "core/string.h"
#include "gfx/gfx.h"
#include "gui/wm.h"
#include "drivers/keyboard.h"
#include "boot/pit.h"

/* ========================================================================
   DOOM mini-engine for SamaraOS
   ------------------------------------------------------------------------
   - WAD loader on top of ATA
   - Renders TITLEPIC from PLAYPAL (DOOM picture format)
   - Loads E1M1 VERTEXES + LINEDEFS + THINGS
   - 3D walkthrough via per-column raycast against all linedefs
   - WASD + arrows movement, point-to-segment collision against walls
   ======================================================================== */

/* ---- WAD format ---- */
typedef struct __attribute__((packed)) {
    uint32_t off;
    uint32_t size;
    char     name[8];
} wad_lump_t;

typedef struct __attribute__((packed)) {
    int16_t  width;
    int16_t  height;
    int16_t  leftoffset;
    int16_t  topoffset;
    uint32_t column_offsets[1];
} doom_pic_t;

typedef struct __attribute__((packed)) {
    int16_t x, y;
} d_vertex_t;

typedef struct __attribute__((packed)) {
    uint16_t v1, v2;
    uint16_t flags;
    uint16_t special;
    uint16_t tag;
    uint16_t front_sd;
    uint16_t back_sd;
} d_linedef_t;

typedef struct __attribute__((packed)) {
    int16_t x, y;
    int16_t angle;
    int16_t type;
    int16_t flags;
} d_thing_t;

#define LINE_TWOSIDED 0x0004

/* ---- loaded WAD bookkeeping ---- */
static uint8_t*    g_palette;          /* 768 */
static uint8_t*    g_titlepic;
static uint32_t    g_titlepic_size;
static char        g_status[80] = "no doom data loaded";
static bool        g_loaded = false;

static wad_lump_t* g_dir;
static uint32_t    g_numlumps;

/* ---- loaded level (E1M1) ---- */
static d_vertex_t* g_verts;
static int         g_n_verts;
static d_linedef_t* g_lines;
static int         g_n_lines;
static int         g_player_start_x;
static int         g_player_start_y;
static int         g_player_start_ang;     /* DOOM degrees, 0=east, 90=north */
static bool        g_level_loaded = false;

/* ---- helpers ---- */

static int load_lump_from_disk(uint32_t offset, uint32_t size, uint8_t** out) {
    if (size == 0) return -1;
    uint32_t first_sec = offset / 512;
    uint32_t off_in    = offset % 512;
    uint32_t total_sec = (size + off_in + 511) / 512;
    uint8_t* buf = (uint8_t*)kmalloc(total_sec * 512);
    if (!buf) return -1;
    if (ata_read_sectors(first_sec, (int)total_sec, buf) != 0) { kfree(buf); return -1; }
    uint8_t* result = (uint8_t*)kmalloc(size);
    if (!result) { kfree(buf); return -1; }
    memcpy(result, buf + off_in, size);
    kfree(buf);
    *out = result;
    return 0;
}

const char* doom_status(void) { return g_status; }
bool doom_loaded(void) { return g_loaded; }

int doom_load_from_disk(void) {
    if (g_loaded) return 0;

    if (!ata_present()) {
        if (!ata_init()) {
            strncpy(g_status, "no ATA disk (attach: -hda DOOM1.WAD,format=raw)", 79);
            return -1;
        }
    }

    uint8_t hdr[512];
    if (ata_read_sectors(0, 1, hdr) != 0) {
        strncpy(g_status, "ATA read sector 0 failed", 79); return -1;
    }
    if ((hdr[0] != 'I' && hdr[0] != 'P') || hdr[1] != 'W' || hdr[2] != 'A' || hdr[3] != 'D') {
        strncpy(g_status, "disk does not look like a WAD", 79); return -2;
    }
    uint32_t numlumps = *(uint32_t*)(hdr + 4);
    uint32_t dir_off  = *(uint32_t*)(hdr + 8);
    if (numlumps == 0 || numlumps > 100000) {
        strncpy(g_status, "WAD: bogus numlumps", 79); return -2;
    }

    /* directory */
    uint32_t dir_bytes = numlumps * sizeof(wad_lump_t);
    uint32_t first_sec = dir_off / 512;
    uint32_t off_in    = dir_off % 512;
    uint32_t total_sec = (dir_bytes + off_in + 511) / 512;
    uint8_t* dir_buf = (uint8_t*)kmalloc(total_sec * 512);
    if (!dir_buf) { strncpy(g_status, "OOM reading directory", 79); return -4; }
    if (ata_read_sectors(first_sec, (int)total_sec, dir_buf) != 0) {
        kfree(dir_buf); strncpy(g_status, "ATA read directory failed", 79); return -1;
    }
    /* keep directory copy for later level loads */
    g_dir = (wad_lump_t*)kmalloc(dir_bytes);
    if (!g_dir) { kfree(dir_buf); strncpy(g_status, "OOM dir copy", 79); return -4; }
    memcpy(g_dir, dir_buf + off_in, dir_bytes);
    g_numlumps = numlumps;
    kfree(dir_buf);

    uint32_t playpal_off = 0, playpal_size = 0;
    uint32_t titlepic_off = 0, titlepic_size = 0;
    for (uint32_t i = 0; i < numlumps; i++) {
        if (!memcmp(g_dir[i].name, "PLAYPAL", 7) && g_dir[i].name[7] == 0) {
            playpal_off  = g_dir[i].off; playpal_size = g_dir[i].size;
        } else if (!memcmp(g_dir[i].name, "TITLEPIC", 8)) {
            titlepic_off = g_dir[i].off; titlepic_size = g_dir[i].size;
        }
    }
    if (!playpal_size || !titlepic_size) {
        strncpy(g_status, "WAD missing PLAYPAL or TITLEPIC", 79); return -3;
    }

    if (load_lump_from_disk(playpal_off, 768, &g_palette) != 0) {
        strncpy(g_status, "ATA read PLAYPAL failed", 79); return -1;
    }
    if (load_lump_from_disk(titlepic_off, titlepic_size, &g_titlepic) != 0) {
        kfree(g_palette); g_palette = NULL;
        strncpy(g_status, "ATA read TITLEPIC failed", 79); return -1;
    }
    g_titlepic_size = titlepic_size;

    g_loaded = true;
    strncpy(g_status, "DOOM TITLEPIC + PLAYPAL loaded", 79);
    return 0;
}

/* ===== TITLEPIC window ===== */

static int g_tp_offx = 0, g_tp_offy = 0;

static void titlepic_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_rect_fill(cx, cy, cw, ch, RGB(0x10, 0x10, 0x10));

    if (!g_loaded || !g_titlepic || !g_palette) {
        gfx_string(cx + 8, cy + 8,  "DOOM data not loaded.", RGB(0xE0,0x60,0x60), 0, false);
        gfx_string(cx + 8, cy + 28, doom_status(),           RGB(0xC0,0xC0,0xC0), 0, false);
        return;
    }
    doom_pic_t* p = (doom_pic_t*)g_titlepic;
    int pw = p->width, ph = p->height;
    if (pw <= 0 || ph <= 0 || pw > 1024 || ph > 1024) return;

    int dx = cx + (cw - pw) / 2 + g_tp_offx;
    int dy = cy + (ch - ph) / 2 + g_tp_offy;

    for (int x = 0; x < pw; x++) {
        uint32_t coloff = p->column_offsets[x];
        if (coloff >= g_titlepic_size) continue;
        const uint8_t* col = g_titlepic + coloff;
        const uint8_t* end = g_titlepic + g_titlepic_size;
        while (col < end && *col != 0xFF) {
            uint8_t topdelta = *col++;
            if (col >= end) break;
            uint8_t length = *col++;
            if (col >= end) break;
            col++;
            for (int i = 0; i < length && col < end; i++) {
                int yy = topdelta + i;
                uint8_t pix = *col++;
                if (yy >= 0 && yy < ph) {
                    uint32_t pi = (uint32_t)pix * 3;
                    if (pi + 2 < 768) {
                        gfx_pixel(dx + x, dy + yy,
                                  RGB(g_palette[pi], g_palette[pi+1], g_palette[pi+2]));
                    }
                }
            }
            if (col < end) col++;
        }
    }
    gfx_string(cx + 4, cy + ch - 18, "Esc closes",
               RGB(0xC0, 0xC0, 0xC0), 0, false);
}

static void titlepic_key(window_t* w, char c) {
    if (c == 0x1B) { wm_close(w); return; }
    if (c == (char)K_LEFT)  g_tp_offx -= 4;
    if (c == (char)K_RIGHT) g_tp_offx += 4;
    if (c == (char)K_UP)    g_tp_offy -= 4;
    if (c == (char)K_DOWN)  g_tp_offy += 4;
}

void doom_open_window(void) {
    g_tp_offx = g_tp_offy = 0;
    wm_open_app(60, 60, 360, 244, "DOOM TITLEPIC", titlepic_paint, titlepic_key, NULL);
}

/* ============================================================
   E1M1 LEVEL LOADER + 3D RAYCAST RENDERER + MOVEMENT
   ============================================================ */

#define ANG_RES   1024              /* 0..1023 = 0..2π */
#define FIX_BITS  12
#define FIX_ONE   (1 << FIX_BITS)   /* 4096 */
#define PI_Q      12867
#define HPI_Q     6434
#define TPI_Q     25734

static int  g_sin[ANG_RES];
static bool g_trig_init = false;

static int sin_taylor(int x_q) {
    long long x = x_q;
    long long x2 = (x * x) / FIX_ONE;
    long long x3 = (x2 * x) / FIX_ONE;
    long long x5 = (x3 * x2) / FIX_ONE;
    long long x7 = (x5 * x2) / FIX_ONE;
    long long s = x - x3/6 + x5/120 - x7/5040;
    return (int)s;
}

static void init_trig(void) {
    if (g_trig_init) return;
    for (int i = 0; i < ANG_RES; i++) {
        long long ang_q = ((long long)i * TPI_Q) / ANG_RES;
        int sign = 1;
        if (ang_q > PI_Q) { ang_q -= PI_Q; sign = -1; }
        if (ang_q > HPI_Q) ang_q = PI_Q - ang_q;
        g_sin[i] = sign * sin_taylor((int)ang_q);
    }
    g_trig_init = true;
}

static inline int isin(int a) { return g_sin[ a            & (ANG_RES - 1)]; }
static inline int icos(int a) { return g_sin[(a + ANG_RES/4) & (ANG_RES - 1)]; }

static int isqrt_ll(long long n) {
    if (n <= 0) return 0;
    long long x = n / 2 + 1;
    if (x == 0) x = 1;
    for (int i = 0; i < 20; i++) {
        long long y = (x + n / x) / 2;
        if (y >= x) break;
        x = y;
    }
    return (int)x;
}

/* ---- locate E1M1 marker in directory ---- */
static int find_lump(const char* name8, int start_idx) {
    char target[8] = {0};
    for (int i = 0; i < 8 && name8[i]; i++) target[i] = name8[i];
    for (uint32_t i = (uint32_t)start_idx; i < g_numlumps; i++) {
        if (!memcmp(g_dir[i].name, target, 8)) return (int)i;
    }
    return -1;
}

static int load_level_e1m1(void) {
    if (g_level_loaded) return 0;
    if (!g_dir) {
        strncpy(g_status, "WAD directory not loaded; run 'doom' first", 79);
        return -1;
    }
    int e1m1 = find_lump("E1M1", 0);
    if (e1m1 < 0) { strncpy(g_status, "WAD: E1M1 marker not found", 79); return -2; }

    /* canonical sub-lump order:
       E1M1, THINGS, LINEDEFS, SIDEDEFS, VERTEXES, SEGS, SSECTORS, NODES, SECTORS, REJECT, BLOCKMAP */
    if ((uint32_t)e1m1 + 4 >= g_numlumps) { strncpy(g_status, "WAD: truncated E1M1", 79); return -2; }

    wad_lump_t* lthings   = &g_dir[e1m1 + 1];
    wad_lump_t* llinedefs = &g_dir[e1m1 + 2];
    wad_lump_t* lvertexes = &g_dir[e1m1 + 4];

    if (memcmp(lvertexes->name, "VERTEXES", 8) != 0 ||
        memcmp(llinedefs->name, "LINEDEFS", 8) != 0 ||
        memcmp(lthings->name,   "THINGS",   6) != 0) {
        strncpy(g_status, "WAD: unexpected E1M1 sub-lump layout", 79);
        return -2;
    }

    /* VERTEXES */
    uint8_t* vraw = NULL;
    if (load_lump_from_disk(lvertexes->off, lvertexes->size, &vraw) != 0) {
        strncpy(g_status, "ATA read VERTEXES failed", 79); return -1;
    }
    g_n_verts = (int)(lvertexes->size / sizeof(d_vertex_t));
    g_verts = (d_vertex_t*)vraw;       /* keep buffer */

    /* LINEDEFS */
    uint8_t* lraw = NULL;
    if (load_lump_from_disk(llinedefs->off, llinedefs->size, &lraw) != 0) {
        kfree(g_verts); g_verts = NULL;
        strncpy(g_status, "ATA read LINEDEFS failed", 79); return -1;
    }
    g_n_lines = (int)(llinedefs->size / sizeof(d_linedef_t));
    g_lines = (d_linedef_t*)lraw;

    /* THINGS — find player 1 start (type 1) */
    uint8_t* traw = NULL;
    if (load_lump_from_disk(lthings->off, lthings->size, &traw) != 0) {
        kfree(g_verts); kfree(g_lines); g_verts = NULL; g_lines = NULL;
        strncpy(g_status, "ATA read THINGS failed", 79); return -1;
    }
    int n_things = (int)(lthings->size / sizeof(d_thing_t));
    d_thing_t* things = (d_thing_t*)traw;
    bool found_start = false;
    for (int i = 0; i < n_things; i++) {
        if (things[i].type == 1) {
            g_player_start_x = things[i].x;
            g_player_start_y = things[i].y;
            g_player_start_ang = things[i].angle;
            found_start = true;
            break;
        }
    }
    kfree(traw);
    if (!found_start) {
        g_player_start_x = 0; g_player_start_y = 0; g_player_start_ang = 0;
    }

    init_trig();
    g_level_loaded = true;
    strncpy(g_status, "E1M1 loaded", 79);
    return 0;
}

/* ---- player + game state ---- */
typedef struct {
    int x, y;          /* DOOM units */
    int z;             /* unused */
    int ang;           /* 0..ANG_RES-1 */
    bool keys[16];     /* W,S,A,D,Left,Right etc */
} doom_player_t;

enum { K_W = 0, K_S, K_A, K_D, K_TL, K_TR, K_RUN, K_USE };

static doom_player_t g_pl;
static uint32_t g_last_tick_ms = 0;

static int doom_deg_to_ang(int deg) {
    /* DOOM angle: 0=east, 90=north, increasing CCW.
       Our angle: 0..ANG_RES, 0=+X, increasing CCW.
       So: ang = deg / 360 * ANG_RES */
    int a = (deg * ANG_RES) / 360;
    return ((a % ANG_RES) + ANG_RES) % ANG_RES;
}

static void doom_reset_player(void) {
    g_pl.x = g_player_start_x;
    g_pl.y = g_player_start_y;
    g_pl.ang = doom_deg_to_ang(g_player_start_ang);
    for (int i = 0; i < 16; i++) g_pl.keys[i] = false;
    g_last_tick_ms = pit_uptime_ms();
}

/* Distance from point to segment (squared root) */
static int dist_point_seg(int px, int py, int ax, int ay, int bx, int by) {
    long long abx = bx - ax;
    long long aby = by - ay;
    long long apx = px - ax;
    long long apy = py - ay;
    long long ab_len_sq = abx*abx + aby*aby;
    long long t_num = apx*abx + apy*aby;
    int t_q;
    if (ab_len_sq == 0) t_q = 0;
    else {
        long long tq = (t_num * FIX_ONE) / ab_len_sq;
        if (tq < 0) tq = 0;
        if (tq > FIX_ONE) tq = FIX_ONE;
        t_q = (int)tq;
    }
    long long cx = ax + (abx * t_q) / FIX_ONE;
    long long cy = ay + (aby * t_q) / FIX_ONE;
    long long ddx = px - cx;
    long long ddy = py - cy;
    return isqrt_ll(ddx*ddx + ddy*ddy);
}

#define PLAYER_RADIUS 16

static bool collides(int nx, int ny) {
    if (!g_lines || !g_verts) return false;
    for (int i = 0; i < g_n_lines; i++) {
        d_linedef_t* l = &g_lines[i];
        if (l->v1 >= (uint16_t)g_n_verts || l->v2 >= (uint16_t)g_n_verts) continue;
        /* allow walking through two-sided lines (open passages) */
        if (l->flags & LINE_TWOSIDED) continue;
        d_vertex_t* a = &g_verts[l->v1];
        d_vertex_t* b = &g_verts[l->v2];
        int d = dist_point_seg(nx, ny, a->x, a->y, b->x, b->y);
        if (d < PLAYER_RADIUS) return true;
    }
    return false;
}

static void doom_step(uint32_t dt_ms) {
    int speed_units_per_sec = g_pl.keys[K_RUN] ? 400 : 200;
    int turn_per_sec = ANG_RES / 3;        /* ~120° per second */

    int move = (speed_units_per_sec * (int)dt_ms) / 1000;
    int turn = (turn_per_sec * (int)dt_ms) / 1000;
    if (move < 1 && (g_pl.keys[K_W]||g_pl.keys[K_S]||g_pl.keys[K_A]||g_pl.keys[K_D])) move = 1;
    if (turn < 1 && (g_pl.keys[K_TL]||g_pl.keys[K_TR])) turn = 1;

    if (g_pl.keys[K_TL]) g_pl.ang = (g_pl.ang + turn) & (ANG_RES - 1);
    if (g_pl.keys[K_TR]) g_pl.ang = (g_pl.ang - turn + ANG_RES) & (ANG_RES - 1);

    int fdx = icos(g_pl.ang);
    int fdy = isin(g_pl.ang);
    int sdx = icos((g_pl.ang - ANG_RES/4) & (ANG_RES - 1));
    int sdy = isin((g_pl.ang - ANG_RES/4) & (ANG_RES - 1));

    int dx = 0, dy = 0;
    if (g_pl.keys[K_W]) { dx += fdx; dy += fdy; }
    if (g_pl.keys[K_S]) { dx -= fdx; dy -= fdy; }
    if (g_pl.keys[K_D]) { dx += sdx; dy += sdy; }
    if (g_pl.keys[K_A]) { dx -= sdx; dy -= sdy; }

    if (dx || dy) {
        /* normalize to `move` units */
        long long len_q = isqrt_ll((long long)dx*dx + (long long)dy*dy);
        if (len_q > 0) {
            int mx = (int)(((long long)dx * move) / len_q);
            int my = (int)(((long long)dy * move) / len_q);
            int nx = g_pl.x + mx;
            int ny = g_pl.y + my;
            /* try full move, else axis-aligned slide */
            if (!collides(nx, ny)) { g_pl.x = nx; g_pl.y = ny; }
            else if (!collides(nx, g_pl.y)) g_pl.x = nx;
            else if (!collides(g_pl.x, ny)) g_pl.y = ny;
        }
    }
}

/* ---- 3D renderer ---- */

#define FOV_DEG 70
#define WALL_HEIGHT_UNITS 96     /* heuristic for screen scaling */

static void render_3d(int cx, int cy, int cw, int ch) {
    /* sky */
    gfx_rect_fill(cx, cy,           cw, ch / 2, RGB(0x35, 0x40, 0x55));
    /* floor */
    gfx_rect_fill(cx, cy + ch / 2,  cw, ch - ch / 2, RGB(0x32, 0x22, 0x16));

    if (!g_lines || !g_verts) return;

    int half_fov_ang = (FOV_DEG * ANG_RES) / 720;   /* (FOV/2) in our angle units */
    int cw2 = cw / 2;

    /* per-column raycast */
    for (int col = 0; col < cw; col++) {
        int ray_off  = ((col - cw2) * 2 * half_fov_ang) / cw;     /* relative angle */
        int ray_ang  = (g_pl.ang + ray_off) & (ANG_RES - 1);
        int dx = icos(ray_ang);
        int dy = isin(ray_ang);

        long long best_t = (long long)1 << 30;
        int       hit_line = -1;

        for (int i = 0; i < g_n_lines; i++) {
            d_linedef_t* l = &g_lines[i];
            if (l->v1 >= (uint16_t)g_n_verts || l->v2 >= (uint16_t)g_n_verts) continue;
            d_vertex_t* a = &g_verts[l->v1];
            d_vertex_t* b = &g_verts[l->v2];
            long long ex = b->x - a->x;
            long long ey = b->y - a->y;
            long long gx = a->x - g_pl.x;
            long long gy = a->y - g_pl.y;

            long long denom = ex * dy - ey * dx;            /* DU * Q */
            if (denom == 0) continue;
            long long s_num = (long long)dx * gy - (long long)dy * gx;  /* DU * Q */
            long long s_q = (s_num * FIX_ONE) / denom;
            if (s_q < 0 || s_q > FIX_ONE) continue;
            long long t_num = ex * gy - ey * gx;            /* DU^2 */
            long long t_du = (t_num * FIX_ONE) / denom;     /* DU */
            if (t_du <= 0) continue;
            if (t_du < best_t) { best_t = t_du; hit_line = i; }
        }

        if (hit_line < 0) continue;

        /* fish-eye: perpendicular distance = t * cos(ray_off) */
        long long perp = (best_t * icos(ray_off)) / FIX_ONE;
        if (perp < 1) perp = 1;

        /* wall column height: focal_len * wall_height_units / perp
           pick focal_len so a 96-unit wall at distance 256 is ~half screen */
        long long focal = (cw / 2) * FIX_ONE / icos(half_fov_ang);
        long long wh = (focal * WALL_HEIGHT_UNITS) / (perp * FIX_ONE / FIX_ONE);
        /* simplified: wh = (cw/2 * WALL_HEIGHT_UNITS) / perp  (focal ~ cw/2 for 60° fov) */
        wh = ((long long)(cw / 2) * WALL_HEIGHT_UNITS) / perp;
        if (wh > ch) wh = ch;
        if (wh < 1)  wh = 1;

        int top    = cy + (ch - (int)wh) / 2;
        int bottom = top + (int)wh;
        if (top < cy) top = cy;
        if (bottom > cy + ch) bottom = cy + ch;

        /* shading by distance + line orientation */
        int dist8 = (int)(perp / 4);
        if (dist8 > 200) dist8 = 200;
        int bright = 230 - dist8;
        if (bright < 30) bright = 30;

        /* color tint by line index for a bit of variety */
        d_linedef_t* l = &g_lines[hit_line];
        d_vertex_t* a = &g_verts[l->v1];
        d_vertex_t* b = &g_verts[l->v2];
        int dxw = b->x - a->x;
        int dyw = b->y - a->y;
        bool vertical_wall = (dxw*dxw < dyw*dyw);
        int r = bright;
        int g = vertical_wall ? bright * 8 / 10 : bright;
        int b8 = vertical_wall ? bright * 6 / 10 : bright * 8 / 10;

        gfx_rect_fill(cx + col, top, 1, bottom - top, RGB(r, g, b8));
    }
}

/* ---- WM glue: paint + key for the play window ---- */

static void play_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);

    uint32_t now = pit_uptime_ms();
    uint32_t dt = now - g_last_tick_ms;
    if (dt > 100) dt = 100;
    g_last_tick_ms = now;

    doom_step(dt);

    render_3d(cx, cy, cw, ch);

    /* HUD */
    char buf[64]; int p = 0; char tmp[16];
    const char* l1 = " E1M1  pos=(";
    for (int i = 0; l1[i]; i++) buf[p++] = l1[i];
    itoa(g_pl.x, tmp, 10); for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ',';
    itoa(g_pl.y, tmp, 10); for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ')'; buf[p++] = ' ';
    const char* l2 = "ang=";
    for (int i = 0; l2[i]; i++) buf[p++] = l2[i];
    itoa((g_pl.ang * 360) / ANG_RES, tmp, 10);
    for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p] = 0;
    /* black strip */
    gfx_rect_fill(cx, cy, cw, 18, RGB(0x10,0x10,0x18));
    gfx_string(cx + 4, cy + 1, buf, RGB(0xE0,0xE0,0xC0), RGB(0x10,0x10,0x18), true);

    gfx_rect_fill(cx, cy + ch - 18, cw, 18, RGB(0x10,0x10,0x18));
    gfx_string(cx + 4, cy + ch - 17, "WASD move  Arrows turn  Shift run  Esc quit",
               RGB(0xC0,0xC0,0xC0), RGB(0x10,0x10,0x18), true);
}

static int key_to_idx(char c, bool* down) {
    /* We only get key-down events from kbd buffer (no key-up).
       Use timer-based release: clear all on each frame? No, then movement would only
       work at key-press moment.  Instead treat each key event as setting down=true and
       set release in the loop below. */
    *down = true;
    if (c == 'w' || c == 'W') return K_W;
    if (c == 's' || c == 'S') return K_S;
    if (c == 'a' || c == 'A') return K_A;
    if (c == 'd' || c == 'D') return K_D;
    if (c == (char)K_LEFT)    return K_TL;
    if (c == (char)K_RIGHT)   return K_TR;
    if (c == (char)K_UP)      return K_W;
    if (c == (char)K_DOWN)    return K_S;
    return -1;
}

/* Because PS/2 layer doesn't deliver key-up, use auto-release timeout per key. */
static uint32_t g_key_pressed_at[16];

static void play_key(window_t* w, char c) {
    if (c == 0x1B) { wm_close(w); return; }
    bool down;
    int k = key_to_idx(c, &down);
    if (k < 0) return;
    g_pl.keys[k] = true;
    g_key_pressed_at[k] = pit_uptime_ms();
}

static void play_paint_with_key_decay(window_t* w) {
    /* auto-release any key that hasn't been pressed in last 200 ms */
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < 16; i++) {
        if (g_pl.keys[i] && (now - g_key_pressed_at[i] > 200)) g_pl.keys[i] = false;
    }
    play_paint(w);
}

int doom_play_e1m1(void) {
    if (!g_loaded) {
        int r = doom_load_from_disk();
        if (r != 0) return r;
    }
    if (load_level_e1m1() != 0) return -2;
    doom_reset_player();
    for (int i = 0; i < 16; i++) g_key_pressed_at[i] = 0;
    /* Native 320x200 render area; window adds title bar */
    wm_open_app(40, 40, 640, 420, "DOOM E1M1", play_paint_with_key_decay, play_key, NULL);
    return 0;
}
