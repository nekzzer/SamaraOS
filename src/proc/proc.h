#ifndef SAMARA_PROC_H
#define SAMARA_PROC_H
#include "core/types.h"
#include "core/task.h"
#include "fs/fs.h"

/* User processes: ring-3 programs loaded from static i386 ELF files in the
   ramfs, talking to the kernel through the Linux int 0x80 ABI so that
   unmodified static binaries (busybox + musl) run as-is. */

#define MAX_PROCS   448             /* java alone has ~40 threads, 48 ran out under minecraft */
#define MAX_FDS     1024            /* Linux default RLIMIT_NOFILE: ld keeps every input open */
#define NSIG_MAX    65
#define KSTACK_SZ   16384

struct file;

typedef enum { P_FREE = 0, P_ALIVE, P_ZOMBIE } pstate_t;

/* What threads of one process share (CLONE_FILES|SIGHAND|FS|VM). The leader
   owns it, threads just point at it. */
typedef struct pshared {
    struct file* fds[MAX_FDS];
    uint8_t  cloexec[MAX_FDS];
    struct {
        uint64_t handler;          /* 0 = SIG_DFL, 1 = SIG_IGN, else user fn */
        uint32_t flags;            /* SA_* */
        uint64_t restorer;         /* musl's __restore / __restore_rt */
        uint64_t mask;
    } sa[NSIG_MAX];
    uint64_t brk_start, brk;
    struct ptimer {                /* timer_create, ms resolution, deadline in uptime ms */
        bool     used, armed;
        int      clk, signo, notify, tid;
        uint64_t val;              /* sigev_value */
        uint32_t at, iv;
        int      over;
    } tm[16];
    fs_node_t* cwd;
    fs_node_t* root;               /* chroot, NULL = real root */
    int      umask;
    uint64_t rl[16][2];            /* rlimits: cur, max */
} pshared_t;

typedef struct proc {
    pstate_t state;
    int      pid, ppid, pgid, sid;
    int      tgid;                 /* thread group = pid of the leader */
    bool     is_thread;            /* made by clone(CLONE_THREAD), not waitable */
    bool     zleader;              /* leader called exit() while threads still run */
    bool     in_futex;
    bool     ujb_on;               /* a kernel fault on a user address longjmps to ujb (-EFAULT) */
    uint64_t ujb[8];
    int      task;                 /* task slot running this process */
    uint64_t pd;                   /* top level page table (physical) */
    uint64_t tls_base;             /* fs base */
    uint64_t gs_base;              /* user gs, wine keeps the TEB there */
    uint64_t clear_child_tid;
    pshared_t* sh;
    pshared_t  shd;                /* leader's own, threads use sh of the leader */
    uint64_t sig_mask;             /* Linux sigset: bit (sig-1) */
    uint64_t sig_pending;          /* caught signals waiting for delivery */
    uint64_t saved_mask;           /* sigsuspend: mask to restore after the handler */
    bool     in_sigsuspend;
    uint64_t ss_sp, ss_size;       /* sigaltstack, size 0 = off. qemu coroutines need it */
    uint32_t alarm_at;             /* uptime ms of pending SIGALRM, 0 = none */
    uint32_t alarm_interval;       /* setitimer reload, ms */
    int      exit_status;          /* wait(2) encoding once zombie */
    bool     kernel_waited;        /* launched by the kernel shell, which reaps it */
    bool     tty_detached;         /* background job: console reads EOF, writes are dropped */
    int      ctty;                 /* controlling terminal: 0 console, -1 none, n>0 pty n-1 */
    char     name[32];
    char     exe[128];
    bool     in_fs;                /* inside a syscall that changes files (ext2 sync waits) */
    int      fault_sig;            /* a cpu fault being turned into a signal: siginfo/sigcontext details */
    uint64_t fault_addr, fault_trap, fault_err;             /* /proc/self/exe, symlinks resolved ($ORIGIN in java) */
    bool     vfork_shared;         /* vfork child running on the parent's page directory */
    /* Accounting for /proc. */
    uint32_t start_ms;
    uint32_t utime, stime;         /* PIT ticks (1 ms) in ring 3 / ring 0 */
    char     cmdline[256];         /* argv, NUL-separated, as the user typed it */
    uint16_t cmdline_len;
    uint32_t itv_at, itv_iv, itp_at, itp_iv;   /* ITIMER_VIRTUAL / PROF: cpu ms left, reload */
    int      sq_sig, sq_tid, sq_over;          /* siginfo of the last timer signal */
    uint64_t sq_val;
    /* ptrace and job stop, see ptrace.c */
    int      tracer;               /* tgid of the tracer, 0 = nobody */
    int      pt_opts;
    int      pt_state;             /* 0 running, 1 stopped, 2 told to go on (isr style stops only) */
    int      pt_stopsig, pt_event, pt_inj, pt_kind;
    uint64_t pt_msg, pt_orig;
    regs_t*  pt_regs;
    bool     pt_rep, pt_sys, pt_isr, pt_isr_stop, pt_intr, pt_seize, pt_job, pt_entry, pt_tf, pt_sival;
    uint8_t  pt_si[128];
    uint64_t dr[8];
    int      nice, policy, rtprio;     /* setpriority, sched_setscheduler */
    /* namespaces (indexes into ns.c tables, 0 = the initial one), see ns.c */
    uint8_t  mntns, utsns, ipcns, userns, netns, pidns, kidns;   /* kidns: pid ns for children after unshare */
    int      vpid[4];                  /* pid at every pid ns level, [0] is the real pid */
    uint64_t nsfl;                     /* CLONE_NEW* for the child being forked */
    /* seccomp */
    bool     nnp;
    bool     fsp;           // clone(CLONE_FS) child: chroot/chdir also hit the parent (no shared fs struct here)
    bool     capx;          // capset was called, cap[] is real (e, p, i)
    uint64_t cap[3];
    uint8_t  sc_mode;                  /* 0 off, 1 strict, 2 filter */
    struct sfilt* sf;                  /* newest filter, older ones hang off ->prev */
    bool     sys_sig;                  /* SIGSYS from seccomp is being delivered */
    int      sys_nr, sys_err;
    uint64_t sys_ip;
} proc_t;

