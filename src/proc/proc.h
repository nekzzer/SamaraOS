#ifndef SAMARA_PROC_H
#define SAMARA_PROC_H
#include "core/types.h"
#include "core/task.h"
#include "fs/fs.h"

/* User processes: ring-3 programs loaded from static i386 ELF files in the
   ramfs, talking to the kernel through the Linux int 0x80 ABI so that
   unmodified static binaries (busybox + musl) run as-is. */

#define MAX_PROCS   48
#define MAX_FDS     64
#define NSIG_MAX    65
#define KSTACK_SZ   16384

struct file;

typedef enum { P_FREE = 0, P_ALIVE, P_ZOMBIE } pstate_t;

typedef struct proc {
    pstate_t state;
    int      pid, ppid, pgid, sid;
    int      task;                 /* task slot running this process */
    uint32_t pd;                   /* page directory (physical) */
    uint32_t brk_start, brk;
    uint32_t tls_base;
    uint32_t clear_child_tid;
    fs_node_t* cwd;
    struct file* fds[MAX_FDS];
    uint8_t  cloexec[MAX_FDS];
    struct {
        uint32_t handler;          /* 0 = SIG_DFL, 1 = SIG_IGN, else user fn */
        uint32_t flags;            /* SA_* */
        uint32_t restorer;         /* musl's __restore / __restore_rt */
        uint64_t mask;
    } sa[NSIG_MAX];
    uint64_t sig_mask;             /* Linux sigset: bit (sig-1) */
    uint64_t sig_pending;          /* caught signals waiting for delivery */
    uint64_t saved_mask;           /* sigsuspend: mask to restore after the handler */
    bool     in_sigsuspend;
    uint32_t alarm_at;             /* uptime ms of pending SIGALRM, 0 = none */
    uint32_t alarm_interval;       /* setitimer reload, ms */
    int      exit_status;          /* wait(2) encoding once zombie */
    int      umask;
    bool     kernel_waited;        /* launched by the kernel shell, which reaps it */
    char     name[32];
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
bool    proc_alive(int pid);          /* still running (not zombie/free) */
int     proc_reap(int pid);           /* free a kernel-launched zombie, returns wait status */
void    proc_kill_all(void);          /* terminate every user process */

proc_t* proc_current(void);           /* NULL on kernel tasks */
proc_t* proc_by_pid(int pid);
int     proc_pid(proc_t* p);
uint32_t proc_tls_base(proc_t* p);
bool    proc_interrupted(void);       /* current process has a fatal signal pending */
void    proc_signal_group(int pgid, int sig);
int     proc_send_signal(proc_t* p, int sig);
int     proc_count(void);
proc_t* proc_at(int i);

/* Fault plumbing called from the exception handlers. */
bool    proc_handle_fault(uint32_t addr, uint32_t err);
void    proc_fault_kill(const char* what, int sig, uint32_t eip, uint32_t addr) __attribute__((noreturn));

/* Syscall-level operations (syscall.c calls these). */
int     proc_fork(regs_t* r);
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
int     proc_sigreturn(regs_t* r, bool rt);
int     proc_sigaction(int sig, const uint32_t* act, uint32_t* oact, bool old_abi);
int     proc_sigsuspend(const uint64_t* mask);
void    proc_check_alarm(proc_t* p, bool from_irq);   /* fire SIGALRM when due */
void    proc_account_tick(proc_t* p, bool user);

/* procfs.c: rebuild /proc from live kernel state (called on /proc lookups). */
void    procfs_refresh(void);

#endif
