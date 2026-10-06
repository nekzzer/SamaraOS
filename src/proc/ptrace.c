/* ptrace, x86_64 Linux style. A stopped tracee sits either in a wait loop
   inside its own syscall/fault (pt_stop from syscall context), or, when the
   scheduler wants to deliver a signal on the way back to ring 3, it is just
   parked as T_BLOCKED with its frame saved ("isr style", see finish() in
   task.c). Either way pt_regs points at the user frame. */

#include "proc/proc.h"
#include "core/string.h"
#include "core/vmm.h"
#include "core/clock.h"
#include "boot/pit.h"
#include "boot/gdt.h"
#include "boot/idt.h"

#define EPERM  1
#define ESRCH  3
#define EIO    5
#define EFAULT 14
#define EBUSY  16
#define EINVAL 22

#define O_SYSGOOD 1
#define O_FORK    2
#define O_VFORK   4
#define O_CLONE   8
#define O_EXEC    0x10
#define O_EXIT    0x40
#define O_EXITKILL 0x100000
#define O_ALL     0x1000ff

static bool uptr(uint64_t a, uint64_t n) { return a >= USER_BASE && a + n <= USER_TOP && a + n >= a; }

/* stopping */

static void pt_si_fill(proc_t* p, int sig, int code) {
    uint32_t* si = (uint32_t*)p->pt_si;
    memset(si, 0, 128);
    si[0] = sig; si[2] = code;
    p->pt_sival = true;
}

int pt_stop(proc_t* p, regs_t* r, int sig, int event, int kind) {
    p->pt_regs = r;
    p->pt_stopsig = sig;
    p->pt_event = event;
    p->pt_kind = kind;
    p->pt_job = kind == 3;
    p->pt_rep = false;
    p->pt_state = 1;
    if (kind != 1) p->pt_sival = false;
    proc_t* tr = proc_by_pid(p->tracer);
    if (tr) { tr->sig_pending |= SIGBIT(17); ready_task_of(tr); }      // gdb waits for sigchld in its event loop
    if (p->pt_isr) {
        p->pt_isr_stop = true;
        return 0;
    }
    task_t* t = task_current();
    while (p->pt_state == 1) {
        t->wake_ms = pit_uptime_ms() + 100;
        t->state = T_BLOCKED;
        task_yield();
    }
    p->pt_state = 0;
    return 1;
}

void pt_syscall_stop(proc_t* p, regs_t* r, bool exit) {
    p->pt_entry = !exit;
    pt_si_fill(p, 5, 5 | (p->pt_opts & O_SYSGOOD ? 0x80 : 0));
    pt_stop(p, r, 5 | (p->pt_opts & O_SYSGOOD ? 0x80 : 0), 0, 0);
    p->pt_entry = false;
}

void pt_event(proc_t* p, regs_t* r, int event, uint64_t msg) {
    p->pt_msg = msg;
    pt_si_fill(p, 5, (event << 8) | 5);
    pt_stop(p, r, 5, event, 0);
}

static regs_t* user_frame(proc_t* p) {
    task_t* t = task_at(p->task);
    return (regs_t*)(t->kstack_top - sizeof(regs_t));
}

void pt_exit_event(proc_t* p, int status) {
    if (!p->tracer || !(p->pt_opts & O_EXIT)) return;
    p->pt_orig = (uint64_t)-1;
    pt_event(p, user_frame(p), 6, (uint64_t)status);
}

void pt_exec(proc_t* p, regs_t* r) {
    if (!p->tracer) return;
    if (p->pt_opts & O_EXEC) pt_event(p, r, 4, 0);
    else if (!p->pt_seize) p->sig_pending |= SIGBIT(5);     /* plain SIGTRAP after exec */
}

/* a new child of a traced task: attached right away when the option asks for it */
void pt_child(proc_t* p, proc_t* c, int kind) {
    int want = kind == 1 ? O_FORK : kind == 2 ? O_VFORK : O_CLONE;
    if (!p->tracer || !(p->pt_opts & want)) return;
    c->tracer = p->tracer;
    c->pt_opts = p->pt_opts;
    c->pt_seize = p->pt_seize;
    if (c->pt_seize) c->pt_intr = true;
    else c->sig_pending |= SIGBIT(19);
}

