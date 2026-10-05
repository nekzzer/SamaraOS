#include "drivers/input.h"
#include "boot/pit.h"
#include "core/task.h"
#include "proc/proc.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "fs/fs.h"
#include "core/string.h"

int snprintf(char* buf, size_t n, const char* fmt, ...);

#define IN_CAP 1024

typedef struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } input_event_t;

/* 0..2 are the old fixed readers, 3+i is /dev/input/event<i> */
static struct { input_event_t ev[IN_CAP]; volatile int head, tail, users; } r[3 + IN_DEVS];
static struct {
    bool used;
    input_dev_t d;
    uint8_t held[96];
    int dx, dy, dz, ax, ay, kmask;
    uint8_t btn, obtn;
    bool hasabs;
} dv[IN_DEVS];
static int ps2k = -1, ps2m = -1;
static volatile bool grab;

void input_open(int d) {
    if (r[d].users++ == 0) r[d].head = r[d].tail = 0;
    grab = true;
}

void input_close(int d) {
    if (r[d].users > 0 && --r[d].users == 0) r[d].head = r[d].tail = 0;
    for (int i = 0; i < 3 + IN_DEVS; i++) if (r[i].users) return;
    grab = false;
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
    bool k = type == 1 && code < 0x100;      /* buttons (0x110..) go with the mouse */
    put(0, type, code, value);
    put(k ? 1 : 2, type, code, value);
    if (k ? ps2k >= 0 : ps2m >= 0) put(3 + (k ? ps2k : ps2m), type, code, value);
}

