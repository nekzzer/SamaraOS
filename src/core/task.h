#ifndef SAMARA_TASK_H
#define SAMARA_TASK_H
#include "core/types.h"

#define MAX_TASKS  64
#define TASK_STACK_SZ 8192

typedef enum { T_FREE = 0, T_READY, T_BLOCKED, T_DEAD } task_state_t;

struct proc;

typedef struct task {
    uint64_t      rsp;
    uint8_t*      stack;
    task_state_t  state;
    int           id;
    char          name[32];
    uint32_t      ticks;       /* total ticks consumed */

    /* Address space + ring-3 plumbing. Kernel-only tasks leave these zero:
       cr3 == 0 means the shared kernel page directory, kstack_top == 0 means
       "no ring-3 code runs on this task, TSS esp0 need not change". */
    uint64_t      cr3;
    uint64_t      kstack_top;
    struct proc*  proc;
    uint8_t*      fpu;         /* 512-byte FXSAVE area, 16-byte aligned */
    uint64_t      fs_base;     /* user TLS, written to the MSR on switch */
    uint8_t*      fpu_alloc;
    uint32_t      wake_ms;     /* T_BLOCKED sleeper: ready again at this uptime */
    volatile int  on_cpu;      /* some cpu runs on this stack (or is still leaving it) */
    int           cpu;         /* the one that ran it last */
    int           ysw;         /* last yield went to somebody else */
} task_t;

/* Frame built by every entry path (isr stubs, syscall_entry, see boot/entry.S),
   low address first. vec/err are 0x100/0 for a syscall. Ring 0 -> ring 0
   interrupts push ss/rsp too in long mode, so it is always the full thing. */
typedef struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vec, err;
    uint64_t rip, cs, rflags, rsp, ss;
} regs_t;

void  task_init(void);
int   task_spawn(const char* name, void (*entry)(void));
int   task_spawn_sz(const char* name, void (*entry)(void), uint32_t sz);   /* bigger kernel stack */
/* Create a task from a caller-built kernel stack whose saved esp points at a
   regs_t frame (see proc.c). The task owns `stack` and frees it on reuse. */
int   task_spawn_frame(const char* name, uint8_t* stack, uint32_t stack_size,
                       uint64_t rsp, uint64_t cr3, struct proc* p);
void  task_exit(void);
void  task_yield(void);
void  task_sleep_ms(uint32_t ms);   /* kernel tasks: give up the CPU for a while */
task_t* task_current(void);
void  task_ready(task_t* t);        /* blocked -> ready, wakes a parked cpu */
void  cpu_wait(void);               /* sti; hlt without holding the big lock */
void  task_ap_start(int id);        /* an ap becomes its cpu's idle task */
void  task_dump(void (*emit)(const char*));
int   task_count(void);
task_t* task_at(int idx);

/* Switch the running task's page directory (0 = kernel) and load it now. */
void  task_set_cr3(uint64_t cr3);
uint64_t task_kernel_cr3(void);

/* Called from PIT ISR — installed by task_init. Not for direct use. */
void task_install_timer(void);

/* pushf; cli: returns the old flags for irq_restore. */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile ("sti" : : : "memory");
}

void wrmsr_fs(uint64_t v);

#endif
