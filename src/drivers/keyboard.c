#include "core/task.h"
#include "drivers/keyboard.h"
#include "core/io.h"
#include "boot/pic.h"
#include "boot/idt.h"
#include "drivers/input.h"
#include "drivers/fbdev.h"

#define K_ESC   0x1B

static const char scancode_lower[128] = {
    0,  K_ESC, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/',
    0, '*', 0, ' ', 0,
};
static const char scancode_upper[128] = {
    0,  K_ESC, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?',
    0, '*', 0, ' ', 0,
};

/* Russian ЙЦУКЕН layout — CP866 high bytes. Punctuation slots map to common
   Russian punctuation (point/comma) so the user can still type sentences. */
static const unsigned char scancode_ru_lower[128] = {
    0,  K_ESC, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t', 0xA9,0xE6,0xE3,0xAA,0xA5,0xAD,0xA3,0xE8,0xE9,0xA7,0xE5,0xEA,'\n', /* й ц у к е н г ш щ з х ъ */
    0,    0xE4,0xEB,0xA2,0xA0,0xAF,0xE0,0xAE,0xAB,0xA4,0xA6,0xED,']',         /* ф ы в а п р о л д ж э */
    0,    '\\',0xEF,0xE7,0xE1,0xAC,0xA8,0xE2,0xEC,0xA1,0xEE,'.',               /* я ч с м и т ь б ю . */
    0, '*', 0, ' ', 0,
};
static const unsigned char scancode_ru_upper[128] = {
    0,  K_ESC, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t', 0x89,0x96,0x93,0x8A,0x85,0x8D,0x83,0x98,0x99,0x87,0x95,0x9A,'\n', /* Й Ц У К Е Н Г Ш Щ З Х Ъ */
    0,    0x94,0x9B,0x82,0x80,0x8F,0x90,0x8E,0x8B,0x84,0x86,0x9D,'}',         /* Ф Ы В А П Р О Л Д Ж Э */
    0,    '|', 0x9F,0x97,0x91,0x8C,0x88,0x92,0x9C,0x81,0x9E,',',               /* Я Ч С М И Т Ь Б Ю , */
    0, '*', 0, ' ', 0,
};

/* Scancodes (set 1, without the E0 prefix) to drop entirely: for a broken,
   chattering physical key. Set from the kernel command line "kbd_ignore=35"
   or the shell's `kbdignore`. */
#define IGNORE_MAX 8
static uint8_t ignored[IGNORE_MAX];
static int     n_ignored;

static volatile uint8_t keybits[32];

/* press/release events by Linux key code for the WM (wayland windows) */
static volatile uint16_t rawq[64];
static volatile int raw_h, raw_t;

int kbd_raw_take(int* code, int* down) {
    if (raw_h == raw_t) return 0;
    uint16_t v = rawq[raw_t];
    raw_t = (raw_t + 1) & 63;
    *code = v & 0x7FFF;
    *down = v >> 15;
    return 1;
}

void kbd_key_bits(uint8_t out[32]) {
    for (int i = 0; i < 32; i++) out[i] = keybits[i];
}

void kbd_ignore_scancode(uint8_t sc) {
    for (int i = 0; i < n_ignored; i++) if (ignored[i] == sc) return;
    if (n_ignored < IGNORE_MAX) ignored[n_ignored++] = sc;
}

void kbd_unignore_all(void) { n_ignored = 0; }

static bool is_ignored(uint8_t sc) {
    for (int i = 0; i < n_ignored; i++) if (ignored[i] == sc) return true;
    return false;
}

static volatile bool     ru_layout = false;
static volatile int      hot_mode, lock_count, prtsc_count;   // hot_mode 1: ctrl+shift
static volatile uint32_t ru_epoch  = 0;
static volatile uint32_t f11_count = 0;     /* F11 presses: the WM toggles fullscreen */

static volatile char  buf[KEY_BUF_SIZE];
static volatile int   head = 0, tail = 0;
static volatile bool  shift = false, ctrl = false, alt = false, caps = false;
static volatile bool  ext = false;

