#include "core/task.h"
#include "core/heap.h"
#include "core/string.h"
#include "boot/idt.h"
#include "boot/pic.h"
#include "boot/gdt.h"
#include "boot/paging.h"
#include "core/io.h"
#include "proc/proc.h"

extern void pit_tick_inc(void);
extern uint32_t pit_uptime_ms(void);
extern void pit_check_stack_guard(uint32_t cur_esp);

static task_t tasks[MAX_TASKS];
static int    n_tasks = 0;          /* high-water mark of used slots */
static int    cur = 0;
static int    started = 0;
static uint32_t kernel_cr3;
static uint32_t loaded_cr3;
static int    yield_switched;

/* CPU accounting for /proc/stat, in PIT ticks. */
volatile int  cpu_idle;             /* set while a task sits in hlt */
uint32_t      cpu_ticks_user, cpu_ticks_sys, cpu_ticks_idle, cpu_ctxt;

/* Clean x87 state (after FNINIT + default control word), copied into every
   new task so each one starts from a known FPU configuration. */
static uint8_t fpu_clean_raw[512 + 16];
static uint8_t* fpu_clean;
static uint8_t fpu_idle_raw[512 + 16];

static uint8_t* align16(uint8_t* p) { return (uint8_t*)(((uint32_t)p + 15u) & ~15u); }

task_t* task_current(void) { return &tasks[cur]; }
int     task_count(void)   { return n_tasks; }
task_t* task_at(int i)     { return (i >= 0 && i < n_tasks) ? &tasks[i] : NULL; }
uint32_t task_kernel_cr3(void) { return kernel_cr3; }

static void load_cr3(uint32_t cr3) {
    if (cr3 == loaded_cr3) return;
    loaded_cr3 = cr3;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void task_set_cr3(uint32_t cr3) {
    uint32_t f = irq_save();
    tasks[cur].cr3 = cr3;
    load_cr3(cr3 ? cr3 : kernel_cr3);
    irq_restore(f);
}

void task_exit(void) {
    cli();
    tasks[cur].state = T_DEAD;
    for (;;) task_yield();
}

/* --------- context layout on a task's stack ---------
   high -> [eip][cs][eflags]                  iret frame (same priv)
           [eax][ecx][edx][ebx][esp_d][ebp][esi][edi]   pusha
           [ds][es][fs][gs]
   low  ->                                            <- saved esp
*/
static uint32_t build_initial_stack(uint8_t* stack_top, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)stack_top;
    *--sp = (uint32_t)task_exit;  /* return address if entry returns */
    *--sp = 0x202;                /* eflags: IF=1 */
    *--sp = 0x08;                 /* cs */
    *--sp = (uint32_t)entry;      /* eip */
    for (int i = 0; i < 8; i++) *--sp = 0;   /* pusha block */
    *--sp = 0x10; /* ds */
    *--sp = 0x10; /* es */
    *--sp = 0x10; /* fs */
    *--sp = 0x10; /* gs */
    return (uint32_t)sp;
}

/* Find a free slot, reclaiming dead tasks' stacks. Called with IRQs off. */
static int alloc_slot(void) {
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t* t = &tasks[i];
        if (i == cur) continue;
        if (t->state == T_FREE || t->state == T_DEAD) {
            if (t->stack) kfree(t->stack);
            if (t->fpu_alloc) kfree(t->fpu_alloc);
            memset(t, 0, sizeof(*t));
            if (i >= n_tasks) n_tasks = i + 1;
            return i;
        }
    }
    return -1;
}

static int finish_spawn(int id, const char* name, uint8_t* stack, uint32_t esp,
                        uint32_t cr3, uint32_t kstack_top, struct proc* p,
                        const uint8_t* fpu_src) {
    task_t* t = &tasks[id];
    t->fpu_alloc = (uint8_t*)kmalloc(512 + 16);
    if (!t->fpu_alloc) return -1;
    t->fpu = align16(t->fpu_alloc);
    memcpy(t->fpu, fpu_src, 512);
    t->id = id;
    strncpy(t->name, name ? name : "task", 31);
    t->name[31] = 0;
    t->stack = stack;
    t->esp = esp;
    t->cr3 = cr3;
    t->kstack_top = kstack_top;
    t->proc = p;
    t->ticks = 0;
    t->state = T_READY;
    return id;
}

