/* Signal delivery to user handlers, Linux i386 style.

   When a process is about to return to ring 3 (end of a syscall, or being
   switched back in by the scheduler) and has an unmasked caught signal
   pending, its user stack gets a signal frame and execution resumes in the
   handler. The handler returns into musl's restorer, which issues
   sigreturn / rt_sigreturn; that restores the interrupted registers, FPU
   state and signal mask from the frame.

   Frame (rt, SA_SIGINFO):       Frame (classic):
     pretcode  <- esp               pretcode  <- esp
     sig                            sig
     &info                          sigcontext (88 bytes)  <- esp at sigreturn
     &uc                            extramask
     siginfo (128)                  FXSAVE area (512, 16-aligned)
     ucontext: flags, link,
       stack(12), sigcontext(88),
       sigmask(8)
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
#define SA_RESTART   0x10000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

#define ERESTART_NR  0

typedef struct {
    uint32_t gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t trapno, err, eip, cs, eflags, esp_at_signal, ss, fpstate, oldmask, cr2;
} sigcontext_t;

#define UNBLOCKABLE (SIGBIT(9) | SIGBIT(19))

bool proc_signal_deliverable(proc_t* p) {
    return (p->sig_pending & ~(p->sig_mask & ~UNBLOCKABLE)) != 0;
}

/* Make [lo, lo+len) writable user memory, growing the stack if needed. */
static bool user_writable(proc_t* p, uint32_t lo, uint32_t len) {
    if (lo < USER_BASE || lo + len > USER_TOP || lo + len < lo) return false;
    for (uint32_t a = lo & ~(PAGE_SIZE - 1); a < lo + len; a += PAGE_SIZE) {
        uint32_t pte = vmm_pte(p->pd, a);
        if ((pte & PTE_P) && (pte & PTE_RW)) continue;
        if (pte & PTE_P) return false;
        if (a < USER_STACK_TOP - USER_STACK_MAX || a >= USER_STACK_TOP) return false;
        if (vmm_alloc_range(p->pd, a, PAGE_SIZE, true) < 0) return false;
    }
    return true;
}

static void save_context(sigcontext_t* sc, const regs_t* r, uint32_t fx, uint64_t oldmask) {
    sc->gs = r->gs; sc->fs = r->fs; sc->es = r->es; sc->ds = r->ds;
    sc->edi = r->edi; sc->esi = r->esi; sc->ebp = r->ebp; sc->esp = r->useresp;
    sc->ebx = r->ebx; sc->edx = r->edx; sc->ecx = r->ecx; sc->eax = r->eax;
    sc->trapno = 0; sc->err = 0;
    sc->eip = r->eip; sc->cs = r->cs; sc->eflags = r->eflags;
    sc->esp_at_signal = r->useresp; sc->ss = r->ss;
    sc->fpstate = fx;
    sc->oldmask = (uint32_t)oldmask;
    sc->cr2 = 0;
}

void proc_deliver_signal(regs_t* r, int nr, int32_t ret) {
    proc_t* p = proc_current();
    if (!p || p->state != P_ALIVE || (r->cs & 3) != 3) return;
    uint64_t ready = p->sig_pending & ~(p->sig_mask & ~UNBLOCKABLE);
    if (!ready) return;
    int sig = 1;
    while (!(ready & SIGBIT(sig))) sig++;
    p->sig_pending &= ~SIGBIT(sig);

    uint32_t handler = p->sa[sig].handler, flags = p->sa[sig].flags;
    if (handler <= 1) return;                        /* reset to DFL/IGN meanwhile */

    /* Interrupted syscall: restart it transparently, or report EINTR. */
    if (nr >= 0 && ret == -EINTR && (flags & SA_RESTART) && nr != 179 && nr != 29) {
        r->eax = (uint32_t)nr;
        r->eip -= 2;                                 /* back onto "int $0x80" */
    }

    uint64_t oldmask = p->in_sigsuspend ? p->saved_mask : p->sig_mask;
    p->in_sigsuspend = false;
    bool rt = (flags & SA_SIGINFO) != 0;
    uint32_t restorer = (flags & SA_RESTORER) ? p->sa[sig].restorer : 0;

    uint32_t sp = r->useresp;
    uint32_t fx = (sp - 512) & ~15u;                 /* FPU state on top */
    uint32_t base;
    if (rt) base = (fx - (16 + 128 + 128)) & ~15u;
    else    base = (fx - (8 + sizeof(sigcontext_t) + 4)) & ~15u;
    base -= 4;                                       /* ABI: (esp+4) 16-aligned at entry */
    if (!user_writable(p, base, sp - base)) {
        proc_send_signal(p, 11);                     /* can't build a frame: SIGSEGV */
        return;
    }

    __asm__ volatile ("fxsave (%0)" : : "r"(fx) : "memory");
    uint32_t* w = (uint32_t*)base;
    if (rt) {
        uint32_t info = base + 16, uc = info + 128;
        w[0] = restorer; w[1] = (uint32_t)sig; w[2] = info; w[3] = uc;
        memset((void*)info, 0, 128 + 128);
        ((uint32_t*)info)[0] = (uint32_t)sig;        /* si_signo */
        uint32_t* u = (uint32_t*)uc;                 /* flags, link, stack[3] */
        save_context((sigcontext_t*)(u + 5), r, fx, oldmask);
        memcpy(u + 5 + 22, &oldmask, 8);             /* uc_sigmask */
    } else {
        w[0] = restorer; w[1] = (uint32_t)sig;
        save_context((sigcontext_t*)(w + 2), r, fx, oldmask);
        w[2 + 22] = (uint32_t)(oldmask >> 32);       /* extramask */
    }

    if (!(flags & SA_NODEFER)) p->sig_mask |= SIGBIT(sig);
    p->sig_mask |= p->sa[sig].mask;
    if (flags & SA_RESETHAND) { p->sa[sig].handler = 0; p->sa[sig].flags = 0; }

    r->useresp = base;
    r->eip = handler;
    r->eax = (uint32_t)sig;                          /* regparm-style extra copies */
    r->edx = rt ? base + 16 : 0;
    r->ecx = rt ? base + 16 + 128 : 0;
    r->eflags &= ~0x400u;                            /* DF=0 on entry per ABI */
    r->cs = GDT_UCODE; r->ss = GDT_UDATA;
    r->ds = r->es = GDT_UDATA;
}

