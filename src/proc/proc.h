#ifndef SAMARA_PROC_H
#define SAMARA_PROC_H
#include "core/types.h"
#include "core/task.h"
#include "fs/fs.h"

/* User processes: ring-3 programs loaded from static i386 ELF files in the
   ramfs, talking to the kernel through the Linux int 0x80 ABI so that
   unmodified static binaries (busybox + musl) run as-is. */

#define MAX_PROCS   48
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
    fs_node_t* cwd;
    int      umask;
} pshared_t;

typedef struct proc {
    pstate_t state;
    int      pid, ppid, pgid, sid;
    int      tgid;                 /* thread group = pid of the leader */
    bool     is_thread;            /* made by clone(CLONE_THREAD), not waitable */
    bool     zleader;              /* leader called exit() while threads still run */
    bool     in_futex;
    int      task;                 /* task slot running this process */
    uint64_t pd;                   /* top level page table (physical) */
    uint64_t tls_base;             /* fs base */
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

/* procfs.c: rebuild /proc from live kernel state (called on /proc lookups). */
void    procfs_refresh(void);

#endif