void    proc_init(void);

/* Kernel-side launch: runs `path` with argv/envp (NULL-terminated) in a new
   process whose stdin/out/err are the console tty. Returns pid or -errno. */
int     proc_spawn(const char* path, char* const argv[], char* const envp[]);
/* Background launch (desktop icons, `cmd &`): stdio is /dev/null, nobody
   waits for it - it is freed when it exits - and it never touches the tty. */
int     proc_spawn_detached(const char* path, char* const argv[], char* const envp[]);
/* Turn a kernel-waited foreground job into such a background one. */
void    proc_detach(int pid);
void    proc_kill_session(int sid);   /* SIGKILL every process of a session */
bool    proc_alive(int pid);          /* still running (not zombie/free) */
int     proc_reap(int pid);           /* free a kernel-launched zombie, returns wait status */
void    proc_kill_all(void);          /* terminate every user process */

proc_t* proc_current(void);           /* NULL on kernel tasks */
proc_t* proc_by_pid(int pid);
int     proc_pid(proc_t* p);
uint64_t proc_tls_base(proc_t* p);
uint64_t proc_gs_base(proc_t* p);
bool    proc_interrupted(void);       /* current process has a fatal signal pending */
void    proc_signal_group(int pgid, int sig);
int     proc_send_signal(proc_t* p, int sig);
int     proc_send_signal_tid(proc_t* p, int sig);
int     proc_count(void);
proc_t* proc_at(int i);

/* Fault plumbing called from the exception handlers. */
bool    proc_handle_fault(uint64_t addr, uint64_t err);
void    proc_fault_stack(uint64_t rsp);
/* cpu fault in ring 3 with a handler installed: queue the signal with its
   details and build the frame now. false = no handler, caller kills */
bool    proc_fault_signal(regs_t* r, int sig, uint64_t trap, uint64_t err, uint64_t addr);
void    proc_fault_kill(const char* what, int sig, uint64_t rip, uint64_t addr) __attribute__((noreturn));

