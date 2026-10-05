#include "core/task.h"
#include "core/heap.h"
#include "core/string.h"
#include "boot/idt.h"
#include "boot/pic.h"
#include "boot/apic.h"
#include "core/smp.h"
#include "core/prof.h"
#include "core/io.h"
#include "boot/gdt.h"
#include "boot/paging.h"
#include "core/io.h"
#include "proc/proc.h"

extern void pit_tick_inc(void);
extern uint32_t pit_uptime_ms(void);
extern void pit_check_stack_guard(uint64_t cur_rsp);

static task_t tasks[MAX_TASKS];
static task_t idle_t[MAX_CPUS];
static int    n_tasks = 0;          /* high-water mark of used slots */
static int    started = 0;
static uint64_t kernel_cr3;

uint32_t      cpu_ctxt;

/* Clean x87 state (after FNINIT + default control word), copied into every
   new task so each one starts from a known FPU configuration. */
static uint8_t fpu_clean_raw[512 + 16];
void wrmsr_fs(uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(0xC0000100), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static uint8_t* fpu_clean;
static uint8_t fpu_idle_raw[512 + 16];

static uint8_t* align16(uint8_t* p) { return (uint8_t*)(((uintptr_t)p + 15u) & ~15ul); }

task_t* task_current(void) { return this_cpu()->cur; }
int     task_count(void)   { return n_tasks; }
task_t* task_at(int i)     { return (i >= 0 && i < n_tasks) ? &tasks[i] : NULL; }
uint64_t task_kernel_cr3(void) { return kernel_cr3; }

static void load_cr3(struct cpu* c, uint64_t cr3) {
    if (cr3 == c->cr3) return;
    c->cr3 = cr3;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

void task_set_cr3(uint64_t cr3) {
    uint64_t f = irq_save();
    struct cpu* c = this_cpu();
    c->cur->cr3 = cr3;
    load_cr3(c, cr3 ? cr3 : kernel_cr3);
    irq_restore(f);
}

void task_exit(void) {
    cli();
    task_current()->state = T_DEAD;
    for (;;) task_yield();
}

void task_ready(task_t* t) {
    if (t->state != T_BLOCKED) return;
    t->wake_ms = 0;
    t->state = T_READY;
    kick_idle();
}

static int has_mwait;                   /* cpuid says monitor/mwait with irq break */
int idle_mode;                          /* 0 hlt, 1 mwait (for /proc) */
static int tickless = 1;

static void idle_cpuid(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & 8)) return;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(5), "c"(0));
    if ((c & 3) == 3) has_mwait = idle_mode = 1;      // ext + irq as break event
}

static void timer_periodic(struct cpu* c) {
    c->oneshot = 0;
    apic_timer_start();
}

/* how long this cpu can sleep without a tick, 0 = don't bother */
static uint32_t idle_ms(void) {
    uint32_t now = pit_uptime_ms();
    int32_t best = 50;
    for (int i = 0; i < n_tasks; i++) {
        task_t* t = &tasks[i];
        if (t->state == T_BLOCKED && t->wake_ms) {
            int32_t d = (int32_t)(t->wake_ms - now);
            if (d < best) best = d;
        }
    }
    uint32_t pt = proc_next_timer(now);
    if (pt < (uint32_t)best) best = pt;
    return best < 2 ? 0 : (uint32_t)best;
}

/* sti; hlt, but the other cpus get the kernel meanwhile. when an irq
   switched us away and later back we already hold it again */
void cpu_wait(void) {
    uint64_t fl = irq_save();
    struct cpu* c = this_cpu();
    c->in_idle = 1;
    if (c->bkl) bkl_drop(c);
    uint32_t ms = 0;
    // only the real idle task: a task that yields in a loop waits for the next tick
    uint32_t cap = c->cur->hint;
    c->cur->hint = 0;
    if (tickless && apic_on && tsc_khz && (c->cur == c->idle || cap || c->cur->state == T_BLOCKED) && !prof_on && (ms = idle_ms())) {
        if (cap && cap < ms) ms = cap;
        c->oneshot = 1;
        c->n_one++;
        c->idle_us = tsc_us();
        apic_timer_oneshot(ms * 1000);
    }
    if (has_mwait) {
        __asm__ volatile ("monitor" : : "a"(&c->in_idle), "c"(0), "d"(0));
        __asm__ volatile ("sti; mwait; cli" : : "a"(0), "c"(0) : "memory");
    } else
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    c = this_cpu();
    if (c->oneshot) {
        uint32_t el = (uint32_t)((tsc_us() - c->idle_us) / 1000);
        if (el > 1) c->t_idle += el - 1;
        timer_periodic(c);
    }
    c->in_idle = 0;
    if (!c->bkl) bkl_take(c);
    /* killed from another cpu while we waited: never go on with it */
    while (c->cur->state == T_DEAD) { task_yield(); c = this_cpu(); }
    irq_restore(fl);
}

