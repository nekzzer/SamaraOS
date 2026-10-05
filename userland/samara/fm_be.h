#ifndef FM_BE_H
#define FM_BE_H
/* drawing, window and input for fm. one backend per display server:
   fm_samara.c now, a wl_shm one later */
#include <stdint.h>

enum { EV_NONE, EV_KEY, EV_DOWN, EV_UP, EV_MOVE, EV_WHEEL, EV_RESIZE, EV_CLOSE };

/* EV_KEY: a = unicode codepoint or FK_*, b = mods
   EV_DOWN/UP/MOVE: a, b = x, y; c = button (1 left, 2 right)
   EV_WHEEL: a = notches, > 0 down
   EV_RESIZE: a, b = new size */
enum { FK_UP = 0x200000, FK_DOWN, FK_LEFT, FK_RIGHT, FK_HOME, FK_END, FK_DEL, FK_PGUP, FK_PGDN,
       FK_ENTER, FK_BACK, FK_ESC, FK_TAB, FK_F2, FK_F5 };
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4

typedef struct { int type, a, b, c, mods; } FmEv;

enum { F_REG, F_BOLD, F_SMALL, F_BIG, F_MONO };

int  be_open(int w, int h, const char *title);
void be_close(void);
int  be_wait(FmEv *e, int ms);                 /* 1 event, 0 timeout, -1 interrupted */
int  be_w(void);
int  be_h(void);
void be_title(const char *t);
void be_fill(int x, int y, int w, int h, uint32_t c);
void be_line(int x0, int y0, int x1, int y1, uint32_t c);
void be_text(int x, int y, const char *s, uint32_t c, int font);
int  be_text_w(const char *s, int font);
int  be_font_h(int font);
void be_blit(int x, int y, int w, int h, const uint32_t *src, int sw, int sh);   /* scaled to w x h */
void be_clip_set(const char *s, int n);        /* system clipboard, utf-8 */
int  be_flip(void);                            /* < 0: window gone */

#endif