/* Syscall-level operations (syscall.c calls these). */
int     proc_fork(regs_t* r);
int     proc_clone(regs_t* r);     /* full clone(2): threads or fork */
void    proc_thread_exit(int code) __attribute__((noreturn));
int     futex_op(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint64_t uaddr2, uint32_t val3);
void    futex_forget(proc_t* p);
void    futex_wake_addr(uint64_t pd, uint64_t addr, int n);
int     proc_vfork(regs_t* r);     /* child shares memory, parent waits for exec/exit */
int     proc_execve(regs_t* r, const char* path, char* const argv[], char* const envp[]);
void    proc_exit(int status) __attribute__((noreturn));   /* status = wait encoding */
int     proc_wait(int pid, int* status, int options);

void    syscall_init(void);

/* ns.c */
int     ns_unshare(uint64_t fl);
int     ns_join(int type, int idx);
int     ns_idx(proc_t* p, int type);
int     ns_level(proc_t* p);
const char* ns_type_name(int t);
int     ns_gpid(proc_t* v, int g);
int     ns_lpid(proc_t* v, int vp);
void    ns_fork(proc_t* p, proc_t* c);          /* child gets the namespaces of p (+ new ones from p->nsfl) */
void    ns_exit(proc_t* p);
int     ns_pid_view(proc_t* v, proc_t* q);      /* pid of q as seen from v, 0 = invisible */
proc_t* ns_pid_find(proc_t* v, int vp);
int     ns_uid(proc_t* p, int g);
int     ns_uname(proc_t* p, char* host, char* dom);
int     ns_sethost(proc_t* p, const char* s, int n, bool dom);
int     ns_idmap(proc_t* t, int what, const char* s, int n);   /* what: 0 uid_map, 1 gid_map, 2 setgroups */
int     ns_idmap_text(proc_t* t, int what, char* out, int cap);
uint32_t ns_ino(proc_t* p, int type);
int     ns_pivot(const char* nw, const char* old);
bool    ns_loopback_only(void);

/* seccomp.c */
struct sfilt;
int     sc_prctl(uint64_t op, uint64_t a, uint64_t b);
int     sc_seccomp(uint64_t op, uint64_t fl, uint64_t uargs);
int     sc_check(regs_t* r);                    /* 0 run it, 1 answered, 2 SIGSYS, 3 kill */
void    sc_trap(regs_t* r);
struct sfilt* sc_dup(struct sfilt* f);
void    sc_put(struct sfilt* f);

/* signal.c */
#define SIGBIT(s) (1ull << ((s) - 1))
bool    proc_signal_deliverable(proc_t* p);
/* On the way back to ring 3: push a handler frame for one pending signal.
   `nr`/`ret` describe the syscall being returned from (nr = -1 if none) so
   SA_RESTART can re-issue it. */
void    proc_deliver_signal(regs_t* r, int nr, int32_t ret);
int     proc_sigreturn(regs_t* r);
int     proc_sigaction(int sig, const uint64_t* act, uint64_t* oact);
int     proc_sigsuspend(const uint64_t* mask);
int     proc_sigaltstack(const uint64_t* ss, uint64_t* old, uint64_t rsp);
void    proc_check_alarm(proc_t* p, bool from_irq);   /* fire SIGALRM when due */
void    proc_account_tick(proc_t* p, bool user);
void    proc_timers_tick(uint32_t now);
void    proc_timer_hint(uint32_t at);
uint32_t proc_next_timer(uint32_t now);
int64_t sys_timer(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

/* ptrace.c */
int64_t sys_ptrace(uint64_t req, uint64_t pid, uint64_t addr, uint64_t data);
int     pt_stop(proc_t* p, regs_t* r, int sig, int event, int kind);   /* 0 = stopped in an isr, caller leaves */
void    pt_syscall_stop(proc_t* p, regs_t* r, bool exit);
void    pt_event(proc_t* p, regs_t* r, int event, uint64_t msg);
void    pt_exec(proc_t* p, regs_t* r);
void    pt_exit_event(proc_t* p, int status);
void    pt_child(proc_t* p, proc_t* c, int kind);                      /* fork/vfork/clone: auto attach */
void    pt_release(int tgid);                                          /* tracer is gone */
bool    pt_wait_report(proc_t* c, proc_t* me, int options, int* st);
void    pt_cont_group(proc_t* p);                                      /* SIGCONT */
void    ready_task_of(proc_t* p);
void    pt_dbg_load(proc_t* p);

/* procfs.c: rebuild /proc from live kernel state (called on /proc lookups). */
void    procfs_refresh(void);

#endif