/* a fresh kernel task: regs_t frame at the top of its stack, iretq into entry */
static uint64_t build_initial_stack(uint8_t* stack_top, void (*entry)(void)) {
    uint64_t* top = (uint64_t*)stack_top;
    top[-1] = (uint64_t)task_exit;               /* return address if entry returns */
    regs_t* r = (regs_t*)((uint8_t*)(top - 2) - sizeof(regs_t));
    memset(r, 0, sizeof(*r));
    r->rip = (uint64_t)entry;
    r->cs = 0x08;
    r->rflags = 0x202;
    r->rsp = (uint64_t)(top - 1);   // 16n+8 at entry like after a call
    r->ss = 0x10;
    return (uint64_t)r;
}

/* Find a free slot, reclaiming dead tasks' stacks. Called with IRQs off. */
static int alloc_slot(void) {
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t* t = &tasks[i];
        if (t->on_cpu) continue;
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

static int finish_spawn(int id, const char* name, uint8_t* stack, uint64_t rsp,
                        uint64_t cr3, uint64_t kstack_top, struct proc* p,
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
    t->rsp = rsp;
    t->cr3 = cr3;
    t->kstack_top = kstack_top;
    t->proc = p;
    t->ticks = 0;
    t->state = T_READY;
    kick_idle();
    return id;
}

int task_spawn_sz(const char* name, void (*entry)(void), uint32_t sz) {
    uint8_t* stack = (uint8_t*)kmalloc(sz);
    if (!stack) return -1;
    memset(stack, 0, sz);
    uint32_t f = irq_save();
    int id = alloc_slot();
    int r = -1;
    if (id >= 0) {
        uint64_t rsp = build_initial_stack(stack + sz, entry);
        r = finish_spawn(id, name, stack, rsp, 0, 0, NULL, fpu_clean);
    }
    irq_restore(f);
    if (r < 0) kfree(stack);
    return r;
}

int task_spawn(const char* name, void (*entry)(void)) { return task_spawn_sz(name, entry, TASK_STACK_SZ); }

int task_spawn_frame(const char* name, uint8_t* stack, uint32_t stack_size,
                     uint64_t rsp, uint64_t cr3, struct proc* p) {
    uint32_t f = irq_save();
    int id = alloc_slot();
    int r = -1;
    if (id >= 0) {
        /* A forked child inherits the parent's FPU registers. */
        const uint8_t* src = fpu_clean;
        task_t* me = task_current();
        if (me->proc && me->fpu) {
            __asm__ volatile ("fxsave64 (%0)" : : "r"(me->fpu) : "memory");
            src = me->fpu;
        }
        r = finish_spawn(id, name, stack, rsp, cr3, (uint64_t)stack + stack_size, p, src);
    }
    irq_restore(f);
    return r;
}

static const uint32_t nice_w[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548, 7620, 6100, 4904, 3906, 3121, 2501, 1991, 1586, 1277,
    1024, 820, 655, 526, 423, 335, 272, 215, 172, 137,
    110, 87, 70, 56, 45, 36, 29, 23, 18, 15 };

void task_set_sched(int id, int policy, int rtprio, int nice) {
    if (id < 0 || id >= MAX_TASKS) return;
    task_t* t = &tasks[id];
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    t->policy = policy;
    t->rtprio = (policy == 1 || policy == 2) ? rtprio : 0;
    t->weight = policy == 5 ? 3 : nice_w[nice + 20];
}

// is a better to run than b. rt first, then least virtual runtime
static int better(task_t* a, task_t* b) {
    if (a->rtprio != b->rtprio) return a->rtprio > b->rtprio;
    return a->vrt < b->vrt;
}

static uint64_t vmin;

/* best ready task, nobody else may be on it. ties go round-robin from where this cpu stopped */
static task_t* pick(struct cpu* c) {
    task_t* best = NULL;
    int bi = 0;
    for (int i = 1; i <= n_tasks; i++) {
        int idx = (c->rr + i) % n_tasks;
        task_t* t = &tasks[idx];
        if (t->state != T_READY || t->on_cpu) continue;
        if (!best || better(t, best)) { best = t; bi = idx; }
    }
    if (best) {
        c->rr = bi;
        // slept for ages: don't let it hog the cpu until it catches up
        if (best->vrt + 30000 < vmin) best->vrt = vmin - 30000;
        if (best->vrt > vmin) vmin = best->vrt;
    }
    return best;
}

static regs_t* switch_to(struct cpu* c, task_t* next, regs_t* saved) {
    task_t* prev = c->cur;
    prev->rsp = (uint64_t)saved;
    if (next == prev) return saved;
    cpu_ctxt++;

    if (prev->fpu) __asm__ volatile ("fxsave64 (%0)" : : "r"(prev->fpu) : "memory");
    if (c->prev) c->prev2 = c->prev;
    c->prev = prev;                 // on_cpu goes away in isr_leave, when we are off its stack
    c->cur = next;
    c->in_idle = 0;
    if (c->oneshot && next != c->idle) timer_periodic(c);
    next->on_cpu = 1;
    next->cpu = c->id;
    if (next->fpu) __asm__ volatile ("fxrstor64 (%0)" : : "r"(next->fpu) : "memory");
    if (next->kstack_top) tss_set_rsp0(next->kstack_top);
    load_cr3(c, next->cr3 ? next->cr3 : kernel_cr3);
    if (next->proc) wrmsr_fs(proc_tls_base(next->proc));
    if (next->proc && next->proc->dr[7]) { pt_dbg_load(next->proc); c->dr_on = 1; }
    else if (c->dr_on) { __asm__ volatile ("mov %0, %%dr7" : : "r"(0ul)); c->dr_on = 0; }
    return (regs_t*)next->rsp;
}

/* last look at the frame we are going back with. a dead task never gets to
   ring 3 again (it may have been killed from another cpu), a live one takes
   its alarms and signals now: it could have been running anywhere meanwhile */
static regs_t* finish(struct cpu* c, regs_t* f) {
    if ((f->cs & 3) != 3) return f;
    if (c->cur->state == T_DEAD) {
        task_t* n = pick(c);
        f = switch_to(c, n ? n : c->idle, f);
        if ((f->cs & 3) != 3) return f;
    }
    task_t* t = c->cur;
    if (t->proc) {
        proc_t* p = t->proc;
        proc_check_alarm(p, true);
        p->pt_isr = true;
        if (proc_signal_deliverable(p)) proc_deliver_signal(f, -1, 0);
        p->pt_isr = false;
        if (p->pt_state == 1 && p->pt_isr_stop) {        /* ptrace-stop: park, the tracer wakes us */
            t->wake_ms = pit_uptime_ms() + 100;
            t->state = T_BLOCKED;
            task_t* n = pick(c);
            return finish(c, switch_to(c, n ? n : c->idle, f));
        }
    }
    return f;
}

regs_t* task_reap(regs_t* f) { return finish(this_cpu(), f); }

/* lapic timer, every cpu */
static regs_t* schedule(regs_t* saved) {
    struct cpu* c = this_cpu();
    c->n_tmr++;
    if (c->oneshot) {
        uint32_t el = (uint32_t)((tsc_us() - c->idle_us) / 1000);
        if (el > 1) c->t_idle += el - 1;
        timer_periodic(c);
    }
    pic_send_eoi(0);
    if (!c->id) {
        pit_tick_inc();
        pit_check_stack_guard((uint64_t)saved);
    }
    if (!started) return saved;

    task_t* cur = c->cur;
    cur->ticks++;
    if (cur != c->idle) cur->vrt += 1048576 / (cur->weight ? cur->weight : 1024);
    {
        static uint32_t wake_done;
        uint32_t now = pit_uptime_ms();
        if (now != wake_done) {
            wake_done = now;
            int woke = 0;
            for (int i = 0; i < n_tasks; i++)
                if (tasks[i].state == T_BLOCKED && tasks[i].wake_ms &&
                    (int32_t)(now - tasks[i].wake_ms) >= 0) { tasks[i].wake_ms = 0; tasks[i].state = T_READY; woke++; }
            if (woke) kick_idle();
            proc_timers_tick(now);
        }
    }
    {
        bool user = (saved->cs & 3) == 3;
        if (c->in_idle || cur == c->idle) c->t_idle++;
        else if (user) c->t_user++;
        else c->t_sys++;
        if (cur->proc && !c->in_idle) proc_account_tick(cur->proc, user);
        if (prof_on) {
            if (ncpu < 2) prof_sample(saved->rip, user, c->in_idle || cur == c->idle, cur->proc ? cur->proc->name : "?");
            else for (int i = 0; i < ncpu; i++) if (i != c->id) lapic_ipi_raw(cpus[i].apic_id, 0x4400);
        }
    }

    /* 1 kHz tick, 10 ms time slice. A task that is no longer runnable gives
       up the CPU immediately. */
    task_t* next = cur;
    if (cur == c->idle) {
        task_t* n = pick(c);
        if (n) next = n;
    } else if (cur->state != T_READY || ++c->slice >= 10) {
        c->slice = 0;
        task_t* n = pick(c);
        if (n && cur->state == T_READY) {
            bool keep = cur->policy == 1 ? n->rtprio <= cur->rtprio :
                        !better(n, cur) && !(cur->policy == 2 && n->rtprio == cur->rtprio);
            if (keep) n = NULL;
        }
        if (n) next = n;
        else if (cur->state != T_READY) next = c->idle;
    }
    return finish(c, switch_to(c, next, saved));
}

/* int 0x81: voluntary switch. No EOI, no tick. */
static regs_t* schedule_yield(regs_t* saved) {
    struct cpu* c = this_cpu();
    task_t* cur = c->cur;
    if (!started) { cur->ysw = 0; return saved; }
    task_t* next = pick(c);
    if (!next) next = (cur->state == T_READY || cur == c->idle) ? cur : c->idle;
    cur->ysw = next != cur;
    c->slice = 0;
    return finish(c, switch_to(c, next, saved));
}

/* somebody made work for an idle cpu */
static regs_t* schedule_kick(regs_t* saved) {
    struct cpu* c = this_cpu();
    lapic_eoi();
    if (!started) return saved;
    task_t* next = c->cur;
    if (next == c->idle) {
        task_t* n = pick(c);
        if (n) next = n;
    }
    return finish(c, switch_to(c, next, saved));
}

void task_yield(void) {
    uint32_t f = irq_save();
    __asm__ volatile ("int $0x81" : : : "memory");
    /* Nobody else wanted the CPU: sleep until the next interrupt instead of
       spinning. A dead task must never fall through to here and return. */
    if (!task_current()->ysw) cpu_wait();
    else if (this_cpu()->bkl) bkl_yield(this_cpu());
    irq_restore(f);
}

void cpu_wait_to(uint32_t ms) {
    uint64_t f = irq_save();
    task_current()->hint = ms;
    cpu_wait();
    irq_restore(f);
}

void task_yield_until(uint32_t t) {
    uint64_t f = irq_save();
    task_t* me = task_current();
    int32_t d = (int32_t)(t - pit_uptime_ms());
    if (d > 1) me->hint = d;
    task_yield();
    me->hint = 0;
    irq_restore(f);
}

void task_yield_fast(void) {
    uint64_t f = irq_save();
    __asm__ volatile ("int $0x81" : : : "memory");
    irq_restore(f);
}

void task_sleep_ms(uint32_t ms) {
    uint32_t f = irq_save();
    task_t* t = task_current();
    if (t->id != 0) {                        /* the kernel/UI task never blocks */
        t->wake_ms = pit_uptime_ms() + (ms ? ms : 1);
        t->state = T_BLOCKED;
        while (t->state == T_BLOCKED) task_yield();   /* woken by the tick */
    } else {
        task_yield();
    }
    irq_restore(f);
}

// poll/pipe/pty/socket waiters sleep here instead of 1ms naps or yield spins.
// the ms is only for things nobody calls io_wake for (nic rx, timers, input irqs)
void task_wait_io(uint32_t ms) {
    uint32_t f = irq_save();
    task_t* t = task_current();
    if (t->id != 0) {
        t->io_wait = 1;
        t->wake_ms = pit_uptime_ms() + (ms ? ms : 1);
        t->state = T_BLOCKED;
        while (t->state == T_BLOCKED) task_yield();
        t->io_wait = 0;
    } else {
        task_yield();
    }
    irq_restore(f);
}

void io_wake(void) {
    uint32_t f = irq_save();
    int woke = 0;
    for (int i = 0; i < n_tasks; i++)
        if (tasks[i].io_wait && tasks[i].state == T_BLOCKED) { tasks[i].wake_ms = 0; tasks[i].state = T_READY; woke++; }
    if (woke) kick_idle();
    irq_restore(f);
}

void task_install_timer(void) {
    if (apic_on) idt_set_sched(VEC_TIMER, schedule);
    else idt_set_sched(0x20, schedule);
    idt_set_sched(0x81, schedule_yield);
    idt_set_sched(VEC_KICK, schedule_kick);
}

static void idle_main(void) { for (;;) task_yield(); }

void task_init(void) {
    n_tasks = 0;
    memset(tasks, 0, sizeof(tasks));
    kernel_cr3 = paging_cr3();
    cpus[0].cr3 = kernel_cr3;
    idle_cpuid();
    extern const char* kernel_cmdline(void);
    if (strstr(kernel_cmdline(), "notickless")) tickless = 0;
    if (strstr(kernel_cmdline(), "nomwait")) has_mwait = idle_mode = 0;

    fpu_clean = align16(fpu_clean_raw);
    __asm__ volatile ("fninit");
    uint16_t cw = 0x037F;
    __asm__ volatile ("fldcw %0" : : "m"(cw));
    __asm__ volatile ("fxsave64 (%0)" : : "r"(fpu_clean) : "memory");
    cw = 0x027F;                     /* kernel keeps fpu_init's 53-bit mode */
    __asm__ volatile ("fldcw %0" : : "m"(cw));

    /* slot 0 = the running kernel/idle task; its esp gets filled on first IRQ */
    task_t* t = &tasks[0];
    t->id = 0;
    strncpy(t->name, "kernel", 31);
    t->state = T_READY;
    t->stack = NULL;     /* uses kernel stack already in use */
    t->fpu = align16(fpu_idle_raw);
    t->on_cpu = 1;
    cpus[0].cur = t;
    n_tasks = 1;

    /* the bsp needs somewhere to park when the boot task is busy elsewhere */
    uint8_t* st = (uint8_t*)kmalloc(16384);
    task_t* id0 = &idle_t[0];
    strncpy(id0->name, "idle", 31);
    id0->id = MAX_TASKS;
    id0->state = T_READY;
    id0->fpu_alloc = (uint8_t*)kmalloc(512 + 16);
    id0->fpu = align16(id0->fpu_alloc);
    memcpy(id0->fpu, fpu_clean, 512);
    id0->rsp = build_initial_stack(st + 16384, idle_main);
    cpus[0].idle = id0;
    started = 1;
    task_install_timer();
}

void task_ap_start(int id) {
    struct cpu* c = &cpus[id];
    task_t* t = &idle_t[id];
    strncpy(t->name, "idle", 31);
    t->id = MAX_TASKS + id;
    t->state = T_READY;
    t->on_cpu = 1;
    t->cpu = id;
    t->fpu_alloc = (uint8_t*)kmalloc(512 + 16);
    t->fpu = align16(t->fpu_alloc);
    memcpy(t->fpu, fpu_clean, 512);
    c->cur = c->idle = t;
    apic_timer_start();
    c->online = 1;
    idle_main();
}

void syscall_dispatch(regs_t* r);
int64_t syscall_nobkl(regs_t* r);
extern uint8_t nobkl_tab[512];
void syscall_enter(regs_t* r) {
    struct cpu* c = this_cpu();
    uint64_t nr = r->rax;
    if (nr < 512 && nobkl_tab[nr] && c->cur->state != T_DEAD) {
        task_t* t = c->cur;
        t->nobkl = 1;
        int64_t ret = syscall_nobkl(r);
        t->nobkl = 0;
        if (ret != NB_SLOW) {
            r->rax = (uint64_t)ret;
            if (t->proc && proc_signal_deliverable(t->proc)) {
                if (!this_cpu()->bkl) bkl_take(this_cpu());
                proc_deliver_signal(r, (int)nr, (int32_t)ret);
            }
            return;
        }
        r->rax = nr;
    }
    c = this_cpu();
    if (!c->bkl) bkl_take(c);
    /* killed from another cpu while it was in ring 3: it never does anything again */
    while (c->cur->state == T_DEAD) { task_yield(); c = this_cpu(); }
    syscall_dispatch(r);
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