/* tracer died or detached everything */
void pt_release(int tgid) {
    for (int i = 0; i < proc_count(); i++) {
        proc_t* q = proc_at(i);
        if (!q || q->tracer != tgid) continue;
        q->tracer = 0;
        q->pt_sys = false;
        q->pt_intr = false;
        if (q->pt_regs && q->pt_tf) { q->pt_regs->rflags &= ~0x100ul; q->pt_tf = false; }
        if (q->pt_opts & O_EXITKILL) proc_send_signal(q, 9);
        q->pt_opts = 0;
        if (q->state == P_ALIVE && q->pt_state == 1 && !q->pt_job) {
            q->pt_inj = 0;
            q->pt_state = 2;
            ready_task_of(q);
        }
    }
}

/* waitpid: a tracee that stopped and nobody told the tracer yet, or a
   child that got SIGSTOP and the parent asks for WUNTRACED */
bool pt_wait_report(proc_t* c, proc_t* me, int options, int* st) {
    if (c->state != P_ALIVE || c->pt_state != 1 || c->pt_rep) return false;
    bool tr = c->tracer == me->tgid && !c->pt_job;
    bool jb = c->pt_job && c->ppid == me->tgid && (options & 2);
    if (!tr && !jb) return false;
    c->pt_rep = true;
    *st = ((c->pt_event << 8 | c->pt_stopsig) << 8) | 0x7f;
    return true;
}

void pt_cont_group(proc_t* p) {
    for (int i = 0; i < proc_count(); i++) {
        proc_t* q = proc_at(i);
        if (!q || q->tgid != p->tgid || !q->pt_job || q->pt_state != 1) continue;
        if (q->tracer) {                    /* seized while group-stopped: tracer restarts it, not us */
            q->pt_job = false;
            q->pt_rep = false;
            q->pt_stopsig = 5;
            q->pt_event = 128;
            continue;
        }
        q->pt_inj = 0;
        q->pt_state = 2;
        ready_task_of(q);
    }
}

/* registers */

/* struct user_regs_struct, 27 words */
static void get_regs(proc_t* p, uint64_t* u) {
    regs_t* r = p->pt_regs;
    u[0] = r->r15; u[1] = r->r14; u[2] = r->r13; u[3] = r->r12; u[4] = r->rbp; u[5] = r->rbx;
    u[6] = r->r11; u[7] = r->r10; u[8] = r->r9; u[9] = r->r8;
    u[10] = p->pt_entry ? (uint64_t)-38 : r->rax;
    u[11] = r->rcx; u[12] = r->rdx; u[13] = r->rsi; u[14] = r->rdi;
    u[15] = p->pt_entry ? r->rax : p->pt_orig;
    u[16] = r->rip; u[17] = 0x33; u[18] = r->rflags; u[19] = r->rsp; u[20] = 0x2b;   // linux selectors, gdb checks cs
    u[21] = p->tls_base; u[22] = p->gs_base;
    u[23] = u[24] = u[25] = u[26] = 0;
    if (p->pt_tf) u[18] &= ~0x100ul;
}

static void set_regs(proc_t* p, const uint64_t* u) {
    regs_t* r = p->pt_regs;
    r->r15 = u[0]; r->r14 = u[1]; r->r13 = u[2]; r->r12 = u[3]; r->rbp = u[4]; r->rbx = u[5];
    r->r11 = u[6]; r->r10 = u[7]; r->r9 = u[8]; r->r8 = u[9];
    if (p->pt_entry) r->rax = u[15];
    else { r->rax = u[10]; p->pt_orig = u[15]; }
    r->rcx = u[11]; r->rdx = u[12]; r->rsi = u[13]; r->rdi = u[14];
    if (u[16] < USER_TOP) r->rip = u[16];
    if (u[19] < USER_TOP) r->rsp = u[19];
    r->rflags = (u[18] & 0xcd5) | 0x202 | (p->pt_tf ? 0x100 : 0);
    r->cs = GDT_UCODE; r->ss = GDT_UDATA;
    p->tls_base = u[21];
}

static uint8_t* fpu_of(proc_t* p) {
    task_t* t = task_at(p->task);
    return t && t->proc == p ? t->fpu : NULL;
}

