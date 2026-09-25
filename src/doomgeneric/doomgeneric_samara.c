/* DOOM platform layer for SamaraOS:
   - DG_Init/DG_DrawFrame/DG_GetTicksMs/DG_SleepMs/DG_GetKey/DG_SetWindowTitle
   - samara_doom_launch() opens a WM window, calls doomgeneric_Create, then
     drives doomgeneric_Tick() from on_paint. */

#include "../core/types.h"
#include "../core/io.h"
#include "../core/heap.h"
#include "../core/string.h"
#include "../gfx/gfx.h"
#include "../gui/wm.h"
#include "../drivers/keyboard.h"
#include "../boot/pit.h"
#include "../drivers/ata.h"

#include "doomgeneric_samara.h"
#include "../../doomgeneric-master/doomgeneric/doomkeys.h"
#include "../../doomgeneric-master/doomgeneric/doomgeneric.h"

extern int  samara_wad_load(void);
extern int  printf(const char* fmt, ...);

/* ---------- key event ring ---------- */

typedef struct { int pressed; unsigned char key; } dkev_t;

#define EV_CAP 64
static dkev_t  evbuf[EV_CAP];
static int     ev_head = 0, ev_tail = 0;

/* per-key auto-release: last time we saw the key, ms */
#define KEY_AR_TIMEOUT_MS 250
static uint32_t last_seen_ms[256];
static bool     key_down[256];

static void ev_push(int pressed, unsigned char k) {
    int next = (ev_head + 1) % EV_CAP;
    if (next == ev_tail) return;        /* drop on overflow */
    evbuf[ev_head].pressed = pressed;
    evbuf[ev_head].key     = k;
    ev_head = next;
}

static int ev_pop(int* pressed, unsigned char* k) {
    if (ev_head == ev_tail) return 0;
    *pressed = evbuf[ev_tail].pressed;
    *k       = evbuf[ev_tail].key;
    ev_tail = (ev_tail + 1) % EV_CAP;
    return 1;
}

static unsigned char map_samara_to_doom(char c) {
    unsigned char k = (unsigned char)c;
    if (k == 0x1B) return KEY_ESCAPE;
    if (k == '\n' || k == '\r') return KEY_ENTER;
    if (k == '\t') return KEY_TAB;
    if (k == 0x08 || k == 0x7F) return KEY_BACKSPACE;
    if (k == K_LEFT)  return KEY_LEFTARROW;
    if (k == K_RIGHT) return KEY_RIGHTARROW;
    if (k == K_UP)    return KEY_UPARROW;
    if (k == K_DOWN)  return KEY_DOWNARROW;
    if (k == K_HOME)  return KEY_HOME;
    if (k == K_END)   return KEY_END;
    if (k == K_DEL)   return KEY_DEL;
    if (k == K_PGUP)  return KEY_PGUP;
    if (k == K_PGDN)  return KEY_PGDN;
    if (k >= K_F1 && k < K_F1 + 10) return (unsigned char)(KEY_F1 + (k - K_F1));
    /* lowercase letters/numbers/etc pass through */
    if (k >= 'A' && k <= 'Z') k += 32;
    return k;
}

/* read pending keys from samara kbd buffer, post pressed events,
   and synthesize pressed=0 for keys idle longer than KEY_AR_TIMEOUT_MS. */
static void poll_keys(void) {
    /* drain kbd buffer */
    char c;
    while ((c = kbd_trygetc()) != 0) {
        unsigned char k = map_samara_to_doom(c);
        if (k == 0) continue;
        uint32_t now = pit_uptime_ms();
        last_seen_ms[k] = now;
        /* always queue a press for this key (rapid taps stay distinct) */
        if (!key_down[k]) {
            ev_push(1, k);
            key_down[k] = true;
        } else {
            /* duplicate press to keep DOOM's autorepeat alive */
            ev_push(1, k);
        }
        /* fire/use synthesized down for shift while moving — handled by DOOM's m_controls */
    }
    /* auto-release */
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < 256; i++) {
        if (key_down[i] && now - last_seen_ms[i] > KEY_AR_TIMEOUT_MS) {
            key_down[i] = false;
            ev_push(0, (unsigned char)i);
        }
    }
}

/* ---------- doomgeneric callbacks ---------- */

static window_t* g_doom_win = 0;
static char      g_status[80] = "doom: idle";

const char* samara_doom_status(void) { return g_status; }

void DG_Init(void) {
    /* DG_ScreenBuffer was malloc'd in doomgeneric_Create */
}

void DG_SleepMs(uint32_t ms) {
    uint32_t target = pit_uptime_ms() + ms;
    while (pit_uptime_ms() < target) { __asm__ volatile (""); }
}

uint32_t DG_GetTicksMs(void) {
    return pit_uptime_ms();
}

int DG_GetKey(int* pressed, unsigned char* key) {
    poll_keys();
    return ev_pop(pressed, key);
}

void DG_SetWindowTitle(const char* title) {
    if (!g_doom_win || !title) return;
    int i = 0;
    while (title[i] && i < 63) { g_doom_win->title[i] = title[i]; i++; }
    g_doom_win->title[i] = 0;
}