static uint32_t sane_seg(uint32_t s) {
    return (s == GDT_UDATA || s == GDT_UTLS) ? s : GDT_UDATA;
}

int proc_sigreturn(regs_t* r, bool rt) {
    proc_t* p = proc_current();
    sigcontext_t sc;
    uint64_t mask;
    if (rt) {
        uint32_t frame = r->useresp - 4;             /* pretcode was popped by ret */
        if (frame < USER_BASE || frame + 16 + 256 > USER_TOP) return -EFAULT;
        uint32_t* u = (uint32_t*)(frame + 16 + 128);
        memcpy(&sc, u + 5, sizeof(sc));
        memcpy(&mask, u + 5 + 22, 8);
    } else {
        uint32_t at = r->useresp;                    /* restorer popped sig too */
        if (at < USER_BASE || at + sizeof(sc) + 4 > USER_TOP) return -EFAULT;
        memcpy(&sc, (void*)at, sizeof(sc));
        mask = sc.oldmask | ((uint64_t)((uint32_t*)at)[22] << 32);
    }
    r->gs = sane_seg(sc.gs); r->fs = sane_seg(sc.fs);
    r->es = GDT_UDATA; r->ds = GDT_UDATA;
    r->edi = sc.edi; r->esi = sc.esi; r->ebp = sc.ebp;
    r->ebx = sc.ebx; r->edx = sc.edx; r->ecx = sc.ecx; r->eax = sc.eax;
    r->eip = sc.eip;
    r->useresp = sc.esp;
    r->eflags = (sc.eflags & 0x0DD5u) | 0x202u;      /* arithmetic flags + DF only */
    r->cs = GDT_UCODE; r->ss = GDT_UDATA;
    if (sc.fpstate >= USER_BASE && sc.fpstate + 512 <= USER_TOP && !(sc.fpstate & 15))
        __asm__ volatile ("fxrstor (%0)" : : "r"(sc.fpstate) : "memory");
    p->sig_mask = mask & ~UNBLOCKABLE;
    return 0;
}

int proc_sigaction(int sig, const uint32_t* act, uint32_t* oact, bool old_abi) {
    if (sig <= 0 || sig >= NSIG_MAX) return -EINVAL;
    proc_t* p = proc_current();
    /* rt layout: handler, flags, restorer, mask[2]; old: handler, mask, flags, restorer */
    if (oact) {
        if (old_abi) {
            oact[0] = p->sa[sig].handler; oact[1] = (uint32_t)p->sa[sig].mask;
            oact[2] = p->sa[sig].flags;   oact[3] = p->sa[sig].restorer;
        } else {
            oact[0] = p->sa[sig].handler; oact[1] = p->sa[sig].flags;
            oact[2] = p->sa[sig].restorer;
            memcpy(&oact[3], &p->sa[sig].mask, 8);
        }
    }
    if (act) {
        if (sig == 9 || sig == 19) return -EINVAL;
        p->sa[sig].handler = act[0];
        if (old_abi) {
            p->sa[sig].mask = act[1]; p->sa[sig].flags = act[2]; p->sa[sig].restorer = act[3];
        } else {
            p->sa[sig].flags = act[1]; p->sa[sig].restorer = act[2];
            memcpy(&p->sa[sig].mask, &act[3], 8);
        }
        p->sa[sig].mask &= ~UNBLOCKABLE;
        if (act[0] <= 1) p->sig_pending &= ~SIGBIT(sig);   /* now DFL/IGN: drop */
    }
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
    if (from_irq && p->sa[14].handler <= 1) return;
    p->alarm_at = p->alarm_interval ? pit_uptime_ms() + p->alarm_interval : 0;
    proc_send_signal(p, 14);                         /* SIGALRM */
}
