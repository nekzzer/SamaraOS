#include "core/types.h"
#include "core/task.h"
#include "core/string.h"
#include "boot/gdt.h"
#include "core/io.h"
#include "proc/proc.h"
#include "gfx/gfx.h"
#include "drivers/vga.h"

extern const unsigned long ksym_n, ksym_addr[];
extern const unsigned int ksym_size[], ksym_off[];
extern const char ksym_names[];
extern int snprintf(char* buf, size_t n, const char* fmt, ...);

static void com(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

const char* ksym_lookup(uint64_t a, uint64_t* off) {
    if (!ksym_n || a < ksym_addr[0] || a >= ksym_addr[ksym_n - 1] + ksym_size[ksym_n - 1] + 4096) return NULL;
    unsigned long lo = 0, hi = ksym_n - 1;
    while (lo < hi) {
        unsigned long m = (lo + hi + 1) / 2;
        if (ksym_addr[m] <= a) lo = m; else hi = m - 1;
    }
    *off = a - ksym_addr[lo];
    return ksym_names + ksym_off[lo];
}

static bool is_call_ret(uint64_t a) {
    const uint8_t* p = (const uint8_t*)a;
    return p[-5] == 0xe8 || p[-2] == 0xff || p[-3] == 0xff || p[-6] == 0xff || p[-7] == 0xff;   // call rel32 / call *r/m
}

static int line_y = 8;
static void out(const char* s, bool fb) {
    com(s); com("\r\n");
    vga_printf("%s\n", s);
    if (fb && gfx_ready() && line_y < gfx_h() - 16) { gfx_string(10, line_y, s, RGB(0xff, 0xff, 0xff), RGB(0x80, 0x10, 0x10), true); line_y += 16; }
}

static volatile int dumping;

// kernel fault: regs, current task, backtrace guessed from the stack (no frame pointers here)
void kpanic_dump(const char* what, regs_t* r) {
    __asm__ volatile ("cli");
    while (__atomic_exchange_n(&dumping, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("pause");
    char b[160];
    if (gfx_ready()) { gfx_target_front(); gfx_rect_fill(0, 0, gfx_w(), 8 + 16 * 24, RGB(0x80, 0x10, 0x10)); }
    uint64_t cr2, cr3, o;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    struct cpu* c = this_cpu();
    snprintf(b, sizeof b, "[PANIC] %s vec=%ld err=%lx cpu=%d", what, r->vec, r->err, c ? c->id : -1);
    out(b, true);
    const char* s = ksym_lookup(r->rip, &o);
    snprintf(b, sizeof b, "rip=%016lx %s+0x%lx cs=%lx rfl=%lx", r->rip, s ? s : "?", s ? o : 0, r->cs, r->rflags);
    out(b, true);
    task_t* t = c ? c->cur : NULL;
    snprintf(b, sizeof b, "task %d \"%s\" pid %d bkl=%d cr2=%016lx cr3=%lx", t ? t->id : -1, t ? t->name : "-",
             t && t->proc ? ((proc_t*)t->proc)->pid : -1, c ? c->bkl : -1, cr2, cr3);
    out(b, false);
    snprintf(b, sizeof b, "rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx", r->rax, r->rbx, r->rcx, r->rdx);
    out(b, true);
    snprintf(b, sizeof b, "rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx", r->rsi, r->rdi, r->rbp, r->rsp);
    out(b, true);
    snprintf(b, sizeof b, "r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx", r->r8, r->r9, r->r10, r->r11);
    out(b, false);
    snprintf(b, sizeof b, "r12=%016lx r13=%016lx r14=%016lx r15=%016lx", r->r12, r->r13, r->r14, r->r15);
    out(b, false);
    out("backtrace:", true);
    uint64_t* sp = (uint64_t*)r->rsp;
    uint64_t end = (r->rsp | 0xfff) + 1 + 0x3000;
    if (t && t->kstack_top && r->rsp < t->kstack_top && r->rsp + 0x8000 > t->kstack_top) end = t->kstack_top;
    int n = 0;
    for (; (uint64_t)sp < end && n < 24; sp++) {
        uint64_t v = *sp;
        if (v < 0xffffffff80100000ul || v > 0xffffffff90000000ul) continue;
        s = ksym_lookup(v, &o);
        if (!s || !is_call_ret(v)) continue;
        snprintf(b, sizeof b, " [%016lx] %s+0x%lx", v, s, o);
        out(b, n < 12);
        n++;
    }
    out("halted", true);
}
