#include "drivers/input.h"
#include "boot/pit.h"
#include "core/task.h"
#include "proc/proc.h"

#define IN_CAP 1024

typedef struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } input_event_t;

static input_event_t ring[IN_CAP];
static volatile int head, tail;
static volatile int users;
static volatile bool grab;

void input_open(void) {
    if (users++ == 0) head = tail = 0;
    grab = true;
}

void input_close(void) {
    if (users > 0 && --users == 0) {
        grab = false;
        head = tail = 0;
    }
}

bool input_grabbed(void) { return grab; }
void input_release_grab(void) { grab = false; }

void input_push(uint16_t type, uint16_t code, int32_t value) {
    int n = (head + 1) % IN_CAP;
    if (n == tail) return;                       /* full: drop */
    uint32_t ms = pit_uptime_ms();
    ring[head].sec = ms / 1000;
    ring[head].usec = (ms % 1000) * 1000;
    ring[head].type = type;
    ring[head].code = code;
    ring[head].value = value;
    head = n;
}

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

bool input_pending(void) { return head != tail; }

int input_read(char* buf, uint32_t n, bool nonblock) {
    if (n < sizeof(input_event_t)) return -22;   /* EINVAL */
    while (head == tail) {
        if (nonblock) return -11;                /* EAGAIN */
        if (!grab && users == 0) return 0;
        if (proc_interrupted()) return -4;       /* EINTR */
        task_yield();
    }
    uint32_t got = 0;
    while (got + sizeof(input_event_t) <= n && head != tail) {
        *(input_event_t*)(buf + got) = ring[tail];
        tail = (tail + 1) % IN_CAP;
        got += sizeof(input_event_t);
    }
    return (int)got;
}
