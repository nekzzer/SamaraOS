#ifndef SAMARA_TASK_H
#define SAMARA_TASK_H
#include "core/types.h"

#define MAX_TASKS  64
#define TASK_STACK_SZ 8192

typedef enum { T_FREE = 0, T_READY, T_BLOCKED, T_DEAD } task_state_t;

struct proc;

typedef struct task {
    uint32_t      esp;
    uint8_t*      stack;
    task_state_t  state;
    int           id;
    char          name[32];
    uint32_t      ticks;       /* total ticks consumed */

    /* Address space + ring-3 plumbing. Kernel-only tasks leave these zero:
       cr3 == 0 means the shared kernel page directory, kstack_top == 0 means
       "no ring-3 code runs on this task, TSS esp0 need not change". */
    uint32_t      cr3;
    uint32_t      kstack_top;
    struct proc*  proc;
    uint8_t*      fpu;         /* 512-byte FXSAVE area, 16-byte aligned */
    uint8_t*      fpu_alloc;
    uint32_t      wake_ms;     /* T_BLOCKED sleeper: ready again at this uptime */
} task_t;

/* Register frame shared by the timer ISR, the yield ISR and the int 0x80
   syscall gate (low address first). The last two words only exist when the
   interrupt came from ring 3. */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

void  task_init(void);
int   task_spawn(const char* name, void (*entry)(void));
/* Create a task from a caller-built kernel stack whose saved esp points at a
   regs_t frame (see proc.c). The task owns `stack` and frees it on reuse. */
int   task_spawn_frame(const char* name, uint8_t* stack, uint32_t stack_size,
                       uint32_t esp, uint32_t cr3, struct proc* p);
void  task_exit(void);
void  task_yield(void);
void  task_sleep_ms(uint32_t ms);   /* kernel tasks: give up the CPU for a while */
task_t* task_current(void);
void  task_dump(void (*emit)(const char*));
int   task_count(void);
task_t* task_at(int idx);

/* Switch the running task's page directory (0 = kernel) and load it now. */
void  task_set_cr3(uint32_t cr3);
uint32_t task_kernel_cr3(void);

/* Called from PIT ISR — installed by task_init. Not for direct use. */
void task_install_timer(void);

/* pushf; cli — returns the old EFLAGS for irq_restore. */
static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile ("sti" : : : "memory");
}

#endif
