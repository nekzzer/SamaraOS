/* Signal delivery to user handlers, Linux x86_64 style.

   When a process is about to return to ring 3 (end of a syscall, or being
   switched back in by the scheduler) and has an unmasked caught signal
   pending, its user stack gets an rt_sigframe and execution resumes in the
   handler. The handler returns into musl's restorer, which issues
   rt_sigreturn; that restores the interrupted registers, FPU state and
   signal mask from the frame.

   Frame (low -> high):
     pretcode (8)  <- rsp at handler entry
     ucontext (304): flags, link, stack(24), sigcontext(256), sigmask(8)
     siginfo (128)
     FXSAVE area (512, 16-aligned) */

#include "proc/proc.h"
#include "core/string.h"
#include "core/vmm.h"
#include "boot/gdt.h"
#include "boot/pit.h"

#define EINTR  4
#define EFAULT 14
#define EINVAL 22

#define SA_SIGINFO   0x00000004u
#define SA_RESTORER  0x04000000u
#define SA_ONSTACK   0x08000000u
#define SA_RESTART   0x10000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

/* linux struct sigcontext */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate;
    uint64_t res[8];
} sigcontext_t;

#define UNBLOCKABLE (SIGBIT(9) | SIGBIT(19))

bool proc_signal_deliverable(proc_t* p) {
    return (p->sig_pending & ~(p->sig_mask & ~UNBLOCKABLE)) != 0;
}

/* Make [lo, lo+len) writable user memory, growing the stack if needed. */
static bool user_writable(proc_t* p, uint64_t lo, uint64_t len) {
    if (lo < USER_BASE || lo + len > USER_TOP || lo + len < lo) return false;
    for (uint64_t a = lo & ~(PAGE_SIZE - 1); a < lo + len; a += PAGE_SIZE) {
        uint64_t pte = vmm_pte(p->pd, a);
        if ((pte & PTE_P) && (pte & PTE_RW)) continue;
        if (pte & PTE_P) { if ((pte & PTE_COW) && vmm_cow(p->pd, a)) continue; return false; }
        if (pte & PTE_LAZY) { if ((pte & PTE_RW) && (pte & PTE_US) && vmm_fault_in(p->pd, a)) continue; return false; }
        if (a < USER_STACK_TOP - USER_STACK_MAX || a >= USER_STACK_TOP) return false;
        if (vmm_alloc_range(p->pd, a, PAGE_SIZE, true) < 0) return false;
    }
    return true;
}

static void save_context(sigcontext_t* sc, const regs_t* r, uint64_t fx, uint64_t oldmask) {
    memset(sc, 0, sizeof(*sc));
    sc->r8 = r->r8; sc->r9 = r->r9; sc->r10 = r->r10; sc->r11 = r->r11;
    sc->r12 = r->r12; sc->r13 = r->r13; sc->r14 = r->r14; sc->r15 = r->r15;
    sc->rdi = r->rdi; sc->rsi = r->rsi; sc->rbp = r->rbp; sc->rbx = r->rbx;
    sc->rdx = r->rdx; sc->rax = r->rax; sc->rcx = r->rcx; sc->rsp = r->rsp;
    sc->rip = r->rip; sc->eflags = r->rflags;
    sc->cs = (uint16_t)r->cs; sc->ss = (uint16_t)r->ss;
    sc->fpstate = fx;
    sc->oldmask = oldmask;
}

