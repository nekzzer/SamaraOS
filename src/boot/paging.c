#include "boot/paging.h"
#include "core/vmm.h"
#include "boot/idt.h"
#include "gfx/gfx.h"
#include "drivers/vga.h"
#include "core/io.h"
#include "proc/proc.h"

/* Single page directory, 4 MiB PSE pages, identity map of the full 4 GiB.
   Forced into .data (not .bss) via an explicit non-zero initializer so it
   lands far below the boot stack. A stack overflow would have to scribble
   over megabytes of .bss + .data first before it could corrupt this. */
__attribute__((aligned(4096)))
static uint32_t pdir[1024] = { 0xDEADBEEFu, /* rest zero-init */ };

/* PDE flags for a 4 MiB page: Present | RW | PageSize. */
#define PDE_P   0x001u
#define PDE_RW  0x002u
#define PDE_PS  0x080u

/* ---------- COM1 direct (no libc dependency, safe in IRQ ctx) ---------- */
static void com_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) {}
    outb(0x3F8, c);
}
static void com_str(const char* s) { while (*s) com_putc(*s++); }
static void com_hex(uint32_t v) {
    com_str("0x");
    const char* h = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) com_putc(h[(v >> (i*4)) & 0xF]);
}

/* Format a hex value into a small buffer (8 chars, no 0x prefix) */
static void hex8(char* out, uint32_t v) {
    const char* h = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) out[7-i] = h[(v >> (i*4)) & 0xF];
    out[8] = 0;
}

/* Draw a panic banner on the framebuffer so the user sees the dump even when
   serial is not being watched. Safe from interrupt context: writes go to the
   front buffer directly, no allocation, no libc. */
static void fb_panic_banner(const char* exc, uint32_t err, uint32_t eip,
                            uint32_t cs, uint32_t cr2) {
    if (!gfx_ready()) return;
    gfx_target_front();
    int W = gfx_w();
    /* Big red bar across the top so the user can't miss it. */
    gfx_rect_fill(0, 0, W, 120, RGB(0x80, 0x10, 0x10));

    char buf[80];
    int p = 0;
    const char* prefix = "[PANIC] ";
    while (prefix[p]) { buf[p] = prefix[p]; p++; }
    int e = 0; while (exc[e] && p < 70) { buf[p++] = exc[e++]; }
    buf[p] = 0;
    gfx_string(10, 10, buf, RGB(0xFF,0xFF,0xFF), RGB(0x80,0x10,0x10), true);

    char hex[9];
    hex8(hex, eip);
    char line[64];
    int q = 0;
    const char* l1 = "EIP=0x";
    while (l1[q]) { line[q] = l1[q]; q++; }
    for (int i = 0; i < 8; i++) line[q++] = hex[i];
    line[q++] = ' ';
    const char* l2 = "CS=0x";
    int k = 0; while (l2[k]) line[q++] = l2[k++];
    hex8(hex, cs);
    for (int i = 4; i < 8; i++) line[q++] = hex[i];  /* CS is 16-bit, show last 4 */
    line[q++] = ' ';
    const char* l3 = "ERR=0x";
    k = 0; while (l3[k]) line[q++] = l3[k++];
    hex8(hex, err);
    for (int i = 0; i < 8; i++) line[q++] = hex[i];
    line[q] = 0;
    gfx_string(10, 36, line, RGB(0xFF,0xFF,0xFF), RGB(0x80,0x10,0x10), true);

    q = 0;
    const char* l4 = "CR2=0x";
    while (l4[q]) { line[q] = l4[q]; q++; }
    hex8(hex, cr2);
    for (int i = 0; i < 8; i++) line[q++] = hex[i];
    line[q] = 0;
    gfx_string(10, 62, line, RGB(0xFF,0xFF,0xFF), RGB(0x80,0x10,0x10), true);

    gfx_string(10, 88, "Halted. Check COM1 for full dump.",
               RGB(0xFF,0xC0,0xC0), RGB(0x80,0x10,0x10), true);
}

