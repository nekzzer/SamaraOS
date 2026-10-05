#include "boot/gdt.h"
#include "core/string.h"
#include "core/types.h"

struct tss64 {
    uint32_t res0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t res1;
    uint64_t ist[7];
    uint64_t res2;
    uint16_t res3, iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 0 null, 1 kcode, 2 kdata, 3 ucode32 (only for STAR), 4 udata, 5 ucode64, 6-7 tss */
static uint64_t gdt[MAX_CPUS][8];
static struct gdt_ptr gdtp[MAX_CPUS];
static struct tss64 tss[MAX_CPUS];
struct cpu cpus[MAX_CPUS];
int ncpu = 1;

extern uint8_t boot_stack[];
#define BOOT_STACK_TOP ((uint64_t)boot_stack + (4u * 1024u * 1024u))

static uint8_t df_stack[MAX_CPUS][8192] __attribute__((aligned(16)));
static uint8_t nmi_stack[MAX_CPUS][8192] __attribute__((aligned(16)));

void tss_set_rsp0(uint64_t rsp0) {
    struct cpu* c = this_cpu();
    tss[c->id].rsp0 = rsp0;
    c->kstack = rsp0;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void cpu_gdt(int id, uint64_t stack) {
    uint64_t* g = gdt[id];
    g[0] = 0;
    g[1] = 0x00AF9A000000FFFFull;
    g[2] = 0x00CF92000000FFFFull;
    g[3] = 0x00CFFA000000FFFFull;
    g[4] = 0x00CFF2000000FFFFull;
    g[5] = 0x00AFFA000000FFFFull;

    struct tss64* t = &tss[id];
    memset(t, 0, sizeof(*t));
    t->rsp0 = stack;
    t->ist[0] = (uint64_t)df_stack[id] + sizeof(df_stack[id]);
    t->ist[1] = (uint64_t)nmi_stack[id] + sizeof(nmi_stack[id]);
    t->iomap_base = sizeof(*t);
    uint64_t base = (uint64_t)t, lim = sizeof(*t) - 1;
    g[6] = (lim & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((lim >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g[7] = base >> 32;

    gdtp[id].limit = sizeof(gdt[id]) - 1;
    gdtp[id].base = (uint64_t)g;
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x08\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov $0x30, %%ax\n\t"
        "ltr %%ax\n\t"
        : : "r"(&gdtp[id]) : "rax", "memory"
    );

    /* gs base = per-cpu block while in the kernel (swapgs at every entry from ring 3) */
    struct cpu* c = &cpus[id];
    c->self = c;
    c->id = id;
    c->kstack = stack;
    wrmsr(0xC0000101, (uint64_t)c);           /* GS_BASE */
    wrmsr(0xC0000102, 0);                     /* KERNEL_GS_BASE: user's */

    extern void syscall_entry(void);
    wrmsr(0xC0000081, ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32));   /* STAR */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);                          /* LSTAR */
    wrmsr(0xC0000084, 0x700);                                            /* SFMASK: TF, IF, DF */
}

void gdt_init(void) {
    cpu_gdt(0, BOOT_STACK_TOP);
    cpus[0].bkl = 1;                          // boot is "in the kernel"
    cpus[0].online = 1;
}

void gdt_init_ap(int id) { cpu_gdt(id, 0); }