void proc_deliver_signal(regs_t* r, int nr, int32_t ret) {
    proc_t* p = proc_current();
    if (!p || p->state != P_ALIVE || (r->cs & 3) != 3) return;
    uint64_t ready = p->sig_pending & ~(p->sig_mask & ~UNBLOCKABLE);
    if (!ready) return;
    int sig = 1;
    while (!(ready & SIGBIT(sig))) sig++;
    p->sig_pending &= ~SIGBIT(sig);

    uint64_t handler = p->sh->sa[sig].handler;
    uint32_t flags = p->sh->sa[sig].flags;
    if (handler <= 1) return;                        /* reset to DFL/IGN meanwhile */

    /* Interrupted syscall: restart it transparently, or report EINTR. */
    if (nr >= 0 && ret == -EINTR && (flags & SA_RESTART) && nr != 130 && nr != 34) {
        r->rax = (uint64_t)nr;
        r->rip -= 2;                                 /* back onto "syscall" */
    }

    uint64_t oldmask = p->in_sigsuspend ? p->saved_mask : p->sig_mask;
    p->in_sigsuspend = false;
    uint64_t restorer = (flags & SA_RESTORER) ? p->sh->sa[sig].restorer : 0;

    uint64_t sp = r->rsp - 128;                      /* red zone */
    bool on_alt = p->ss_size && sp - p->ss_sp <= p->ss_size;
    if ((flags & SA_ONSTACK) && p->ss_size && !on_alt) sp = p->ss_sp + p->ss_size;
    uint64_t fx = (sp - 512) & ~15ul;                /* FPU state on top */
    uint64_t base = ((fx - (8 + 304 + 128)) & ~15ul) - 8;   /* rsp + 8 is 16-aligned at entry */
    if (!user_writable(p, base, sp - base)) {
        proc_send_signal(p, 11);                     /* can't build a frame: SIGSEGV */
        return;
    }

    __asm__ volatile ("fxsave64 (%0)" : : "r"(fx) : "memory");
    uint64_t uc = base + 8, info = uc + 304;
    *(uint64_t*)base = restorer;
    memset((void*)uc, 0, 304 + 128);
    uint64_t* u = (uint64_t*)uc;                     /* flags, link, stack{sp, flags, size} */
    u[2] = p->ss_sp; u[3] = p->ss_size ? (on_alt ? 1 : 0) : 2; u[4] = p->ss_size;
    sigcontext_t* sc = (sigcontext_t*)(u + 5);
    save_context(sc, r, fx, oldmask);
    memcpy(u + 5 + 32, &oldmask, 8);                 /* uc_sigmask */
    uint32_t* si = (uint32_t*)info;
    si[0] = (uint32_t)sig;                           /* si_signo */
    if (sig == p->fault_sig) {                       /* from a cpu fault: what and where */
        si[2] = p->fault_trap == 14 ? ((p->fault_err & 1) ? 2 : 1)    /* SEGV_ACCERR / MAPERR */
              : 1;                                                    /* FPE_INTDIV, ILL_ILLOPC */
        *(uint64_t*)(si + 4) = p->fault_trap == 14 ? p->fault_addr : r->rip;   /* si_addr */
        sc->trapno = p->fault_trap; sc->err = p->fault_err; sc->cr2 = p->fault_addr;
        p->fault_sig = 0;
    }

    if (!(flags & SA_NODEFER)) p->sig_mask |= SIGBIT(sig);
    p->sig_mask |= p->sh->sa[sig].mask;
    if (flags & SA_RESETHAND) { p->sh->sa[sig].handler = 0; p->sh->sa[sig].flags = 0; }

    r->rsp = base;
    r->rip = handler;
    r->rdi = (uint64_t)sig;
    r->rsi = info;
    r->rdx = uc;
    r->rax = 0;
    r->rflags &= ~0x400ul;                           /* DF=0 on entry per ABI */
    r->cs = GDT_UCODE; r->ss = GDT_UDATA;
}

/* hotspot lives on this: implicit null checks, safepoint polls and stack
   banging are SIGSEGVs it catches and fixes up in the ucontext. before,
   every ring 3 fault killed the process (minecraft died at once) */
bool proc_fault_signal(regs_t* r, int sig, uint64_t trap, uint64_t err, uint64_t addr) {
    proc_t* p = proc_current();
    if (!p || p->state != P_ALIVE) return false;
    if (p->sh->sa[sig].handler <= 1 || (p->sig_mask & SIGBIT(sig))) return false;   /* DFL/IGN/blocked: die */
    p->fault_sig = sig; p->fault_trap = trap; p->fault_err = err; p->fault_addr = addr;
    uint64_t other = p->sig_pending;                 /* this one first, the rest stays queued */
    p->sig_pending = SIGBIT(sig);
    proc_deliver_signal(r, -1, 0);
    bool done = !(p->sig_pending & SIGBIT(sig)) && r->rip == p->sh->sa[sig].handler;
    p->sig_pending |= other;
    if (!done) { p->fault_sig = 0; return false; }
    return true;
}