static void dump(const char* tag, uint32_t err, uint32_t eip,
                 uint32_t cs, uint32_t cr2) {
    com_str("\r\n[#"); com_str(tag); com_str("] ");
    com_str("eip="); com_hex(eip);
    com_str(" cs=");  com_hex(cs);
    com_str(" err="); com_hex(err);
    com_str(" cr2="); com_hex(cr2);
    com_str("\r\n");

    /* Also try the text-mode VGA in case we ever run there. Harmless in gfx. */
    vga_printf("\n[#%s] eip=0x%x err=0x%x cr2=0x%x\n", tag, eip, err, cr2);

    fb_panic_banner(tag, err, eip, cs, cr2);
}

__attribute__((interrupt))
static void pf_isr(struct interrupt_frame* f, uint32_t err) {
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    /* Lazily grown user stack (also hit by the kernel copying into it). */
    if (proc_handle_fault(cr2, err)) return;
    if ((f->cs & 3) == 3) proc_fault_kill("Segmentation fault", 11, f->eip, cr2);
    dump("PF", err, f->eip, f->cs, cr2);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((interrupt))
static void gp_isr(struct interrupt_frame* f, uint32_t err) {
    if ((f->cs & 3) == 3) proc_fault_kill("General protection fault", 11, f->eip, err);
    dump("GP", err, f->eip, f->cs, 0);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((interrupt))
static void df_isr(struct interrupt_frame* f, uint32_t err) {
    dump("DF", err, f->eip, f->cs, 0);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((interrupt))
static void ud_isr(struct interrupt_frame* f) {
    if ((f->cs & 3) == 3) proc_fault_kill("Illegal instruction", 4, f->eip, 0);
    dump("UD", 0, f->eip, f->cs, 0);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

__attribute__((interrupt))
static void de_isr(struct interrupt_frame* f) {
    if ((f->cs & 3) == 3) proc_fault_kill("Floating point exception", 8, f->eip, 0);
    dump("DE", 0, f->eip, f->cs, 0);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

void paging_init(void) {
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t phys = i << 22;                 /* 4 MiB stride */
        pdir[i] = phys | PDE_P | PDE_RW | PDE_PS;
    }
    /* Direct map of physical RAM at DMAP_BASE (see core/vmm.h). */
    for (uint32_t i = 0; i < (DMAP_SIZE >> 22); i++)
        pdir[(DMAP_BASE >> 22) + i] = (i << 22) | PDE_P | PDE_RW | PDE_PS;

    idt_set_gate(0,  de_isr, 0x08, 0x8E);        /* #DE Divide Error */
    idt_set_gate(6,  ud_isr, 0x08, 0x8E);        /* #UD Invalid Opcode */
    idt_set_gate(8,  df_isr, 0x08, 0x8E);        /* #DF Double Fault */
    idt_set_gate(13, gp_isr, 0x08, 0x8E);        /* #GP General Protection */
    idt_set_gate(14, pf_isr, 0x08, 0x8E);        /* #PF Page Fault */

    __asm__ volatile (
        "mov %0, %%cr3\n\t"
        "mov %%cr4, %%eax\n\t"
        "or  $0x00000010, %%eax\n\t"             /* CR4.PSE */
        "mov %%eax, %%cr4\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80010000, %%eax\n\t"             /* CR0.PG | CR0.WP */
        "mov %%eax, %%cr0\n\t"
        : : "r"(pdir) : "eax", "memory"
    );
}

uint32_t paging_cr0(void) { uint32_t r; __asm__ volatile ("mov %%cr0, %0":"=r"(r)); return r; }
uint32_t paging_cr3(void) { uint32_t r; __asm__ volatile ("mov %%cr3, %0":"=r"(r)); return r; }
uint32_t paging_cr4(void) { uint32_t r; __asm__ volatile ("mov %%cr4, %0":"=r"(r)); return r; }