static void buf_push(char c) {
    int n = (head + 1) % KEY_BUF_SIZE;
    if (n != tail) { buf[head] = c; head = n; }
}

static int buf_pop(char* out) {
    if (head == tail) return 0;
    *out = buf[tail];
    tail = (tail + 1) % KEY_BUF_SIZE;
    return 1;
}

static void kbd_byte(uint8_t sc) {

    if (sc == 0xE0) { ext = true; return; }

    bool released = sc & 0x80;
    sc &= 0x7F;

    if (!ext && is_ignored(sc)) { return; }

    /* Held-key bitmap by Linux key code, for games (see gui/uwin.c). */
    {
        uint16_t lk = input_linux_key(sc, ext);
        if (lk && lk < 256) {
            if (released) keybits[lk >> 3] &= (uint8_t)~(1u << (lk & 7));
            else          keybits[lk >> 3] |= (uint8_t)(1u << (lk & 7));
            int nh = (raw_h + 1) & 63;
            if (nh != raw_t) { rawq[raw_h] = (uint16_t)(lk | (released ? 0 : 0x8000)); raw_h = nh; }
        }
    }

    /* /dev/input owns the keyboard: raw make/break events, no characters. */
    if (input_grabbed()) {
        if (!ext) {
            switch (sc) {
                case 0x2A: case 0x36: shift = !released; break;
                case 0x1D: ctrl = !released; break;
                case 0x38: alt = !released; break;
            }
            if (sc == 0x57 && !released) f11_count++;
            /* Ctrl+Alt+Backspace: escape hatch. was ctrl+alt+q, that one quits qemu (gtk) */
            if (sc == 0x0E && !released && ctrl && alt) {
                input_release_grab();
                fbdev_force_release();
                        return;
            }
        }
        uint16_t code = input_linux_key(sc, ext);
        ext = false;
        if (code) { input_push(IEV_KEY, code, released ? 0 : 1); input_sync(INPUT_KBD); }
        return;
    }

    if (ext) {
        ext = false;
        if (!released) {
            switch (sc) {
                case 0x37: prtsc_count++; break;
                case 0x48: buf_push((char)K_UP); break;
                case 0x50: buf_push((char)K_DOWN); break;
                case 0x4B: buf_push((char)K_LEFT); break;
                case 0x4D: buf_push((char)K_RIGHT); break;
                case 0x47: buf_push((char)K_HOME); break;
                case 0x4F: buf_push((char)K_END); break;
                case 0x53: buf_push((char)K_DEL); break;
                case 0x49: buf_push((char)K_PGUP); break;
                case 0x51: buf_push((char)K_PGDN); break;
                /* Menu/Context key doubles as '/' ('?' with Shift): the
                   owner's '/' key is dead and remapped to Menu on the host,
                   but QEMU forwards the raw Menu scancode, not the remap. */
                case 0x5D: buf_push(shift ? '?' : '/'); break;
                case 0x35: buf_push('/'); break;           /* keypad '/' */
            }
        }
        return;
    }

    switch (sc) {
        /* Alt+Shift (either order) toggles RU/EN, fired on the second key. */
        case 0x2A: case 0x36:
            if (!released && !shift && (hot_mode ? ctrl : alt)) { ru_layout = !ru_layout; ru_epoch++; }
            shift = !released; return;
        case 0x1D:
            if (!released && !ctrl && hot_mode && shift) { ru_layout = !ru_layout; ru_epoch++; }
            ctrl  = !released; return;
        case 0x38:
            if (!released && !alt && shift) { ru_layout = !ru_layout; ru_epoch++; }
            alt   = !released; return;
        case 0x3A: if (!released) caps = !caps; return;
    }

    if (released) { return; }

    // super+l, keybits is already updated above
    if (sc == 0x26 && ((keybits[125 >> 3] >> (125 & 7)) & 1 || (keybits[126 >> 3] >> (126 & 7)) & 1)) { lock_count++; return; }

    if (sc >= 0x3B && sc <= 0x44) {
        buf_push((char)(K_F1 + (sc - 0x3B)));
        return;
    }
    /* F11: fullscreen toggle for the WM. Not a buffer code - everything
       above 0x80 is also CP866 Cyrillic - so it is counted on the side. */
    if (sc == 0x57) {
        f11_count++;
        return;
    }
    if (sc == 0x58) { return; }        /* F12: unused */

    /* base char */
    bool letter_case_up = shift ^ caps;
    char c;
    if (ru_layout) {
        c = letter_case_up
            ? (char)scancode_ru_upper[sc]
            : (char)scancode_ru_lower[sc];
        if (c == 0 && shift) c = (char)scancode_upper[sc];
        if (c == 0) c = scancode_lower[sc];
    } else {
        c = scancode_lower[sc];
        if (c >= 'a' && c <= 'z') {
            if (letter_case_up) c = (char)(c - 'a' + 'A');
        } else if (shift) {
            c = scancode_upper[sc];
        }
    }

    /* Ctrl + letter -> ASCII control code (0x01..0x1A) */
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else if (c == ' ') c = 0;       /* Ctrl+Space -> NUL, harmless */
        /* leave other ctrl combos alone */
    }

    if (c) buf_push(c);
}