/* tracee memory through its page tables */
static int mem_rd(uint64_t pd, uint64_t va, void* dst, uint64_t len) {
    uint8_t* d = dst;
    while (len) {
        uint64_t pte = vmm_pte(pd, va);
        if (!(pte & PTE_P)) {
            if (!vmm_fault_in(pd, va)) return -1;
            pte = vmm_pte(pd, va);
        }
        uint64_t off = va & 0xFFF, n = PAGE_SIZE - off;
        if (n > len) n = len;
        memcpy(d, (uint8_t*)P2V(pte & PTE_ADDR) + off, n);
        d += n; va += n; len -= n;
    }
    return 0;
}

int pt_mem_read(proc_t* p, uint64_t va, void* dst, uint64_t len) {
    if (!p->pd || va < USER_BASE || va + len > USER_TOP) return -1;
    return mem_rd(p->pd, va, dst, len);
}

int pt_mem_write(proc_t* p, uint64_t va, const void* src, uint64_t len) {
    if (!p->pd || va < USER_BASE || va + len > USER_TOP) return -1;
    return vmm_copy_to(p->pd, va, src, len);
}

/* PEEKUSER / POKEUSER: offsets into struct user */
static int64_t user_peek(proc_t* p, uint64_t off, uint64_t* out) {
    if (off & 7) return -EIO;
    if (off < 216) {
        uint64_t u[27];
        get_regs(p, u);
        *out = u[off / 8];
        return 0;
    }
    if (off >= 848 && off < 848 + 64) { *out = p->dr[(off - 848) / 8]; return 0; }
    *out = 0;
    return off < 912 ? 0 : -EIO;
}

static int64_t user_poke(proc_t* p, uint64_t off, uint64_t v) {
    if (off & 7) return -EIO;
    if (off < 216) {
        uint64_t u[27];
        get_regs(p, u);
        u[off / 8] = v;
        set_regs(p, u);
        return 0;
    }
    if (off >= 848 && off < 848 + 64) {
        int i = (off - 848) / 8;
        if (i < 4 && v >= USER_TOP) return -EIO;
        if (i == 7) v &= 0xffff00ff | 0xff;
        p->dr[i] = v;
        return 0;
    }
    return -EIO;
}

/* the hw debug regs only matter while the tracee is on a cpu, task switch loads them */
void pt_dbg_load(proc_t* p) {
    __asm__ volatile ("mov %0, %%dr0" : : "r"(p->dr[0]));
    __asm__ volatile ("mov %0, %%dr1" : : "r"(p->dr[1]));
    __asm__ volatile ("mov %0, %%dr2" : : "r"(p->dr[2]));
    __asm__ volatile ("mov %0, %%dr3" : : "r"(p->dr[3]));
    __asm__ volatile ("mov %0, %%dr7" : : "r"(p->dr[7]));
}

/* the syscall */

static void resume(proc_t* p, int sig, int mode) {
    p->pt_inj = sig;
    p->pt_sys = mode == 1;
    bool step = mode == 2;
    if (step && !p->pt_tf) p->pt_regs->rflags |= 0x100;
    if (!step && p->pt_tf) p->pt_regs->rflags &= ~0x100ul;
    p->pt_tf = step;
    p->pt_state = 2;
    ready_task_of(p);
}

static int regset(proc_t* p, int nt, uint64_t iov, bool set) {
    if (!uptr(iov, 16)) return -EFAULT;
    uint64_t* v = (uint64_t*)iov;
    uint64_t buf = v[0], len = v[1];
    if (nt == 1) {
        uint64_t u[27];
        if (len > 216) len = 216;
        if (!uptr(buf, len)) return -EFAULT;
        if (set) { get_regs(p, u); memcpy(u, (void*)buf, len); set_regs(p, u); }
        else { get_regs(p, u); memcpy((void*)buf, u, len); }
    } else if (nt == 2) {
        uint8_t* f = fpu_of(p);
        if (!f) return -EIO;
        if (len > 512) len = 512;
        if (!uptr(buf, len)) return -EFAULT;
        if (set) memcpy(f, (void*)buf, len);
        else memcpy((void*)buf, f, len);
    } else return -EINVAL;
    v[1] = len;
    return 0;
}

