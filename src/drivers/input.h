#ifndef SAMARA_INPUT_H
#define SAMARA_INPUT_H
#include "core/types.h"

/* /dev/input: raw keyboard + mouse events in Linux `struct input_event`
   layout (16 bytes: sec, usec, u16 type, u16 code, s32 value), with Linux
   evdev key codes. While the device is open the keyboard and mouse are
   "grabbed": the shell and window manager stop seeing them. Ctrl+Alt+Q
   drops the grab from the kernel side (escape hatch for a stuck program). */

#define IEV_KEY 1
#define IEV_REL 2
#define IEV_REL_X 0
#define IEV_REL_Y 1
#define IEV_REL_WHEEL 8

void input_open(void);
void input_close(void);
bool input_grabbed(void);
void input_release_grab(void);               /* forced, from the ISR */
void input_push(uint16_t type, uint16_t code, int32_t value);   /* ISR-safe */
uint16_t input_linux_key(uint8_t sc, bool ext);
bool input_pending(void);
int  input_read(char* buf, uint32_t n, bool nonblock);   /* bytes or -errno */

#endif