static void kbd_isr(regs_t* f) {
    (void)f;
    /* Only take the byte if it is keyboard data: with the AUX bit set it is
       the mouse's (touchpads / BIOS USB emulation), and IRQ 12 reads it. */
    uint8_t status = inb(0x64);
    if ((status & 0x01) && !(status & 0x20)) kbd_byte(inb(0x60));
    pic_send_eoi(1);
}

// usb / virtio keyboards come in by linux key code, same path as the ps/2 bytes
void kbd_feed_key(uint16_t lk, bool down) {
    uint8_t sc = 0;
    bool x = true;
    switch (lk) {
        case 103: sc = 0x48; break;
        case 108: sc = 0x50; break;
        case 105: sc = 0x4B; break;
        case 106: sc = 0x4D; break;
        case 102: sc = 0x47; break;
        case 107: sc = 0x4F; break;
        case 104: sc = 0x49; break;
        case 109: sc = 0x51; break;
        case 110: sc = 0x52; break;
        case 111: sc = 0x53; break;
        case 97: sc = 0x1D; break;
        case 100: sc = 0x38; break;
        case 96: sc = 0x1C; break;
        case 98: sc = 0x35; break;
        case 127: sc = 0x5D; break;
        case 125: sc = 0x5B; break;
        case 126: sc = 0x5C; break;
        case 99: sc = 0x37; break;
        default: x = false; if (lk && lk < 0x59) sc = lk;
    }
    if (!sc) return;
    uint64_t fl;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    if (x) kbd_byte(0xE0);
    kbd_byte(down ? sc : sc | 0x80);
    if (fl & 0x200) __asm__ volatile ("sti" ::: "memory");
}

void kbd_init(void) {
    head = tail = 0;
    idt_set_handler(0x21, kbd_isr);
    pic_clear_mask(1);
}

int kbd_has_key(void) { return head != tail; }

char kbd_trygetc(void) {
    char c = 0;
    buf_pop(&c);
    return c;
}

char kbd_getc(void) {
    char c = 0;
    while (!buf_pop(&c)) cpu_wait();
    return c;
}

bool     kbd_is_ru(void)         { return ru_layout; }
void     kbd_set_hotkey(int m)   { hot_mode = m; }
int      kbd_hotkey(void)        { return hot_mode; }
int      kbd_lock_take(void)     { int n = lock_count; lock_count = 0; return n; }
int      kbd_prtsc_take(void)    { int n = prtsc_count; prtsc_count = 0; return n; }
void     kbd_set_ru(bool ru)     { if (ru != ru_layout) { ru_layout = ru; ru_epoch++; } }
uint32_t kbd_layout_epoch(void)  { return ru_epoch; }

bool kbd_ctrl_held(void) { return ctrl; }
bool kbd_shift_held(void) { return shift; }
bool kbd_alt_held(void) { return alt; }

int kbd_f11_take(void) {
    static uint32_t seen;
    int n = (int)(f11_count - seen);
    seen = f11_count;
    return n;
}
