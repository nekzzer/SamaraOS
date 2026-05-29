#include "task.h"
#include "heap.h"
#include "string.h"
#include "idt.h"
#include "pic.h"
#include "io.h"

extern void pit_tick_inc(void);

static task_t tasks[MAX_TASKS];
static int    n_tasks = 0;
static int    cur = 0;
static int    started = 0;

task_t* task_current(void) { return &tasks[cur]; }
int     task_count(void)   { return n_tasks; }
task_t* task_at(int i)     { return (i >= 0 && i < n_tasks) ? &tasks[i] : NULL; }

void task_exit(void) {
    cli();
    tasks[cur].state = T_DEAD;
    sti();
    while (1) { __asm__ volatile ("hlt"); }
}

/* --------- context layout on a task's stack ---------
   high -> [eip][cs][eflags]                  iret frame (same priv)
           [eax][ecx][edx][ebx][esp_d][ebp][esi][edi]   pusha
           [ds][es][fs][gs]
   low  ->                                            <- saved esp
*/
static uint32_t build_initial_stack(uint8_t* stack_top, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)stack_top;
    *--sp = 0x202;                /* eflags: IF=1 */
    *--sp = 0x08;                 /* cs */
    *--sp = (uint32_t)entry;      /* eip */
    /* pusha order pushed: eax, ecx, edx, ebx, esp, ebp, esi, edi
       popa pops in reverse, so on stack from low->high: edi,esi,ebp,esp,ebx,edx,ecx,eax */
    *--sp = 0;  /* eax */
    *--sp = 0;  /* ecx */
    *--sp = 0;  /* edx */
    *--sp = 0;  /* ebx */
    *--sp = 0;  /* esp_dummy */
    *--sp = 0;  /* ebp */
    *--sp = 0;  /* esi */
    *--sp = 0;  /* edi */
    *--sp = 0x10; /* ds */
    *--sp = 0x10; /* es */
    *--sp = 0x10; /* fs */
    *--sp = 0x10; /* gs */
    return (uint32_t)sp;
}

int task_spawn(const char* name, void (*entry)(void)) {
    if (n_tasks >= MAX_TASKS) return -1;
    int id = n_tasks++;
    task_t* t = &tasks[id];
    t->id = id;
    strncpy(t->name, name ? name : "task", 31);
    t->name[31] = 0;
    t->stack = (uint8_t*)kmalloc(TASK_STACK_SZ);
    if (!t->stack) { n_tasks--; return -1; }
    memset(t->stack, 0, TASK_STACK_SZ);

    /* wrap entry to call task_exit on return */
    extern void task_trampoline(void);
    /* push entry pointer for trampoline to read */
    uint8_t* top = t->stack + TASK_STACK_SZ;
    /* place entry pointer at top-4 so trampoline can find it via its own stack */
    /* Simpler: set EIP directly to entry; user code returning will fault.
       Provide a small wrapper: caller-side we wrap entry via a table. */
    t->esp = build_initial_stack(top, entry);
    t->state = T_READY;
    t->ticks = 0;
    return id;
}

/* Choose next ready task starting from `cur+1` round-robin */
static int pick_next(void) {
    if (n_tasks == 0) return 0;
    for (int i = 1; i <= n_tasks; i++) {
        int idx = (cur + i) % n_tasks;
        if (tasks[idx].state == T_READY) return idx;
    }
    return cur;     /* nobody else ready -> stay on current */
}

/* Called from naked ISR. Receives saved esp of the running task,
   returns esp of the task to switch to. */
uint32_t schedule(uint32_t saved_esp) {
    pit_tick_inc();
    pic_send_eoi(0);

    if (!started || n_tasks == 0) return saved_esp;

    tasks[cur].esp = saved_esp;
    tasks[cur].ticks++;

    int next = pick_next();
    cur = next;
    return tasks[cur].esp;
}

void task_yield(void) {
    /* trigger soft reschedule: just int $0x20 isn't safe (PIC EOI logic),
       so disable interrupts briefly and call hlt to wait for next tick. */
    __asm__ volatile ("sti; hlt");
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

void task_install_timer(void) {
    idt_set_gate(0x20, timer_isr, 0x08, 0x8E);
}

void task_init(void) {
    n_tasks = 0;
    cur = 0;
    memset(tasks, 0, sizeof(tasks));
    /* slot 0 = the running kernel/idle task; its esp gets filled on first IRQ */
    task_t* t = &tasks[0];
    t->id = 0;
    strncpy(t->name, "idle", 31);
    t->state = T_READY;
    t->stack = NULL;     /* uses kernel stack already in use */
    n_tasks = 1;
    started = 1;
    task_install_timer();
}

void task_dump(void (*emit)(const char*)) {
    char buf[64];
    for (int i = 0; i < n_tasks; i++) {
        const char* st = "?";
        switch (tasks[i].state) {
            case T_FREE: st = "free"; break;
            case T_READY: st = "ready"; break;
            case T_BLOCKED: st = "block"; break;
            case T_DEAD: st = "dead"; break;
        }
        char tmp[16];
        emit("  ");
        itoa(tasks[i].id, tmp, 10); emit(tmp);
        emit("  "); emit(st);
        emit("  t="); itoa((int)tasks[i].ticks, tmp, 10); emit(tmp);
        emit("  "); emit(tasks[i].name);
        emit("\n");
        (void)buf;
    }
}