int64_t sys_ptrace(uint64_t req, uint64_t pid, uint64_t addr, uint64_t data) {
    proc_t* me = proc_current();
    if (req == 0) {                                  /* TRACEME */
        proc_t* par = proc_by_pid(me->ppid);
        if (me->tracer || !par) return -EPERM;
        me->tracer = par->tgid;
        return 0;
    }
    proc_t* p = proc_by_pid((int)pid);
    if (!p || p->state != P_ALIVE) return -ESRCH;
    if (req == 16 || req == 0x4206) {                /* ATTACH / SEIZE */
        if (p->tracer || p->tgid == me->tgid) return -EPERM;
        p->tracer = me->tgid;
        p->pt_seize = req == 0x4206;
        if (p->pt_seize) p->pt_opts = (int)data & O_ALL;
        else proc_send_signal(p, 19);
        return 0;
    }
    if (p->tracer != me->tgid) return -ESRCH;
    if (req == 8) { proc_send_signal(p, 9); return 0; }                  /* KILL */
    if (req == 0x4207) {                                                 /* INTERRUPT */
        if (!p->pt_seize) return -EIO;
        p->pt_intr = true;
        ready_task_of(p);
        return 0;
    }
    if (p->pt_state != 1 || !p->pt_regs) return -ESRCH;
    uint64_t u[27];
    switch (req) {
        case 1: case 2: {
            uint64_t w;
            if (pt_mem_read(p, addr, &w, 8) < 0 || !uptr(data, 8)) return -EIO;
            *(uint64_t*)data = w;
            return 0;
        }
        case 4: case 5:
            return pt_mem_write(p, addr, &data, 8) < 0 ? -EIO : 0;
        case 3: {
            uint64_t w;
            int64_t e = user_peek(p, addr, &w);
            if (e) return e;
            if (!uptr(data, 8)) return -EFAULT;
            *(uint64_t*)data = w;
            return 0;
        }
        case 6: return user_poke(p, addr, data);
        case 7: case 24: case 9:
            if (data >= 65) return -EIO;
            resume(p, (int)data, req == 24 ? 1 : req == 9 ? 2 : 0);
            return 0;
        case 17:                                                         /* DETACH */
            if (p->pt_tf) p->pt_regs->rflags &= ~0x100ul;
            p->pt_tf = false;
            p->tracer = 0;
            p->pt_opts = 0;
            p->pt_sys = false;
            resume(p, (int)data, 0);
            return 0;
        case 12:
            if (!uptr(data, 216)) return -EFAULT;
            get_regs(p, u);
            memcpy((void*)data, u, 216);
            return 0;
        case 13:
            if (!uptr(data, 216)) return -EFAULT;
            get_regs(p, u);
            memcpy(u, (void*)data, 216);
            set_regs(p, u);
            return 0;
        case 14: case 15: {
            uint8_t* f = fpu_of(p);
            if (!f) return -EIO;
            if (!uptr(data, 512)) return -EFAULT;
            if (req == 14) memcpy((void*)data, f, 512);
            else memcpy(f, (void*)data, 512);
            return 0;
        }
        case 0x4200:
            if (data & ~(uint64_t)O_ALL) return -EINVAL;
            p->pt_opts = (int)data;
            return 0;
        case 0x4201:
            if (!uptr(data, 8)) return -EFAULT;
            *(uint64_t*)data = p->pt_msg;
            return 0;
        case 0x4202:
            if (!p->pt_sival) return -EINVAL;
            if (!uptr(data, 128)) return -EFAULT;
            memcpy((void*)data, p->pt_si, 128);
            return 0;
        case 0x4203:
            if (!uptr(data, 128)) return -EFAULT;
            memcpy(p->pt_si, (void*)data, 128);
            return 0;
        case 0x4204: return regset(p, (int)addr, data, false);
        case 0x4205: return regset(p, (int)addr, data, true);
        case 0x420a:
            if (!uptr(data, 8)) return -EFAULT;
            *(uint64_t*)data = p->sig_mask;
            return 0;
    }
    return -EIO;
}

/* int3 and #DB (single step, hw breakpoints) */
void pt_trap(regs_t* r) {
    uint64_t dr6 = 0;
    if (r->vec == 1) {
        __asm__ volatile ("mov %%dr6, %0" : "=r"(dr6));
        __asm__ volatile ("mov %0, %%dr6" : : "r"(0ul));
    }
    if ((r->cs & 3) != 3) return;
    proc_t* p = proc_current();
    if (r->vec == 1) {
        p->dr[6] = dr6;
        if (p->pt_tf) { r->rflags &= ~0x100ul; p->pt_tf = false; }
    }
    if (!proc_fault_signal(r, 5, r->vec, 0, 0)) proc_fault_kill("trap", 5, r->rip, 0);
}
