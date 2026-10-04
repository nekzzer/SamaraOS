#ifndef SAMARA_INPUT_H
#define SAMARA_INPUT_H
#include "core/types.h"

/* /dev/input: raw keyboard + mouse events in Linux `struct input_event`
   layout (16 bytes: sec, usec, u16 type, u16 code, s32 value), with Linux
   evdev key codes. While the device is open the keyboard and mouse are
   "grabbed": the shell and window manager stop seeing them. Ctrl+Alt+Backspace
   drops the grab from the kernel side (escape hatch for a stuck program). */

#define IEV_KEY 1
#define IEV_REL 2
#define IEV_REL_X 0
#define IEV_REL_Y 1
#define IEV_REL_WHEEL 8

/* three readers: 0 = /dev/input (everything, no EV_SYN, yutani counts on
   that), 1 = /dev/input-kbd and 2 = /dev/input-mouse, evdev style with
   EV_SYN and the EVIOC ioctls for xorg's evdev driver */
#define INPUT_ALL 0
#define INPUT_KBD 1
#define INPUT_MOUSE 2

void input_open(int d);
void input_close(int d);
bool input_grabbed(void);
void input_release_grab(void);               /* forced, from the ISR */
void input_push(uint16_t type, uint16_t code, int32_t value);   /* ISR-safe */
void input_sync(int d);                      /* EV_SYN on device d */
uint16_t input_linux_key(uint8_t sc, bool ext);
bool input_pending(int d);
int  input_read(int d, char* buf, uint32_t n, bool nonblock);   /* bytes or -errno */
int  input_ioctl(int d, uint32_t req, uint8_t* arg);             /* EVIOC*, arg already checked */

#endif
