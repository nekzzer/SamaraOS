#ifndef SAMARA_KEYBOARD_H
#define SAMARA_KEYBOARD_H
#include "core/types.h"

#define KEY_BUF_SIZE 256

/* special key codes returned through the buffer (above ASCII range) */
#define K_UP    0x81
#define K_DOWN  0x82
#define K_LEFT  0x83
#define K_RIGHT 0x84
#define K_HOME  0x85
#define K_END   0x86
#define K_DEL   0x87
#define K_PGUP  0x88
#define K_PGDN  0x89
#define K_F1    0x90   /* F1..F10 = K_F1 .. K_F1+9 */

void kbd_init(void);
int  kbd_has_key(void);
char kbd_getc(void);          /* blocking */
char kbd_trygetc(void);       /* non-blocking, 0 if empty */
bool kbd_is_ru(void);         /* current layout: true = RU (ЙЦУКЕН), false = EN */
void kbd_set_ru(bool ru);
/* Monotonically increases on every RU/EN toggle — callers (the WM) sample
   this each frame to know they should redraw the layout indicator. */
uint32_t kbd_layout_epoch(void);
void kbd_ignore_scancode(uint8_t sc);   /* drop a (broken) key's scancode */
void kbd_unignore_all(void);

/* Currently held keys, one bit per Linux key code (KEY_A = 30, ...). */
void kbd_key_bits(uint8_t out[32]);

#endif