int task_spawn(const char* name, void (*entry)(void)) {
    uint8_t* stack = (uint8_t*)kmalloc(TASK_STACK_SZ);
    if (!stack) return -1;
    memset(stack, 0, TASK_STACK_SZ);
    uint32_t f = irq_save();
    int id = alloc_slot();
    int r = -1;
    if (id >= 0) {
        uint32_t esp = build_initial_stack(stack + TASK_STACK_SZ, entry);
        r = finish_spawn(id, name, stack, esp, 0, 0, NULL, fpu_clean);
    }
    irq_restore(f);
    if (r < 0) kfree(stack);
    return r;
}

int task_spawn_frame(const char* name, uint8_t* stack, uint32_t stack_size,
                     uint32_t esp, uint32_t cr3, struct proc* p) {
    uint32_t f = irq_save();
    int id = alloc_slot();
    int r = -1;
    if (id >= 0) {
        /* A forked child inherits the parent's FPU registers. */
        const uint8_t* src = fpu_clean;
        if (tasks[cur].proc && tasks[cur].fpu) {
            __asm__ volatile ("fxsave (%0)" : : "r"(tasks[cur].fpu) : "memory");
            src = tasks[cur].fpu;
        }
        r = finish_spawn(id, name, stack, esp, cr3, (uint32_t)stack + stack_size, p, src);
    }
    irq_restore(f);
    return r;
}

/* Choose next ready task starting from `cur+1` round-robin */
static int pick_next(void) {
    for (int i = 1; i <= n_tasks; i++) {
        int idx = (cur + i) % n_tasks;
        if (tasks[idx].state == T_READY) return idx;
    }
    return cur;     /* nobody else ready -> stay on current */
}

static uint32_t switch_to(int next, uint32_t saved_esp) {
    tasks[cur].esp = saved_esp;
    if (next == cur) return saved_esp;
    cpu_ctxt++;

    if (tasks[cur].fpu) __asm__ volatile ("fxsave (%0)" : : "r"(tasks[cur].fpu) : "memory");
    cur = next;
    task_t* t = &tasks[cur];
    if (t->fpu) __asm__ volatile ("fxrstor (%0)" : : "r"(t->fpu) : "memory");
    if (t->kstack_top) tss_set_esp0(t->kstack_top);
    load_cr3(t->cr3 ? t->cr3 : kernel_cr3);
    if (t->proc) {
        gdt_set_tls(proc_tls_base(t->proc));
        /* Preempted in ring 3 with a caught signal waiting: enter the handler. */
        regs_t* fr = (regs_t*)t->esp;
        if ((fr->cs & 3) == 3) {
            proc_check_alarm(t->proc, true);
            if (proc_signal_deliverable(t->proc)) proc_deliver_signal(fr, -1, 0);
        }
    }
    return t->esp;
}

static uint32_t slice = 0;

/* Called from naked ISR. Receives saved esp of the running task,
   returns esp of the task to switch to. */
uint32_t schedule(uint32_t saved_esp) {
    pit_tick_inc();
    pit_check_stack_guard(saved_esp);
    pic_send_eoi(0);

    if (!started || n_tasks == 0) return saved_esp;

    tasks[cur].ticks++;
    {
        uint32_t now = pit_uptime_ms();
        for (int i = 0; i < n_tasks; i++)
            if (tasks[i].state == T_BLOCKED && tasks[i].wake_ms &&
                (int32_t)(now - tasks[i].wake_ms) >= 0) { tasks[i].wake_ms = 0; tasks[i].state = T_READY; }
    }
    {
        bool user = (((regs_t*)saved_esp)->cs & 3) == 3;
        if (cpu_idle) cpu_ticks_idle++;
        else if (user) cpu_ticks_user++;
        else cpu_ticks_sys++;
        if (tasks[cur].proc && !cpu_idle) proc_account_tick(tasks[cur].proc, user);
    }

    /* PIT runs at 1 kHz for timing precision; keep a 10 ms time slice.
       A task that is no longer runnable gives up the CPU immediately. */
    if (tasks[cur].state == T_READY && ++slice < 10) return saved_esp;
    slice = 0;
    return switch_to(pick_next(), saved_esp);
}

