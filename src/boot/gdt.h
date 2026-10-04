#ifndef SAMARA_GDT_H
#define SAMARA_GDT_H
#include "core/types.h"

void gdt_init(void);

/* ring0 stack for the next ring3 -> ring0 transition (tss.rsp0 and the
   per-cpu slot syscall_entry loads) */
void tss_set_rsp0(uint64_t rsp0);

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UDATA 0x23
#define GDT_UCODE 0x2B

/* per-cpu block, GS base in kernel mode. syscall_entry knows the offsets */
struct cpu {
    uint64_t kstack;       /* 0 */
    uint64_t user_rsp;     /* 8 */
    int      id;
};
extern struct cpu cpu0;

#endif
