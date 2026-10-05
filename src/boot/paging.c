#include "boot/paging.h"
#include "core/vmm.h"
#include "boot/idt.h"
#include "gfx/gfx.h"
#include "drivers/vga.h"
#include "core/io.h"
#include "proc/proc.h"
#include "core/task.h"

/* boot.S built these: identity + direct map + kernel image over the first 4 GiB */
extern uint64_t boot_pml4[], boot_pdpt_a[], boot_pd[];

/* page directories for RAM above 4 GiB, 2 MiB pages (up to 64 GiB) */
#define HI_PDS 60
__attribute__((aligned(4096))) static uint64_t hi_pd[HI_PDS][512];

/* ---------- COM1 direct (no libc dependency, safe in IRQ ctx) ---------- */
static void com_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) {}
    outb(0x3F8, c);
}
static void com_str(const char* s) { while (*s) com_putc(*s++); }
static void com_hex(uint64_t v) {
    com_str("0x");
    const char* h = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) com_putc(h[(v >> (i*4)) & 0xF]);
}

static void hex16(char* out, uint64_t v) {
    const char* h = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) out[15-i] = h[(v >> (i*4)) & 0xF];
    out[16] = 0;
}

/* Draw a panic banner on the framebuffer so the user sees the dump even when
   serial is not being watched. Safe from interrupt context: writes go to the
   front buffer directly, no allocation, no libc. */
static void fb_panic_banner(const char* exc, uint64_t err, uint64_t eip,
                            uint64_t cs, uint64_t cr2) {
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

    char hex[17];
    hex16(hex, eip);
    char line[80];
    int q = 0;
    const char* l1 = "RIP=0x";
    while (l1[q]) { line[q] = l1[q]; q++; }
    for (int i = 0; i < 16; i++) line[q++] = hex[i];
    line[q++] = ' ';
    const char* l2 = "CS=0x";
    int k = 0; while (l2[k]) line[q++] = l2[k++];
    hex16(hex, cs);
    for (int i = 12; i < 16; i++) line[q++] = hex[i];
    line[q++] = ' ';
    const char* l3 = "ERR=0x";
    k = 0; while (l3[k]) line[q++] = l3[k++];
    hex16(hex, err);
    for (int i = 8; i < 16; i++) line[q++] = hex[i];
    line[q] = 0;
    gfx_string(10, 36, line, RGB(0xFF,0xFF,0xFF), RGB(0x80,0x10,0x10), true);

    q = 0;
    const char* l4 = "CR2=0x";
    while (l4[q]) { line[q] = l4[q]; q++; }
    hex16(hex, cr2);
    for (int i = 0; i < 16; i++) line[q++] = hex[i];
    line[q] = 0;
    gfx_string(10, 62, line, RGB(0xFF,0xFF,0xFF), RGB(0x80,0x10,0x10), true);

    gfx_string(10, 88, "Halted. Check COM1 for full dump.",
               RGB(0xFF,0xC0,0xC0), RGB(0x80,0x10,0x10), true);
}

static void dump(const char* tag, uint64_t err, uint64_t rip,
                 uint64_t cs, uint64_t cr2) {
    com_str("\r\n[#"); com_str(tag); com_str("] ");
    com_str("rip="); com_hex(rip);
    com_str(" cs=");  com_hex(cs);
    com_str(" err="); com_hex(err);
    com_str(" cr2="); com_hex(cr2);
    com_str("\r\n");

    vga_printf("\n[#%s] rip=0x%lx err=0x%lx cr2=0x%lx\n", tag, rip, err, cr2);

    fb_panic_banner(tag, err, rip, cs, cr2);
}

/* #PF #GP #UD #DE #DF come here with a full frame, so a ring 3 fault can
   become a signal with a context the handler may edit - java needs that */
extern void longjmp(void* env, int val) __attribute__((noreturn));
static void fault_c(regs_t* r) {
    uint64_t vec = r->vec, err = r->err, cr2 = 0;
    if (vec == 14) {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        /* Lazily grown user stack (also hit by the kernel copying into it). */
        if (proc_handle_fault(cr2, err)) return;
    }
    if ((r->cs & 3) == 3) {
        int sig = vec == 6 ? 4 : vec == 0 ? 8 : 11;
        if (proc_fault_signal(r, sig, vec, err, cr2)) return;      /* the handler runs on iret */
        const char* what = vec == 14 ? "Segmentation fault" : vec == 13 ? "General protection fault"
                         : vec == 6 ? "Illegal instruction" : "Floating point exception";
        if (vec == 14) proc_fault_stack(r->rsp);
        proc_fault_kill(what, sig, r->rip, vec == 14 ? cr2 : err);
    }
    /* kernel tripped over a bad pointer from a syscall: -EFAULT via longjmp
       back into syscall_dispatch */
    if (vec == 14 && cr2 >= USER_BASE && cr2 < USER_TOP && proc_current() && proc_current()->ujb_on) {
        proc_current()->ujb_on = false;
        if (r->rflags & 0x200) __asm__ volatile ("sti");
        longjmp((void*)proc_current()->ujb, 1);
    }
    if (vec == 14 && cr2 >= USER_BASE && cr2 < USER_TOP && proc_current())
        proc_fault_kill("bad user ptr", 11, r->rip, cr2);
    {   // poor man's backtrace
        uint64_t* sp = (uint64_t*)r->rsp;
        extern int last_vec; com_str("last="); com_hex(last_vec); com_str("rsp="); com_hex(r->rsp); com_str(" stk:");
        for (int i = -60; i < 400; i++) if (sp[i] >= 0xffffffff80100000ul && sp[i] < 0xffffffff801b5000ul) { com_str(" "); com_hex(i); com_str(":"); com_hex(sp[i]); }
        com_str("\r\n");
    }
    dump(vec == 14 ? "PF" : vec == 13 ? "GP" : vec == 6 ? "UD" : vec == 8 ? "DF" : "DE", err, r->rip, r->cs, cr2);
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

void paging_init(uint64_t ram_top) {
    /* the boot tables stay the kernel's. drop the identity map, and map RAM past 4 GiB too */
    boot_pml4[0] = 0;
    uint64_t top = ram_top > 0x1000000000ull ? 0x1000000000ull : ram_top;
    for (uint64_t g = 4; g * 0x40000000ull < top && g - 4 < HI_PDS; g++) {
        for (int i = 0; i < 512; i++) hi_pd[g - 4][i] = (g << 30) | ((uint64_t)i << 21) | 0x83;
        boot_pdpt_a[g] = V2P(hi_pd[g - 4]) | 3;
    }
    __asm__ volatile ("mov %0, %%cr3" : : "r"(V2P(boot_pml4)) : "memory");

    idt_set_handler(0, fault_c);
    idt_set_handler(6, fault_c);
    idt_set_handler(8, fault_c);
    idt_set_handler(13, fault_c);
    idt_set_handler(14, fault_c);
}

uint64_t paging_cr0(void) { uint64_t r; __asm__ volatile ("mov %%cr0, %0":"=r"(r)); return r; }
uint64_t paging_cr3(void) { uint64_t r; __asm__ volatile ("mov %%cr3, %0":"=r"(r)); return r; }
uint64_t paging_cr4(void) { uint64_t r; __asm__ volatile ("mov %%cr4, %0":"=r"(r)); return r; }