void input_sync(int d) {
    put(d, 0, 0, 0);
    if (d == INPUT_KBD && ps2k >= 0) put(3 + ps2k, 0, 0, 0);
    if (d == INPUT_MOUSE && ps2m >= 0) put(3 + ps2m, 0, 0, 0);
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

bool input_pending(int d) { return r[d].head != r[d].tail; }

int input_read(int d, char* buf, uint32_t n, bool nonblock) {
    if (n < sizeof(input_event_t)) return -22;   /* EINVAL */
    while (r[d].head == r[d].tail) {
        if (nonblock) return -11;                /* EAGAIN */
        if (!grab && r[d].users == 0) return 0;
        if (d >= 3 && !dv[d - 3].used) return -19;   /* ENODEV, unplugged */
        if (proc_interrupted()) return -4;       /* EINTR */
        task_yield();
    }
    uint32_t got = 0, sz = d >= 3 ? 24 : sizeof(input_event_t);   /* eventN: 64 bit timeval like x86_64 linux */
    if (n < sz) return -22;
    while (got + sz <= n && r[d].head != r[d].tail) {
        input_event_t* e = &r[d].ev[r[d].tail];
        if (d >= 3) {
            uint64_t* t = (uint64_t*)(buf + got);
            t[0] = e->sec; t[1] = e->usec;
            *(uint16_t*)(buf + got + 16) = e->type;
            *(uint16_t*)(buf + got + 18) = e->code;
            *(int32_t*)(buf + got + 20) = e->value;
        } else *(input_event_t*)(buf + got) = *e;
        r[d].tail = (r[d].tail + 1) % IN_CAP;
        got += sz;
    }
    return (int)got;
}

static void setbit(uint8_t* b, uint32_t len, int i) { if ((uint32_t)i / 8 < len) b[i / 8] |= (uint8_t)(1 << (i & 7)); }

static void put_file(const char* path, const char* s) {
    fs_node_t* n = fs_resolve(fs_root(), path);
    if (!n) n = fs_create(fs_root(), path, FS_FILE);
    if (n) fs_write(n, s, strlen(s));
}

static void mk_dir(const char* path) {
    char part[128];
    int len = 0;
    for (const char* p = path; ; p++) {
        if (*p == '/' || !*p) {
            part[len] = 0;
            if (len > 1 && !fs_resolve(fs_root(), part)) fs_create(fs_root(), part, FS_DIR);
            if (!*p) break;
        }
        if (len < 127) part[len++] = *p;
    }
}

// 64 bit words, high first, "0" if empty. that is how sysfs prints capabilities
static void hexmap(char* o, int cap, const uint8_t* b, int n) {
    int w = n / 8, len = 0, any = 0;
    for (int i = w - 1; i >= 0; i--) {
        uint64_t v = 0;
        for (int j = 7; j >= 0; j--) v = v << 8 | b[i * 8 + j];
        if (!v && !any && i) continue;
        len += snprintf(o + len, cap - len, any ? " %lx" : "%lx", v);
        any = 1;
    }
    snprintf(o + len, cap - len, "\n");
}

static void mk_sysfs(int i) {
    input_dev_t* d = &dv[i].d;
    char p[160], b[300], q[160];
    uint8_t m[96];
    snprintf(p, sizeof p, "/sys/devices/virtual/input/input%d", i);
    snprintf(q, sizeof q, "%s/event%d", p, i);
    mk_dir(q);
    snprintf(q, sizeof q, "%s/id", p); mk_dir(q);
    snprintf(q, sizeof q, "%s/capabilities", p); mk_dir(q);
    snprintf(q, sizeof q, "%s/name", p); snprintf(b, sizeof b, "%s\n", d->name); put_file(q, b);
    snprintf(q, sizeof q, "%s/phys", p); put_file(q, "\n");
    snprintf(q, sizeof q, "%s/uniq", p); put_file(q, "\n");
    snprintf(q, sizeof q, "%s/properties", p); put_file(q, "0\n");
    snprintf(q, sizeof q, "%s/id/bustype", p); snprintf(b, sizeof b, "%04x\n", d->bus); put_file(q, b);
    snprintf(q, sizeof q, "%s/id/vendor", p); snprintf(b, sizeof b, "%04x\n", d->vid); put_file(q, b);
    snprintf(q, sizeof q, "%s/id/product", p); snprintf(b, sizeof b, "%04x\n", d->pid); put_file(q, b);
    snprintf(q, sizeof q, "%s/id/version", p); snprintf(b, sizeof b, "%04x\n", d->ver); put_file(q, b);
    snprintf(q, sizeof q, "%s/capabilities/ev", p); snprintf(b, sizeof b, "%x\n", d->ev); put_file(q, b);
    snprintf(q, sizeof q, "%s/capabilities/key", p); hexmap(b, sizeof b, d->key, 96); put_file(q, b);
    memset(m, 0, 8); memcpy(m, &d->rel, 4);
    snprintf(q, sizeof q, "%s/capabilities/rel", p); hexmap(b, sizeof b, m, 8); put_file(q, b);
    memset(m, 0, 8); memcpy(m, &d->abs, 4);
    snprintf(q, sizeof q, "%s/capabilities/abs", p); hexmap(b, sizeof b, m, 8); put_file(q, b);
    snprintf(q, sizeof q, "%s/capabilities/led", p); put_file(q, "0\n");
    snprintf(q, sizeof q, "%s/capabilities/msc", p); put_file(q, "0\n");
    snprintf(q, sizeof q, "%s/capabilities/sw", p); put_file(q, "0\n");
    snprintf(q, sizeof q, "%s/capabilities/ff", p); put_file(q, "0\n");
    snprintf(q, sizeof q, "%s/capabilities/snd", p); put_file(q, "0\n");
    snprintf(b, sizeof b, "PRODUCT=%x/%x/%x/%x\nNAME=\"%s\"\nPROP=0\nEV=%x\nMODALIAS=input:b%04Xv%04Xp%04Xe%04X\n",
             d->bus, d->vid, d->pid, d->ver, d->name, d->ev, d->bus, d->vid, d->pid, d->ver);
    snprintf(q, sizeof q, "%s/uevent", p); put_file(q, b);
    snprintf(q, sizeof q, "%s/event%d/dev", p, i); snprintf(b, sizeof b, "13:%d\n", 64 + i); put_file(q, b);
    snprintf(b, sizeof b, "MAJOR=13\nMINOR=%d\nDEVNAME=input/event%d\n", 64 + i, i);
    snprintf(q, sizeof q, "%s/event%d/uevent", p, i); put_file(q, b);
    snprintf(q, sizeof q, "%s/event%d/device", p, i);
    if (!fs_resolve_nf(fs_root(), q)) {
        snprintf(q, sizeof q, "%s/event%d", p, i);
        fs_symlink(fs_resolve(fs_root(), q), "device", "..");
    }
    mk_dir("/sys/class/input");
    mk_dir("/sys/dev/char");
    char l[100], n[16];
    snprintf(n, sizeof n, "input%d", i);
    snprintf(l, sizeof l, "../../devices/virtual/input/input%d", i);
    if (!fs_resolve_nf(fs_root(), "/sys/class/input") ) return;
    snprintf(q, sizeof q, "/sys/class/input/%s", n);
    if (!fs_resolve_nf(fs_root(), q)) fs_symlink(fs_resolve(fs_root(), "/sys/class/input"), n, l);
    snprintf(n, sizeof n, "event%d", i);
    snprintf(l, sizeof l, "../../devices/virtual/input/input%d/event%d", i, i);
    snprintf(q, sizeof q, "/sys/class/input/%s", n);
    if (!fs_resolve_nf(fs_root(), q)) fs_symlink(fs_resolve(fs_root(), "/sys/class/input"), n, l);
    snprintf(n, sizeof n, "13:%d", 64 + i);
    snprintf(l, sizeof l, "../../devices/virtual/input/input%d/event%d", i, i);
    snprintf(q, sizeof q, "/sys/dev/char/%s", n);
    if (!fs_resolve_nf(fs_root(), q)) fs_symlink(fs_resolve(fs_root(), "/sys/dev/char"), n, l);
}

int input_register(const input_dev_t* d) {
    int i = 0;
    while (i < IN_DEVS && dv[i].used) i++;
    if (i == IN_DEVS) return -1;
    memset(&dv[i], 0, sizeof(dv[i]));
    dv[i].d = *d;
    r[3 + i].head = r[3 + i].tail = 0;
    char p[40];
    snprintf(p, sizeof p, "/dev/input/event%d", i);
    mk_dir("/dev/input");
    fs_node_t* n = fs_resolve(fs_root(), p);
    if (!n) n = fs_create(fs_root(), p, FS_FILE);
    if (n) { n->dev = FS_DEV_EVENT + i; n->mode = 0666; }
    mk_sysfs(i);
    dv[i].used = true;
    return i;
}

void input_unregister(int i) {
    if (i < 0 || !dv[i].used) return;
    dv[i].used = false;
    char p[40];
    snprintf(p, sizeof p, "/dev/input/event%d", i);
    fs_unlink(fs_root(), p);
    if (i == ps2k) ps2k = -1;
    if (i == ps2m) ps2m = -1;
}

void input_init(void) {
    input_dev_t k, m;
    memset(&k, 0, sizeof k);
    memset(&m, 0, sizeof m);
    strcpy(k.name, "AT Translated Set 2 keyboard");
    k.bus = 0x11; k.vid = 1; k.pid = 1; k.ver = 0xab41;
    k.ev = 0x120003;
    for (int c = 1; c < 0xF8; c++) k.key[c / 8] |= 1 << (c & 7);
    strcpy(m.name, "ImExPS/2 Generic Explorer Mouse");
    m.bus = 0x11; m.vid = 2; m.pid = 6; m.ver = 0xab41;
    m.ev = 7;
    for (int c = 0x110; c <= 0x112; c++) m.key[c / 8] |= 1 << (c & 7);
    m.rel = 0x103;
    ps2k = input_register(&k);
    ps2m = input_register(&m);
}

// usb/virtio devices report here, linux style. SYN sends the packet on to the DE
void input_report(int i, uint16_t type, uint16_t code, int32_t val) {
    if (i < 0 || !dv[i].used) return;
    uint64_t fl = irq_save();
    if (type == IEV_SYN) {
        put(3 + i, 0, 0, 0);
        if (dv[i].kmask & 1) put(1, 0, 0, 0);
        if (dv[i].kmask & 2) put(2, 0, 0, 0);
        dv[i].kmask = 0;
        if (!grab) {
            if (dv[i].hasabs) {
                input_dev_t* d = &dv[i].d;
                mouse_feed_abs(dv[i].ax - d->absmin[0], dv[i].ay - d->absmin[1], d->absmax[0] - d->absmin[0],
                               d->absmax[1] - d->absmin[1], -dv[i].dz, dv[i].btn);
            } else if (dv[i].dx || dv[i].dy || dv[i].dz || dv[i].btn != dv[i].obtn)
                mouse_feed(dv[i].dx, -dv[i].dy, -dv[i].dz, dv[i].btn);
        }
        dv[i].dx = dv[i].dy = dv[i].dz = 0;
        dv[i].obtn = dv[i].btn;
        irq_restore(fl);
        return;
    }
    put(3 + i, type, code, val);
    if (type == IEV_KEY && code < 0x100) {
        put(0, type, code, val);
        put(1, type, code, val);
        dv[i].kmask |= 1;
        if (val != 2) {
            if (val) dv[i].held[code / 8] |= 1 << (code & 7);
            else dv[i].held[code / 8] &= ~(1 << (code & 7));
            if (!grab) kbd_feed_key(code, val);
        }
    } else if (type == IEV_KEY) {
        put(0, type, code, val);
        put(2, type, code, val);
        dv[i].kmask |= 2;
        if (code >= 0x110 && code <= 0x112) {
            if (val) dv[i].btn |= 1 << (code - 0x110);
            else dv[i].btn &= ~(1 << (code - 0x110));
        }
    } else if (type == IEV_REL) {
        put(0, type, code, val);
        put(2, type, code, val);
        dv[i].kmask |= 2;
        if (code == IEV_REL_X) dv[i].dx += val;
        else if (code == IEV_REL_Y) dv[i].dy += val;
        else if (code == IEV_REL_WHEEL) dv[i].dz += val;
    } else if (type == IEV_ABS) {
        dv[i].hasabs = true;
        if (code == 0) dv[i].ax = val;
        else if (code == 1) dv[i].ay = val;
    }
    irq_restore(fl);
}

// key repeat for the DE only, apps on evdev do their own
void input_repeat(uint16_t code) {
    if (!grab) kbd_feed_key(code, 1);
}

static int dev_ioctl(int i, uint32_t req, uint8_t* arg) {
    uint32_t nr = req & 0xFF, len = (req >> 16) & 0x3FFF;
    input_dev_t* d = &dv[i].d;
    if (((req >> 8) & 0xFF) != 0x45) return -25;
    switch (nr) {
        case 0x01: *(int*)arg = 0x010001; return 0;
        case 0x02: {
            uint16_t* id = (uint16_t*)arg;
            id[0] = d->bus; id[1] = d->vid; id[2] = d->pid; id[3] = d->ver;
            return 0;
        }
        case 0x03: ((int*)arg)[0] = 250; ((int*)arg)[1] = 33; return 0;
        case 0x06: case 0x07: case 0x08: {
            const char* s = nr == 0x06 ? d->name : "";
            uint32_t n = strlen(s) + 1;
            if (n > len) n = len;
            for (uint32_t k = 0; k < n; k++) arg[k] = s[k];
            if (n) arg[n - 1] = 0;
            return (int)n;
        }
        case 0x09: case 0x19: case 0x1a: case 0x1b:
            for (uint32_t k = 0; k < len; k++) arg[k] = 0;
            return (int)len;
        case 0x18:
            for (uint32_t k = 0; k < len; k++) arg[k] = k < 96 ? dv[i].held[k] : 0;
            return (int)len;
        case 0x90: case 0x91: case 0xa0: return 0;
    }
    if (nr >= 0x20 && nr < 0x40) {
        for (uint32_t k = 0; k < len; k++) arg[k] = 0;
        int ev = (int)nr - 0x20;
        if (ev == 0) {
            for (uint32_t k = 0; k < len && k < 4; k++) arg[k] = d->ev >> (8 * k);
        } else if (ev == 1) {
            for (uint32_t k = 0; k < len && k < 96; k++) arg[k] = d->key[k];
        } else if (ev == 2) {
            for (uint32_t k = 0; k < len && k < 4; k++) arg[k] = d->rel >> (8 * k);
        } else if (ev == 3) {
            for (uint32_t k = 0; k < len && k < 4; k++) arg[k] = d->abs >> (8 * k);
        }
        return (int)len;
    }
    if (nr >= 0x40 && nr < 0x80) {                                       /* EVIOCGABS */
        int a = (int)nr - 0x40;
        if (a >= 8 || !(d->abs & (1u << a)) || len < 24) return -22;
        int32_t* v = (int32_t*)arg;
        memset(v, 0, 24);
        v[0] = a == 0 ? dv[i].ax : a == 1 ? dv[i].ay : 0;
        v[1] = d->absmin[a]; v[2] = d->absmax[a];
        return 0;
    }
    return -22;
}

/* what libevdev asks before xorg's evdev takes a device. req is the whole
   _IOC: size in bits 16..29, 'E' = 0x45 in 8..15, nr in 0..7 */
int input_ioctl(int d, uint32_t req, uint8_t* arg) {
    uint32_t nr = req & 0xFF, len = (req >> 16) & 0x3FFF;
    if (d >= 3) return dev_ioctl(d - 3, req, arg);
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