/* int 0x81: voluntary switch. No EOI, no tick. */
uint32_t schedule_yield(uint32_t saved_esp) {
    if (!started) { yield_switched = 0; return saved_esp; }
    int next = pick_next();
    yield_switched = (next != cur);
    slice = 0;
    return switch_to(next, saved_esp);
}

void task_yield(void) {
    uint32_t f = irq_save();
    __asm__ volatile ("int $0x81" : : : "memory");
    /* Nobody else wanted the CPU: sleep until the next interrupt instead of
       spinning. A dead task must never fall through to here and return. */
    if (!yield_switched) {
        cpu_idle = 1;
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
        cpu_idle = 0;
    }
    irq_restore(f);
}

void task_sleep_ms(uint32_t ms) {
    uint32_t f = irq_save();
    if (cur != 0) {                          /* the kernel/UI task never blocks */
        tasks[cur].wake_ms = pit_uptime_ms() + (ms ? ms : 1);
        tasks[cur].state = T_BLOCKED;
        while (tasks[cur].state == T_BLOCKED) task_yield();   /* woken by the tick */
    } else {
        task_yield();
    }
    irq_restore(f);
}

/* IRQ0 stub - naked, full context save/switch */
__attribute__((naked))
void timer_isr(void) {
    __asm__ volatile (
        "pusha\n"
        "push %ds\n push %es\n push %fs\n push %gs\n"
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n mov %ax, %es\n mov %ax, %fs\n mov %ax, %gs\n"
        "push %esp\n"                  /* arg: current esp */
        "call schedule\n"
        "mov %eax, %esp\n"             /* switch stack to chosen task */
        "pop %gs\n pop %fs\n pop %es\n pop %ds\n"
        "popa\n"
        "iret\n"
    );
}

__attribute__((naked))
void yield_isr(void) {
    __asm__ volatile (
        "pusha\n"
        "push %ds\n push %es\n push %fs\n push %gs\n"
        "mov $0x10, %ax\n"
        "mov %ax, %ds\n mov %ax, %es\n mov %ax, %fs\n mov %ax, %gs\n"
        "push %esp\n"
        "call schedule_yield\n"
        "mov %eax, %esp\n"
        "pop %gs\n pop %fs\n pop %es\n pop %ds\n"
        "popa\n"
        "iret\n"
    );
}

void task_install_timer(void) {
    idt_set_gate(0x20, timer_isr, 0x08, 0x8E);
    idt_set_gate(0x81, yield_isr, 0x08, 0x8E);
}

void task_init(void) {
    n_tasks = 0;
    cur = 0;
    memset(tasks, 0, sizeof(tasks));
    kernel_cr3 = paging_cr3();
    loaded_cr3 = kernel_cr3;

    fpu_clean = align16(fpu_clean_raw);
    __asm__ volatile ("fninit");
    uint16_t cw = 0x037F;
    __asm__ volatile ("fldcw %0" : : "m"(cw));
    __asm__ volatile ("fxsave (%0)" : : "r"(fpu_clean) : "memory");
    cw = 0x027F;                     /* kernel keeps fpu_init's 53-bit mode */
    __asm__ volatile ("fldcw %0" : : "m"(cw));

    /* slot 0 = the running kernel/idle task; its esp gets filled on first IRQ */
    task_t* t = &tasks[0];
    t->id = 0;
    strncpy(t->name, "kernel", 31);
    t->state = T_READY;
    t->stack = NULL;     /* uses kernel stack already in use */
    t->fpu = align16(fpu_idle_raw);
    n_tasks = 1;
    started = 1;
    task_install_timer();
}

void task_dump(void (*emit)(const char*)) {
    for (int i = 0; i < n_tasks; i++) {
        const char* st = "?";
        switch (tasks[i].state) {
            case T_FREE: continue;
            case T_READY: st = "ready"; break;
            case T_BLOCKED: st = "block"; break;
            case T_DEAD: continue;
        }
        char tmp[16];
        emit("  ");
        itoa(tasks[i].id, tmp, 10); emit(tmp);
        emit("  "); emit(st);
        emit("  t="); itoa((int)tasks[i].ticks, tmp, 10); emit(tmp);
        emit("  "); emit(tasks[i].name);
        if (tasks[i].proc) {
            emit("  pid="); itoa(proc_pid(tasks[i].proc), tmp, 10); emit(tmp);
        }
        emit("\n");
    }
}