int proc_sigreturn(regs_t* r) {
    proc_t* p = proc_current();
    sigcontext_t sc;
    uint64_t mask;
    uint64_t frame = r->rsp - 8;                     /* pretcode was popped by ret */
    if (frame < USER_BASE || frame + 8 + 304 + 128 > USER_TOP) return -EFAULT;
    uint64_t* u = (uint64_t*)(frame + 8);
    memcpy(&sc, u + 5, sizeof(sc));
    memcpy(&mask, u + 5 + 32, 8);
    r->r8 = sc.r8; r->r9 = sc.r9; r->r10 = sc.r10; r->r11 = sc.r11;
    r->r12 = sc.r12; r->r13 = sc.r13; r->r14 = sc.r14; r->r15 = sc.r15;
    r->rdi = sc.rdi; r->rsi = sc.rsi; r->rbp = sc.rbp; r->rbx = sc.rbx;
    r->rdx = sc.rdx; r->rcx = sc.rcx; r->rax = sc.rax;
    r->rip = sc.rip;
    r->rsp = sc.rsp;
    r->rflags = (sc.eflags & 0x0DD5ul) | 0x202ul;     /* arithmetic flags + DF only */
    r->cs = GDT_UCODE; r->ss = GDT_UDATA;
    if (sc.fpstate >= USER_BASE && sc.fpstate + 512 <= USER_TOP && !(sc.fpstate & 15))
        __asm__ volatile ("fxrstor64 (%0)" : : "r"(sc.fpstate) : "memory");
    p->sig_mask = mask & ~UNBLOCKABLE;
    return 0;
}

/* kernel rt_sigaction: handler, flags, restorer, mask */
int proc_sigaction(int sig, const uint64_t* act, uint64_t* oact) {
    if (sig <= 0 || sig >= NSIG_MAX) return -EINVAL;
    proc_t* p = proc_current();
    if (oact) {
        oact[0] = p->sh->sa[sig].handler; oact[1] = p->sh->sa[sig].flags;
        oact[2] = p->sh->sa[sig].restorer; oact[3] = p->sh->sa[sig].mask;
    }
    if (act) {
        if (sig == 9 || sig == 19) return -EINVAL;
        p->sh->sa[sig].handler = act[0];
        p->sh->sa[sig].flags = (uint32_t)act[1]; p->sh->sa[sig].restorer = act[2];
        p->sh->sa[sig].mask = act[3] & ~UNBLOCKABLE;
        if (act[0] <= 1) p->sig_pending &= ~SIGBIT(sig);   /* now DFL/IGN: drop */
    }
    return 0;
}

/* stack_t: sp, flags (+pad), size. SS_ONSTACK 1, SS_DISABLE 2 */
int proc_sigaltstack(const uint64_t* ss, uint64_t* old, uint64_t rsp) {
    proc_t* p = proc_current();
    bool on = p->ss_size && rsp - p->ss_sp <= p->ss_size;
    if (old) { old[0] = p->ss_sp; old[1] = p->ss_size ? (on ? 1 : 0) : 2; old[2] = p->ss_size; }
    if (!ss) return 0;
    if (on) return -1;                               /* EPERM */
    if (ss[1] & 2) { p->ss_sp = p->ss_size = 0; return 0; }
    if (ss[1] & 0xFFFFFFFD) return -EINVAL;
    if (ss[2] < 2048) return -12;                    /* ENOMEM, MINSIGSTKSZ */
    p->ss_sp = ss[0];
    p->ss_size = ss[2];
    return 0;
}

int proc_sigsuspend(const uint64_t* mask) {
    proc_t* p = proc_current();
    p->saved_mask = p->sig_mask;
    p->sig_mask = *mask & ~UNBLOCKABLE;
    p->in_sigsuspend = true;
    while (!proc_signal_deliverable(p)) task_sleep_ms(10);
    return -EINTR;
}

/* `from_irq`: called while switching tasks, where a default (fatal) SIGALRM
   must wait for the process's next syscall instead of tearing it down. */
void proc_check_alarm(proc_t* p, bool from_irq) {
    if (!p || !p->alarm_at || (int32_t)(pit_uptime_ms() - p->alarm_at) < 0) return;
    if (from_irq && p->sh->sa[14].handler <= 1) return;
    p->alarm_at = p->alarm_interval ? pit_uptime_ms() + p->alarm_interval : 0;
    proc_send_signal(p, 14);                         /* SIGALRM */
}
