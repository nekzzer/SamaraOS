#include "keyboard.h"
#include "io.h"
#include "pic.h"
#include "idt.h"

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

__attribute__((interrupt))
static void kbd_isr(struct interrupt_frame* f) {
    (void)f;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { ext = true; pic_send_eoi(1); return; }

    bool released = sc & 0x80;
    sc &= 0x7F;

    if (ext) {
        ext = false;
        if (!released) {
            switch (sc) {
                case 0x48: buf_push((char)K_UP); break;
                case 0x50: buf_push((char)K_DOWN); break;
                case 0x4B: buf_push((char)K_LEFT); break;
                case 0x4D: buf_push((char)K_RIGHT); break;
                case 0x47: buf_push((char)K_HOME); break;
                case 0x4F: buf_push((char)K_END); break;
                case 0x53: buf_push((char)K_DEL); break;
                case 0x49: buf_push((char)K_PGUP); break;
                case 0x51: buf_push((char)K_PGDN); break;
            }
        }
        pic_send_eoi(1);
        return;
    }

    switch (sc) {
        case 0x2A: case 0x36: shift = !released; pic_send_eoi(1); return;
        case 0x1D: ctrl  = !released; pic_send_eoi(1); return;
        case 0x38: alt   = !released; pic_send_eoi(1); return;
        case 0x3A: if (!released) caps = !caps; pic_send_eoi(1); return;
    }

    if (released) { pic_send_eoi(1); return; }

    if (sc >= 0x3B && sc <= 0x44) {
        buf_push((char)(K_F1 + (sc - 0x3B)));
        pic_send_eoi(1);
        return;
    }

    /* base char */
    bool letter_case_up = shift ^ caps;
    char c = scancode_lower[sc];
    if (c >= 'a' && c <= 'z') {
        if (letter_case_up) c = (char)(c - 'a' + 'A');
    } else if (shift) {
        c = scancode_upper[sc];
    }

    /* Ctrl + letter -> ASCII control code (0x01..0x1A) */
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else if (c == ' ') c = 0;       /* Ctrl+Space -> NUL, harmless */
        /* leave other ctrl combos alone */
    }

    if (c) buf_push(c);
    pic_send_eoi(1);
}

void kbd_init(void) {
    head = tail = 0;
    idt_set_gate(0x21, kbd_isr, 0x08, 0x8E);
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
    while (!buf_pop(&c)) { __asm__ volatile ("sti; hlt"); }
    return c;
}
