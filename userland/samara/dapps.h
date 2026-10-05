#ifndef DAPPS_H
#define DAPPS_H
/* edit, sysmon, pkg, settings: one binary, picked by argv[0] (busybox style). window stuff is fm_be.h */
#include <stdint.h>
#include "fm_be.h"

extern uint32_t c_bg, c_bg2, c_bar, c_well, c_ink, c_dim, c_faint, c_rule, c_hover, c_acc, c_selbg, c_danger, c_ok;
extern int fh_reg, fh_small, fh_mono;

typedef struct { char buf[512]; int len, pos, all; } LineEd;

void th_load(void);
uint32_t now_ms(void);
int cp_utf8(int c, char *o);
int u8_len(const char *s);                       /* bytes in the char at s */
void fit(const char *s, int w, int font, char *out, int cap);
void frame(int x, int y, int w, int h, uint32_t c);
void txt(int x, int y, int h, const char *s, uint32_t c, int font);
int  button(int x, int y, int w, const char *label, int primary, int mx, int my);   /* draws, 1 if the pointer is on it */
void le_set(LineEd *e, const char *s);
int  le_key(LineEd *e, FmEv *k);                 /* 1 enter, 2 esc */
void le_draw(LineEd *e, int x, int y, int w, int h, int focus);
int  le_click(LineEd *e, int x, int px);
void clip_text(const char *s, int n);
int  ask_file(const char *title, char *path, int save);   /* modal, 1 = picked */
int  ask_yn(const char *title, const char *q);            /* 1 yes */
void run_bg(const char *cmd);

int edit_main(int argc, char **argv);
int sysmon_main(int argc, char **argv);
int pkg_main(int argc, char **argv);
int settings_main(int argc, char **argv);

#endif
int ui_open(int w, int h, const char *title);   /* be_open + font heights */
