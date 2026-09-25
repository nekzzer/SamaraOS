#include "drivers/input.h"
#include "drivers/mouse.h"
#include "core/io.h"
#include "boot/pic.h"
#include "boot/idt.h"
#include "drivers/vga.h"

/* PS/2 mouse over the controller at 0x60/0x64 */

static volatile int mx = 320, my = 200;
static volatile int mxmax = 80 * 8 - 1, mymax = 25 * 16 - 1;
static volatile uint8_t btn = 0;
static volatile uint8_t pkt[3];
static volatile int pkt_idx = 0;

static volatile uint16_t saved_cell = 0;
static volatile int saved_x = -1, saved_y = -1;
static volatile bool cursor_visible = false;
static volatile bool text_cursor_enabled = true;

void mouse_set_text_cursor(bool e) {
    if (!e && cursor_visible && saved_x >= 0) {
        vga_set_cell(saved_x, saved_y, saved_cell);
        cursor_visible = false;
        saved_x = saved_y = -1;
    }
    text_cursor_enabled = e;
}

void mouse_set_range(int max_x, int max_y) {
    mxmax = max_x;
    mymax = max_y;
    if (mx > mxmax) mx = mxmax;
    if (my > mymax) my = mymax;
}

void mouse_get_range(int* max_x, int* max_y) {
    *max_x = mxmax;
    *max_y = mymax;
}

void mouse_set_pos(int x, int y) {
    if (x < 0) x = 0;
    if (x > mxmax) x = mxmax;
    if (y < 0) y = 0;
    if (y > mymax) y = mymax;
    mx = x; my = y;
}

static void mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return;
}
static void mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return;
}

static void mouse_cmd(uint8_t cmd) {
    mouse_wait_write(); outb(0x64, 0xD4);
    mouse_wait_write(); outb(0x60, cmd);
    mouse_wait_read();  inb(0x60);   /* ACK */
}

void mouse_hide_cursor(void) {
    if (cursor_visible && saved_x >= 0) {
        vga_set_cell(saved_x, saved_y, saved_cell);
    }
    cursor_visible = false;
    saved_x = saved_y = -1;
}

void mouse_draw_cursor(void) {
    if (!text_cursor_enabled) return;
    int cx = (mx * VGA_WIDTH)  / (mxmax + 1);
    int cy = (my * VGA_HEIGHT) / (mymax + 1);
    if (cx >= VGA_WIDTH)  cx = VGA_WIDTH - 1;
    if (cy >= VGA_HEIGHT) cy = VGA_HEIGHT - 1;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;

    if (cursor_visible && (cx != saved_x || cy != saved_y)) {
        vga_set_cell(saved_x, saved_y, saved_cell);
        cursor_visible = false;
    }
    if (!cursor_visible) {
        saved_cell = vga_get_cell(cx, cy);
        saved_x = cx; saved_y = cy;
    }
    /* invert colors: swap fg/bg of saved cell */
    uint16_t s = saved_cell;
    uint8_t  ch = s & 0xFF;
    uint8_t  col = (s >> 8) & 0xFF;
    uint8_t  inv = ((col & 0x0F) << 4) | ((col >> 4) & 0x0F);
    vga_set_cell(cx, cy, ((uint16_t)inv << 8) | ch);
    cursor_visible = true;
}

__attribute__((interrupt))
static void mouse_isr(struct interrupt_frame* f) {
    (void)f;
    if (!(inb(0x64) & 1)) { pic_send_eoi(12); return; }
    uint8_t v = inb(0x60);

    if (pkt_idx == 0 && !(v & 0x08)) { pic_send_eoi(12); return; } /* sync */

    pkt[pkt_idx++] = v;
    if (pkt_idx < 3) { pic_send_eoi(12); return; }
    pkt_idx = 0;

    uint8_t flags = pkt[0];
    int dx = pkt[1];
    int dy = pkt[2];
    if (flags & 0x10) dx |= 0xFFFFFF00;
    if (flags & 0x20) dy |= 0xFFFFFF00;
    if (flags & 0x40 || flags & 0x80) { pic_send_eoi(12); return; } /* overflow */

    uint8_t newbtn = flags & 0x07;
    if (input_grabbed()) {
        if (dx) input_push(IEV_REL, IEV_REL_X, dx);
        if (dy) input_push(IEV_REL, IEV_REL_Y, -dy);
        for (int i = 0; i < 3; i++)
            if ((newbtn ^ btn) & (1 << i))
                input_push(IEV_KEY, (uint16_t)(0x110 + i), (newbtn >> i) & 1);
        btn = newbtn;
        pic_send_eoi(12);
        return;
    }
    btn = newbtn;
    mx += dx;
    my -= dy;          /* invert Y to screen coords */

    if (mx < 0) mx = 0;
    if (mx > mxmax) mx = mxmax;
    if (my < 0) my = 0;
    if (my > mymax) my = mymax;

    mouse_draw_cursor();
    pic_send_eoi(12);
}

void mouse_init(void) {
    /* enable aux device */
    mouse_wait_write(); outb(0x64, 0xA8);
    /* enable IRQ12 in compaq status byte */
    mouse_wait_write(); outb(0x64, 0x20);
    mouse_wait_read();  uint8_t status = inb(0x60) | 2;
    status &= ~0x20;
    mouse_wait_write(); outb(0x64, 0x60);
    mouse_wait_write(); outb(0x60, status);
    /* defaults + enable streaming */
    mouse_cmd(0xF6);
    mouse_cmd(0xF4);

    idt_set_gate(0x2C, mouse_isr, 0x08, 0x8E);
    pic_clear_mask(2);   /* unmask cascade */
    pic_clear_mask(12);
}

void mouse_get(int* x, int* y, uint8_t* b) {
    *x = mx; *y = my; *b = btn;
}
