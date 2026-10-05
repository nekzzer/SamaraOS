#ifndef SAMARA_GDT_H
#define SAMARA_GDT_H
#include "core/types.h"

void gdt_init(void);                    /* bsp */
void gdt_init_ap(int id);               /* gdt, tss, gs, syscall msrs of this cpu */

/* ring0 stack for the next ring3 -> ring0 transition (tss.rsp0 and the
   per-cpu slot syscall_entry loads) */
void tss_set_rsp0(uint64_t rsp0);

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA 0x23
#define GDT_UCODE 0x2B

struct task;

/* per-cpu block, GS base in kernel mode. syscall_entry knows the first offsets */
struct cpu {
    uint64_t kstack;       /* 0 */
    uint64_t user_rsp;     /* 8 */
    struct cpu* self;      /* 16 */
    int      id;
    int      apic_id;
    volatile int online;
    int      bkl;          /* this cpu holds the big lock */
    int      in_idle;      /* parked in hlt */
    struct task* cur;
    struct task* idle;
    struct task* prev;     /* switched away from, on_cpu still set */
    struct task* prev2;    /* finish() switched a second time on the way out */
    int      rr;
    uint32_t slice;
    uint64_t cr3;          /* what is loaded */
    volatile uint32_t tlb_req;
    volatile uint32_t unload;   /* leave the address space you have, it is going away */
    uint32_t t_user, t_sys, t_idle;
    int dr_on;
};
extern struct cpu cpus[MAX_CPUS];
extern int ncpu;                        /* started */

static inline struct cpu* this_cpu(void) {
    struct cpu* c;
    __asm__ volatile ("mov %%gs:16, %0" : "=r"(c));
    return c;
}

#endif
