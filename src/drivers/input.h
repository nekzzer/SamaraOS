#ifndef SAMARA_INPUT_H
#define SAMARA_INPUT_H
#include "core/types.h"

/* /dev/input: raw keyboard + mouse events in Linux `struct input_event`
   layout (16 bytes: sec, usec, u16 type, u16 code, s32 value), with Linux
   evdev key codes. While the device is open the keyboard and mouse are
   "grabbed": the shell and window manager stop seeing them. Ctrl+Alt+Backspace
   drops the grab from the kernel side (escape hatch for a stuck program). */

#define IEV_SYN 0
#define IEV_KEY 1
#define IEV_REL 2
#define IEV_ABS 3
#define IEV_REL_X 0
#define IEV_REL_Y 1
#define IEV_REL_WHEEL 8

/* three readers: 0 = /dev/input (everything, no EV_SYN, yutani counts on
   that), 1 = /dev/input-kbd and 2 = /dev/input-mouse, evdev style with
   EV_SYN and the EVIOC ioctls for xorg's evdev driver */
#define INPUT_ALL 0
#define INPUT_KBD 1
#define INPUT_MOUSE 2

/* devices: one /dev/input/eventN each. ring index for open() is 3 + id */
#define IN_DEVS 12
typedef struct {
    char name[48];
    uint16_t bus, vid, pid, ver;
    uint32_t ev, rel, abs;           /* EV_* types, REL_* and ABS_* (0..31) as bit masks */
    uint8_t key[96];                 /* key and button bitmap */
    int absmin[8], absmax[8];
} input_dev_t;
int  input_register(const input_dev_t* d);
void input_unregister(int id);
void input_report(int id, uint16_t type, uint16_t code, int32_t val);
void input_repeat(uint16_t code);
void input_init(void);

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