extern uint8_t* I_VideoBuffer;       /* DOOM 320x200 8-bit indexed */
extern uint32_t* DG_ScreenBuffer;    /* 640x400 32-bpp BGRX, cmap_to_fb output */
extern const uint8_t* samara_wad_find_lump(const char* name, uint32_t* out_size);

#define DOOM_FB_W 640
#define DOOM_FB_H 400

void DG_DrawFrame(void) {
    if (!g_doom_win || !DG_ScreenBuffer) return;
    int cx, cy, cw, ch;
    wm_client_rect(g_doom_win, &cx, &cy, &cw, &ch);

    int dx0 = cx + (cw > DOOM_FB_W ? (cw - DOOM_FB_W) / 2 : 0);
    int dy0 = cy + (ch > DOOM_FB_H ? (ch - DOOM_FB_H) / 2 : 0);

    /* cmap_to_fb wrote rgba8888 in the layout R=offset16 G=8 B=0 — same as our
       RGB() macro, so a row-wise memcpy via gfx_blit_argb is correct. */
    gfx_blit_argb(dx0, dy0, DOOM_FB_W, DOOM_FB_H, DG_ScreenBuffer);
}

/* ---------- WM window glue ---------- */

extern void doomgeneric_Create(int argc, char** argv);
extern void doomgeneric_Tick(void);

static int  g_doom_initialized = 0;
static int  g_doom_init_failed = 0;

/* argv stays alive: pointers must remain valid for DOOM's whole lifetime. */
static char  argv0_buf[16] = "samara-doom";
static char  argv1_buf[8]  = "-iwad";
static char  argv2_buf[16] = "doom1.wad";
static char  argv3_buf[8]  = "-warp";
static char  argv4_buf[4]  = "1";
static char  argv5_buf[4]  = "1";
static char* g_argv[8];

static void doom_paint(window_t* w) {
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);

    if (g_doom_init_failed) {
        gfx_rect_fill(cx, cy, cw, ch, RGB(0x10,0x10,0x10));
        gfx_string(cx + 8, cy + 8,  "DOOM init failed.", RGB(0xE0,0x60,0x60), 0, false);
        gfx_string(cx + 8, cy + 28, samara_doom_status(),  RGB(0xC0,0xC0,0xC0), 0, false);
        return;
    }

    if (!g_doom_initialized) {
        gfx_rect_fill(cx, cy, cw, ch, RGB(0x08,0x08,0x14));
        gfx_string(cx + 8, cy + 8,  "Loading DOOM, please wait...",
                   RGB(0xE0,0xE0,0xC0), 0, false);
        gfx_string(cx + 8, cy + 28, "Reading WAD from ATA disk...",
                   RGB(0xC0,0xC0,0xC0), 0, false);
        /* one-shot init */
        g_argv[0] = argv0_buf;
        g_argv[1] = argv1_buf;
        g_argv[2] = argv2_buf;
        g_argv[3] = 0;
        doomgeneric_Create(3, g_argv);   /* runs until first frame is drawn */
        g_doom_initialized = 1;
        return;
    }

    doomgeneric_Tick();
}

static void doom_key(window_t* w, char c) {
    (void)w;
    if (c == 0) return;
    /* feed synthesized pressed event; DG_GetKey will translate */
    unsigned char k = map_samara_to_doom(c);
    if (k == 0) return;
    last_seen_ms[k] = pit_uptime_ms();
    if (!key_down[k]) {
        ev_push(1, k);
        key_down[k] = true;
    } else {
        ev_push(1, k);
    }
}

int samara_doom_launch(void) {
    if (!gfx_ready()) {
        const char* m = "doom: enter desktop first";
        for (int i = 0; m[i]; i++) g_status[i] = m[i];
        g_status[24] = 0;
        return -1;
    }

    /* Load WAD into RAM and register with libc shim. */
    int r = samara_wad_load();
    if (r != 0) {
        const char* m = "doom: WAD load failed";
        for (int i = 0; m[i]; i++) g_status[i] = m[i];
        g_status[21] = 0;
        return -2;
    }

    g_doom_init_failed = 0;
    g_doom_initialized = 0;
    g_doom_win = wm_open_app(20, 30, DOOMGENERIC_RESX + 4, DOOMGENERIC_RESY + 28,
                             "DOOM", doom_paint, doom_key, 0);
    if (!g_doom_win) {
        const char* m = "doom: no WM slot";
        for (int i = 0; m[i]; i++) g_status[i] = m[i];
        g_status[16] = 0;
        return -3;
    }

    const char* m = "doom: window opened, init pending on first paint";
    int i; for (i = 0; m[i] && i < 79; i++) g_status[i] = m[i];
    g_status[i] = 0;
    return 0;
}

/* called from libc when a fatal error happens */
void doomgen_panic(const char* msg) {
    g_doom_init_failed = 1;
    const char* m = msg ? msg : "panic";
    int i; for (i = 0; m[i] && i < 79; i++) g_status[i] = m[i];
    g_status[i] = 0;
    /* spin so WM keeps repainting and shows the message */
    while (1) { __asm__ volatile ("hlt"); }
}
