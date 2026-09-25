/* Minimal samara.h demo: a window with text, shapes and a mouse-following
   dot. Build inside SamaraOS:  tcc /usr/src/samara/hello.c -o hello && ./hello */
#include <stdio.h>
#include <samara.h>

int main(void) {
    SmWin *w = sm_open(360, 220, 1, "Привет, samara.h");
    if (!w) { fprintf(stderr, "hello: no desktop (run `desktop` first)\n"); return 1; }
    int mx = -1, my = -1, clicks = 0, done = 0;
    uint32_t t0 = sm_ticks();
    while (!done) {
        SmEvent e;
        while (sm_event(w, &e, 0) > 0) {
            if (e.type == SM_EV_CLOSE || (e.type == SM_EV_KEY && e.a == SM_CH_ESC)) done = 1;
            if (e.type == SM_EV_MOUSE_DOWN) clicks++;
        }
        sm_mouse(w, &mx, &my, 0);

        uint32_t t = sm_ticks() - t0;
        sm_clear(w, SM_PAPER);
        sm_text(w, 16, 12, "Hello from C!  Привет из Си!", SM_DARK, SM_FONT_BIG);
        char buf[64];
        snprintf(buf, sizeof buf, "t = %u ms   clicks = %d   mouse = %d,%d", t, clicks, mx, my);
        sm_text(w, 16, 44, buf, SM_GRAY, SM_FONT_SMALL);
        sm_rect(w, 16, 80, 80, 50, SM_ORANGE);
        sm_frame(w, 110, 80, 80, 50, SM_BLUE);
        sm_disc(w, 240, 105, 25, SM_GREEN);
        sm_circle(w, 310, 105, 25, SM_RED);
        int x = 16 + (int)(t / 8 % 328);
        sm_line(w, 16, 150, x, 200, SM_MAGENTA);
        sm_text(w, 16, 200, "Esc / X closes", SM_DARK, SM_FONT_MONO);
        if (mx >= 0 && my >= 0) sm_disc(w, mx, my, 4, SM_RED);
        if (sm_present(w) < 0) break;
        sm_sleep_ms(16);
    }
    sm_close(w);
    return 0;
}
