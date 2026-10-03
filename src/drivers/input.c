#include "drivers/input.h"
#include "boot/pit.h"
#include "core/task.h"
#include "proc/proc.h"

#define IN_CAP 1024

typedef struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } input_event_t;

static struct { input_event_t ev[IN_CAP]; volatile int head, tail, users; } r[3];
static volatile bool grab;

void input_open(int d) {
    if (r[d].users++ == 0) r[d].head = r[d].tail = 0;
    grab = true;
}

void input_close(int d) {
    if (r[d].users > 0 && --r[d].users == 0) r[d].head = r[d].tail = 0;
    if (!r[0].users && !r[1].users && !r[2].users) grab = false;
}

bool input_grabbed(void) { return grab; }
void input_release_grab(void) { grab = false; }

static void put(int d, uint16_t type, uint16_t code, int32_t value) {
    if (!r[d].users) return;
    int n = (r[d].head + 1) % IN_CAP;
    if (n == r[d].tail) return;                  /* full: drop */
    uint32_t ms = pit_uptime_ms();
    input_event_t* e = &r[d].ev[r[d].head];
    e->sec = ms / 1000;
    e->usec = (ms % 1000) * 1000;
    e->type = type;
    e->code = code;
    e->value = value;
    r[d].head = n;
}

void input_push(uint16_t type, uint16_t code, int32_t value) {
    put(0, type, code, value);
    put(type == 1 && code < 0x100 ? 1 : 2, type, code, value);   /* buttons (0x110..) go with the mouse */
}

void input_sync(int d) { put(d, 0, 0, 0); }

/* PS/2 set-1 scancode -> Linux KEY_*. Plain codes are identical; the E0
   prefixed ones (arrows, nav cluster, right ctrl/alt) get their own. */
uint16_t input_linux_key(uint8_t sc, bool ext) {
    if (!ext) return sc;
    switch (sc) {
        case 0x48: return 103;  /* up */
        case 0x50: return 108;  /* down */
        case 0x4B: return 105;  /* left */
        case 0x4D: return 106;  /* right */
        case 0x47: return 102;  /* home */
        case 0x4F: return 107;  /* end */
        case 0x49: return 104;  /* pgup */
        case 0x51: return 109;  /* pgdn */
        case 0x52: return 110;  /* insert */
        case 0x53: return 111;  /* delete */
        case 0x1D: return 97;   /* right ctrl */
        case 0x38: return 100;  /* right alt */
        case 0x1C: return 96;   /* keypad enter */
        case 0x35: return 98;   /* keypad slash */
    }
    return 0;
}

bool input_pending(int d) { return r[d].head != r[d].tail; }

int input_read(int d, char* buf, uint32_t n, bool nonblock) {
    if (n < sizeof(input_event_t)) return -22;   /* EINVAL */
    while (r[d].head == r[d].tail) {
        if (nonblock) return -11;                /* EAGAIN */
        if (!grab && r[d].users == 0) return 0;
        if (proc_interrupted()) return -4;       /* EINTR */
        task_yield();
    }
    uint32_t got = 0;
    while (got + sizeof(input_event_t) <= n && r[d].head != r[d].tail) {
        *(input_event_t*)(buf + got) = r[d].ev[r[d].tail];
        r[d].tail = (r[d].tail + 1) % IN_CAP;
        got += sizeof(input_event_t);
    }
    return (int)got;
}

static void setbit(uint8_t* b, uint32_t len, int i) { if ((uint32_t)i / 8 < len) b[i / 8] |= (uint8_t)(1 << (i & 7)); }

/* what libevdev asks before xorg's evdev takes a device. req is the whole
   _IOC: size in bits 16..29, 'E' = 0x45 in 8..15, nr in 0..7 */
int input_ioctl(int d, uint32_t req, uint8_t* arg) {
    uint32_t nr = req & 0xFF, len = (req >> 16) & 0x3FFF;
    if (((req >> 8) & 0xFF) != 0x45) return -25;  /* ENOTTY */
    bool kbd = d != INPUT_MOUSE, mouse = d != INPUT_KBD;
    switch (nr) {
        case 0x01: *(int*)arg = 0x010001; return 0;                      /* EVIOCGVERSION */
        case 0x02: {                                                     /* EVIOCGID */
            uint16_t* id = (uint16_t*)arg;
            id[0] = 0x11; id[1] = 1; id[2] = kbd ? 1 : 2; id[3] = 0xab41;   /* BUS_I8042 */
            return 0;
        }
        case 0x03: ((int*)arg)[0] = 250; ((int*)arg)[1] = 33; return 0;   /* EVIOCGREP */
        case 0x06: case 0x07: case 0x08: {                               /* NAME, PHYS, UNIQ */
            const char* s = nr == 0x08 ? "" : nr == 0x07 ? (kbd ? "isa0060/serio0/input0" : "isa0060/serio1/input0")
                          : d == INPUT_KBD ? "SamaraOS keyboard" : d == INPUT_MOUSE ? "SamaraOS mouse" : "SamaraOS input";
            uint32_t n = 0;
            while (s[n]) n++;
            n++;
            if (n > len) n = len;
            for (uint32_t i = 0; i < n; i++) arg[i] = (uint8_t)s[i];
            if (n) arg[n - 1] = 0;
            return (int)n;
        }
        case 0x09: case 0x18: case 0x19: case 0x1a: case 0x1b:           /* PROP, KEY, LED, SND, SW: all clear */
            for (uint32_t i = 0; i < len; i++) arg[i] = 0;
            return (int)len;
        case 0x90: case 0x91: case 0xa0: return 0;                       /* GRAB, REVOKE, SCLOCKID */
    }
    if (nr >= 0x20 && nr < 0x40) {                                       /* EVIOCGBIT(ev) */
        for (uint32_t i = 0; i < len; i++) arg[i] = 0;
        int ev = (int)nr - 0x20;
        if (ev == 0) {
            setbit(arg, len, 0);
            setbit(arg, len, 1);
            if (mouse) setbit(arg, len, 2);
            if (kbd) setbit(arg, len, 0x14);                             /* EV_REP */
        } else if (ev == 1) {
            if (kbd) for (int k = 1; k < 0xF8; k++) setbit(arg, len, k);
            if (mouse) for (int k = 0x110; k <= 0x112; k++) setbit(arg, len, k);
        } else if (ev == 2 && mouse) {
            setbit(arg, len, 0); setbit(arg, len, 1); setbit(arg, len, 8);
        }
        return (int)len;
    }
    return -22;                                                          /* EVIOCGABS & co: no abs axes */
}
