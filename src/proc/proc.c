#include "gui/uwin.h"
#include "proc/proc.h"
#include "proc/file.h"
#include "proc/tty.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/vmm.h"
#include "core/io.h"
#include "boot/gdt.h"
#include "boot/pit.h"

#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define E2BIG   7
#define ENOEXEC 8
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EISDIR  21
#define EINVAL  22

static proc_t procs[MAX_PROCS];
static int    next_pid = 1;

/* ---------------- serial log (debugging aid) ---------------- */

static void com_putc(char c) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, c); }
static void klog(const char* s) { while (*s) com_putc(*s++); }
static void klog_num(uint32_t v, int base) { char b[16]; utoa(v, b, base); klog(b); }

/* ---------------- lookup ---------------- */

proc_t* proc_current(void) { return task_current()->proc; }
int     proc_pid(proc_t* p) { return p ? p->pid : 0; }
uint32_t proc_tls_base(proc_t* p) { return p->tls_base; }
int     proc_count(void) { return MAX_PROCS; }
proc_t* proc_at(int i) { return (i >= 0 && i < MAX_PROCS && procs[i].state != P_FREE) ? &procs[i] : NULL; }

proc_t* proc_by_pid(int pid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != P_FREE && procs[i].pid == pid) return &procs[i];
    return NULL;
}

bool proc_alive(int pid) {
    proc_t* p = proc_by_pid(pid);
    return p && p->state == P_ALIVE;
}

static proc_t* alloc_proc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != P_FREE) continue;
        proc_t* p = &procs[i];
        memset(p, 0, sizeof(*p));
        p->state = P_ALIVE;
        p->pid = next_pid++;
        p->task = -1;
        p->start_ms = pit_uptime_ms();
        p->umask = 022;
        return p;
    }
    return NULL;
}

void proc_init(void) {
    memset(procs, 0, sizeof(procs));
    tty_init();
}

/* ---------------- argument vectors ---------------- */

typedef struct {
    int    argc, envc;
    char** argv;
    char** envp;
    char*  mem;
} kargs_t;

#define ARGS_MAX (128u * 1024u)

static void kargs_free(kargs_t* a) { if (a->mem) kfree(a->mem); a->mem = NULL; }

/* Deep-copy (optional prefix) + argv + envp into one kernel block. The
   source pointers may be user memory of the running process. */
static int kargs_build(kargs_t* a, const char* const* pre, int npre,
                       char* const argv[], int skip, char* const envp[]) {
    int argc = 0, envc = 0;
    uint32_t bytes = 0;
    for (int i = 0; i < npre; i++) bytes += strlen(pre[i]) + 1;
    if (argv) for (; argv[argc]; argc++) {
        if (argc >= skip) bytes += strlen(argv[argc]) + 1;
        if (bytes > ARGS_MAX) return -E2BIG;
    }
    if (envp) for (; envp[envc]; envc++) {
        bytes += strlen(envp[envc]) + 1;
        if (bytes > ARGS_MAX) return -E2BIG;
    }
    int nargs = npre + (argc > skip ? argc - skip : 0);
    uint32_t ptrs = (uint32_t)(nargs + 1 + envc + 1) * sizeof(char*);
    char* mem = (char*)kmalloc(ptrs + bytes + 1);
    if (!mem) return -ENOMEM;
    char** pv = (char**)mem;
    char* sp = mem + ptrs;
    int k = 0;
    for (int i = 0; i < npre; i++) { pv[k++] = sp; strcpy(sp, pre[i]); sp += strlen(sp) + 1; }
    for (int i = skip; i < argc; i++) { pv[k++] = sp; strcpy(sp, argv[i]); sp += strlen(sp) + 1; }
    pv[k++] = NULL;
    char** ev = pv + k;
    for (int i = 0; i < envc; i++) { ev[i] = sp; strcpy(sp, envp[i]); sp += strlen(sp) + 1; }
    ev[envc] = NULL;
    a->argc = nargs;
    a->envc = envc;
    a->argv = pv;
    a->envp = ev;
    a->mem = mem;
    return 0;
}

/* ---------------- ELF loading ---------------- */

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf_ehdr_t;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed)) elf_phdr_t;

#define PT_LOAD 1
#define PT_PHDR 6
#define PF_W    2

static bool elf_ok(const fs_node_t* n) {
    if (n->size < sizeof(elf_ehdr_t)) return false;
    const elf_ehdr_t* h = (const elf_ehdr_t*)n->data;
    return h->ident[0] == 0x7F && h->ident[1] == 'E' && h->ident[2] == 'L' && h->ident[3] == 'F' &&
           h->ident[4] == 1 /*32-bit*/ && h->type == 2 /*EXEC*/ && h->machine == 3 /*i386*/ &&
           h->phoff + (uint32_t)h->phnum * sizeof(elf_phdr_t) <= n->size;
}

typedef struct {
    uint32_t entry, brk, phdr, phnum;
} image_t;

static int load_elf(uint32_t pd, const fs_node_t* n, image_t* img) {
    const elf_ehdr_t* h = (const elf_ehdr_t*)n->data;
    const elf_phdr_t* ph = (const elf_phdr_t*)(n->data + h->phoff);
    uint32_t top = 0;
    img->phdr = 0;
    for (int i = 0; i < h->phnum; i++) {
        const elf_phdr_t* p = &ph[i];
        if (p->type == PT_PHDR) img->phdr = p->vaddr;
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        if (p->vaddr < USER_BASE || p->vaddr + p->memsz > USER_MMAP_BASE ||
            p->filesz > p->memsz || p->offset + p->filesz > n->size) return -ENOEXEC;
        if (vmm_alloc_range(pd, p->vaddr, p->memsz, true) < 0) return -ENOMEM;
        vmm_copy_to(pd, p->vaddr, n->data + p->offset, p->filesz);
        vmm_copy_to(pd, p->vaddr + p->filesz, NULL, p->memsz - p->filesz);
        if (p->vaddr + p->memsz > top) top = p->vaddr + p->memsz;
        if (!img->phdr && p->offset == 0) img->phdr = p->vaddr + h->phoff;
    }
    /* Text read-only (so fork can share it), then re-open writable segments
       in case one shares a page with text. */
    for (int i = 0; i < h->phnum; i++)
        if (ph[i].type == PT_LOAD && !(ph[i].flags & PF_W))
            vmm_set_writable(pd, ph[i].vaddr, ph[i].memsz, false);
    for (int i = 0; i < h->phnum; i++)
        if (ph[i].type == PT_LOAD && (ph[i].flags & PF_W))
            vmm_set_writable(pd, ph[i].vaddr, ph[i].memsz, true);
    img->entry = h->entry;
    img->brk = (top + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    img->phnum = h->phnum;
    return 0;
}

/* auxv keys */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

static uint32_t rnd_seed = 0x1234567;
static uint32_t rnd32(void) {
    rnd_seed ^= pit_ticks() * 2654435761u;
    rnd_seed ^= rnd_seed << 13; rnd_seed ^= rnd_seed >> 17; rnd_seed ^= rnd_seed << 5;
    return rnd_seed;
}

/* Lay out argc/argv/envp/auxv + strings at the top of the new stack. */
static int build_stack(uint32_t pd, const kargs_t* a, const image_t* img, uint32_t* sp_out) {
    uint32_t strbytes = 0;
    for (int i = 0; i < a->argc; i++) strbytes += strlen(a->argv[i]) + 1;
    for (int i = 0; i < a->envc; i++) strbytes += strlen(a->envp[i]) + 1;
    strbytes += 5 + 16;                                   /* "i686" + AT_RANDOM bytes */
    int naux = 16;
    uint32_t words = 1 + (uint32_t)a->argc + 1 + (uint32_t)a->envc + 1 + (uint32_t)naux * 2;
    uint32_t total = ((strbytes + 15) & ~15u) + words * 4 + 16;
    total = (total + 15) & ~15u;
    if (total > 192u * 1024u) return -E2BIG;

    uint32_t map = (total + 64u * 1024u + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (vmm_alloc_range(pd, USER_STACK_TOP - map, map, true) < 0) return -ENOMEM;

    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return -ENOMEM;
    memset(buf, 0, total);
    uint32_t base = USER_STACK_TOP - total;               /* user address of buf[0] */
    uint32_t* w = (uint32_t*)buf;
    uint32_t spos = total - ((strbytes + 15) & ~15u);     /* strings start (offset) */

#define PUTSTR(s) ({ uint32_t _ua = base + spos; uint32_t _l = strlen(s) + 1; \
                     memcpy(buf + spos, (s), _l); spos += _l; _ua; })
    int k = 0;
    w[k++] = (uint32_t)a->argc;
    for (int i = 0; i < a->argc; i++) w[k++] = PUTSTR(a->argv[i]);
    w[k++] = 0;
    for (int i = 0; i < a->envc; i++) w[k++] = PUTSTR(a->envp[i]);
    w[k++] = 0;
    uint32_t plat = PUTSTR("i686");
    uint32_t rnd = base + spos;
    for (int i = 0; i < 16; i += 4) { uint32_t r = rnd32(); memcpy(buf + spos + i, &r, 4); }
    spos += 16;
#undef PUTSTR
    uint32_t execfn = a->argc ? w[1] : 0;
    const uint32_t aux[][2] = {
        { AT_PHDR, img->phdr }, { AT_PHENT, sizeof(elf_phdr_t) }, { AT_PHNUM, img->phnum },
        { AT_PAGESZ, PAGE_SIZE }, { AT_ENTRY, img->entry }, { AT_UID, 0 }, { AT_EUID, 0 },
        { AT_GID, 0 }, { AT_EGID, 0 }, { AT_HWCAP, 0 }, { AT_CLKTCK, 100 }, { AT_SECURE, 0 },
        { AT_RANDOM, rnd }, { AT_PLATFORM, plat }, { AT_EXECFN, execfn }, { AT_NULL, 0 },
    };
    for (unsigned i = 0; i < sizeof(aux) / sizeof(aux[0]); i++) { w[k++] = aux[i][0]; w[k++] = aux[i][1]; }

    int r = vmm_copy_to(pd, base, buf, total);
    kfree(buf);
    if (r < 0) return -ENOMEM;
    *sp_out = base;
    return 0;
}

static void init_user_frame(regs_t* r, uint32_t entry, uint32_t sp) {
    memset(r, 0, sizeof(*r));
    r->gs = r->fs = r->es = r->ds = GDT_UDATA;
    r->cs = GDT_UCODE;
    r->ss = GDT_UDATA;
    r->eip = entry;
    r->useresp = sp;
    r->eflags = 0x202;
}

/* Resolve `path`, follow up to two "#!" levels, load the ELF into a fresh
   address space. On success *pd_out/frame are ready and `a` holds the final
   argv (caller frees). Nothing about the calling process changes. */
static void save_cmdline(proc_t* p, const kargs_t* a) {
    uint32_t n = 0;
    for (int i = 0; i < a->argc; i++) {
        uint32_t l = strlen(a->argv[i]) + 1;
        if (n + l > sizeof(p->cmdline)) break;
        memcpy(p->cmdline + n, a->argv[i], l);
        n += l;
    }
    p->cmdline_len = (uint16_t)n;
}

static int load_program(fs_node_t* cwd, const char* path, kargs_t* a,
                        uint32_t* pd_out, regs_t* frame, uint32_t* brk_out,
                        char* name_out) {
    char cur[256];
    strncpy(cur, path, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    fs_node_t* n = NULL;
    for (int depth = 0;; depth++) {
        n = fs_resolve(cwd, cur);
        if (!n) return -ENOENT;
        if (n->type == FS_DIR) return -EACCES;
        if (!(n->mode & 0111) || n->dev) return -EACCES;
        if (n->size >= 2 && n->data[0] == '#' && n->data[1] == '!') {
            if (depth >= 2) return -ENOEXEC;
            char interp[128], arg[128];
            int i = 2, j = 0;
            while (i < (int)n->size && (n->data[i] == ' ' || n->data[i] == '\t')) i++;
            while (i < (int)n->size && n->data[i] != ' ' && n->data[i] != '\t' &&
                   n->data[i] != '\n' && j < 127) interp[j++] = n->data[i++];
            interp[j] = 0;
            while (i < (int)n->size && (n->data[i] == ' ' || n->data[i] == '\t')) i++;
            j = 0;
            while (i < (int)n->size && n->data[i] != '\n' && j < 127) arg[j++] = n->data[i++];
            while (j > 0 && (arg[j - 1] == ' ' || arg[j - 1] == '\t' || arg[j - 1] == '\r')) j--;
            arg[j] = 0;
            if (!interp[0]) return -ENOEXEC;
            const char* pre[3];
            int np = 0;
            pre[np++] = interp;
            if (arg[0]) pre[np++] = arg;
            pre[np++] = cur;
            kargs_t b;
            int r = kargs_build(&b, pre, np, a->argv, 1, a->envp);
            if (r < 0) return r;
            kargs_free(a);
            *a = b;
            strncpy(cur, interp, sizeof(cur) - 1);
            continue;
        }
        break;
    }
    if (!elf_ok(n)) return -ENOEXEC;

    uint32_t pd = vmm_new_space();
    if (!pd) return -ENOMEM;
    image_t img;
    int r = load_elf(pd, n, &img);
    uint32_t sp = 0;
    if (r == 0) r = build_stack(pd, a, &img, &sp);
    if (r < 0) { vmm_destroy_space(pd); return r; }
    init_user_frame(frame, img.entry, sp);
    *pd_out = pd;
    *brk_out = img.brk;

    const char* base = path;              /* the applet, not its interpreter */
    for (const char* s = base; *s; s++) if (*s == '/' && s[1]) base = s + 1;
    strncpy(name_out, base, 31);
    name_out[31] = 0;
    return 0;
}

static int start_task(proc_t* p, const regs_t* frame) {
    uint8_t* ks = (uint8_t*)kmalloc(KSTACK_SZ);
    if (!ks) return -ENOMEM;
    regs_t* r = (regs_t*)(ks + KSTACK_SZ - sizeof(regs_t));
    *r = *frame;
    int t = task_spawn_frame(p->name, ks, KSTACK_SZ, (uint32_t)r, p->pd, p);
    if (t < 0) { kfree(ks); return -EAGAIN; }
    p->task = t;
    return 0;
}

/* ---------------- spawn from the kernel shell ---------------- */

int proc_spawn(const char* path, char* const argv[], char* const envp[]) {
    extern fs_node_t* cwd;                               /* shell's cwd */
    kargs_t a;
    int r = kargs_build(&a, NULL, 0, argv, 0, envp);
    if (r < 0) return r;
    proc_t* p = alloc_proc();
    if (!p) { kargs_free(&a); return -EAGAIN; }
    regs_t frame;
    save_cmdline(p, &a);
    r = load_program(cwd, path, &a, &p->pd, &frame, &p->brk_start, p->name);
    kargs_free(&a);
    if (r < 0) { p->state = P_FREE; return r; }
    p->brk = p->brk_start;
    p->ppid = 0;
    p->pgid = p->sid = p->pid;
    p->kernel_waited = true;
    p->cwd = cwd ? cwd : fs_root();
    file_t* t = file_new(F_TTY, 2 /*O_RDWR*/);
    if (!t) { vmm_destroy_space(p->pd); p->state = P_FREE; return -ENOMEM; }
    p->fds[0] = p->fds[1] = p->fds[2] = t;
    t->refs = 3;
    tty_reset();
    tty_set_fg_pgrp(p->pgid);
    r = start_task(p, &frame);
    if (r < 0) {
        file_close(t); file_close(t); file_close(t);
        vmm_destroy_space(p->pd);
        p->state = P_FREE;
        return r;
    }
    return p->pid;
}

int proc_reap(int pid) {
    proc_t* p = proc_by_pid(pid);
    if (!p || p->state != P_ZOMBIE) return -1;
    int st = p->exit_status;
    p->state = P_FREE;
    return st;
}

/* ---------------- teardown ---------------- */

static void free_or_zombify(proc_t* p) {
    proc_t* parent = p->ppid ? proc_by_pid(p->ppid) : NULL;
    if (!p->kernel_waited && (!parent || parent->state != P_ALIVE)) p->state = P_FREE;
    else p->state = P_ZOMBIE;
}

/* Release everything of a process that is not running right now, or of the
   current one right before it switches away for good. */
static void teardown(proc_t* p, int status) {
    uwin_proc_exit(p->pid);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) { file_close(p->fds[i]); p->fds[i] = NULL; }
    }
    task_t* t = task_at(p->task);
    if (t && t->proc == p) {
        t->proc = NULL;
        t->cr3 = 0;
        t->state = T_DEAD;
    }
    if (p->pd) {
        if (p == proc_current() || (t && t == task_current())) task_set_cr3(0);
        vmm_destroy_space(p->pd);
        p->pd = 0;
    }
    /* Orphans: children lose their parent; finished ones go away. */
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* c = &procs[i];
        if (c->state == P_FREE || c->ppid != p->pid) continue;
        c->ppid = 0;
        if (c->state == P_ZOMBIE && !c->kernel_waited) c->state = P_FREE;
    }
    p->exit_status = status;
    free_or_zombify(p);
    if (p->state == P_ZOMBIE && p->ppid) {
        proc_t* parent = proc_by_pid(p->ppid);
        if (parent && parent->state == P_ALIVE && parent->sa[17].handler > 1)
            parent->sig_pending |= SIGBIT(17);                  /* SIGCHLD */
    }
    if (tty_fg_pgrp() == p->pgid && p->kernel_waited) tty_set_fg_pgrp(0);
}

void proc_exit(int status) {
    cli();
    proc_t* p = proc_current();
    if (p) teardown(p, status);
    else task_current()->state = T_DEAD;
    for (;;) task_yield();
}

void proc_kill_all(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_ALIVE) proc_send_signal(&procs[i], 9);
}

/* ---------------- signals ---------------- */

static bool sig_default_ignored(int sig) {
    return sig == 17 /*CHLD*/ || sig == 18 /*CONT*/ || sig == 23 /*URG*/ || sig == 28 /*WINCH*/ ||
           sig == 19 || sig == 20 || sig == 21 || sig == 22;   /* stop signals: no job stop */
}

int proc_send_signal(proc_t* p, int sig) {
    if (!p || p->state != P_ALIVE) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 0 || sig >= NSIG_MAX) return -EINVAL;
    if (sig != 9) {
        uint32_t h = p->sa[sig].handler;
        if (h == 1) return 0;                               /* SIG_IGN */
        if (h > 1) { p->sig_pending |= SIGBIT(sig); return 0; }   /* caught: delivered on return to ring 3 */
        if (sig_default_ignored(sig)) return 0;
    }
    uint32_t f = irq_save();
    if (p == proc_current()) {
        irq_restore(f);
        proc_exit(sig & 0x7F);
    }
    /* Not running: it sits either in user mode or at a yield point inside a
       syscall, both safe to tear down from here. */
    teardown(p, sig & 0x7F);
    irq_restore(f);
    return 0;
}

void proc_signal_group(int pgid, int sig) {
    proc_t* me = proc_current();
    bool self = false;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* p = &procs[i];
        if (p->state != P_ALIVE || p->pgid != pgid) continue;
        if (p == me) { self = true; continue; }
        proc_send_signal(p, sig);
    }
    if (self) proc_send_signal(me, sig);
}

/* A blocking syscall gives up with EINTR when a caught signal is waiting;
   fatal signals tear the process down directly instead. */
bool proc_interrupted(void) {
    proc_t* p = proc_current();
    if (!p) return false;
    proc_check_alarm(p, false);
    return proc_signal_deliverable(p);
}

/* ---------------- faults ---------------- */

bool proc_handle_fault(uint32_t addr, uint32_t err) {
    proc_t* p = proc_current();
    if (!p || (err & 1)) return false;                   /* protection faults are real */
    if (addr < USER_STACK_TOP - USER_STACK_MAX || addr >= USER_STACK_TOP) return false;
    return vmm_alloc_range(p->pd, addr & ~(PAGE_SIZE - 1), PAGE_SIZE, true) == 0;
}

void proc_fault_kill(const char* what, int sig, uint32_t eip, uint32_t addr) {
    proc_t* p = proc_current();
    klog("[proc] pid ");
    klog_num(p ? (uint32_t)p->pid : 0, 10);
    klog(" ("); klog(p ? p->name : "?"); klog("): "); klog(what);
    klog(" eip=0x"); klog_num(eip, 16);
    klog(" addr=0x"); klog_num(addr, 16);
    klog("\r\n");
    if (!p) { cli(); for (;;) hlt(); }
    proc_exit(sig & 0x7F);
}

/* ---------------- fork / exec / wait ---------------- */

int proc_fork(regs_t* r) {
    proc_t* parent = proc_current();
    proc_t* c = alloc_proc();
    if (!c) return -EAGAIN;
    c->pd = vmm_clone_space(parent->pd);
    if (!c->pd) { c->state = P_FREE; return -ENOMEM; }
    c->ppid = parent->pid;
    c->pgid = parent->pgid;
    c->sid = parent->sid;
    c->brk_start = parent->brk_start;
    c->brk = parent->brk;
    c->tls_base = parent->tls_base;
    c->cwd = parent->cwd;
    c->umask = parent->umask;
    c->sig_mask = parent->sig_mask;         /* alarms are not inherited */
    memcpy(c->sa, parent->sa, sizeof(c->sa));
    memcpy(c->name, parent->name, sizeof(c->name));
    memcpy(c->cmdline, parent->cmdline, sizeof(c->cmdline));
    c->cmdline_len = parent->cmdline_len;
    for (int i = 0; i < MAX_FDS; i++) {
        c->fds[i] = parent->fds[i];
        c->cloexec[i] = parent->cloexec[i];
        file_ref(c->fds[i]);
    }
    regs_t child = *r;
    child.eax = 0;
    int e = start_task(c, &child);
    if (e < 0) {
        for (int i = 0; i < MAX_FDS; i++) { file_close(c->fds[i]); c->fds[i] = NULL; }
        vmm_destroy_space(c->pd);
        c->state = P_FREE;
        return e;
    }
    return c->pid;
}

int proc_execve(regs_t* r, const char* path, char* const argv[], char* const envp[]) {
    proc_t* p = proc_current();
    kargs_t a;
    char kpath[256];
    strncpy(kpath, path, sizeof(kpath) - 1);
    kpath[sizeof(kpath) - 1] = 0;
    int e = kargs_build(&a, NULL, 0, argv, 0, envp);
    if (e < 0) return e;
    proc_t tmp;
    save_cmdline(&tmp, &a);
    uint32_t pd, brk;
    regs_t frame;
    char name[32];
    e = load_program(p->cwd, kpath, &a, &pd, &frame, &brk, name);
    kargs_free(&a);
    if (e < 0) return e;

    uint32_t old = p->pd;
    p->pd = pd;
    task_set_cr3(pd);
    vmm_destroy_space(old);
    p->brk_start = p->brk = brk;
    p->tls_base = 0;
    gdt_set_tls(0);
    memcpy(p->name, name, sizeof(p->name));
    memcpy(p->cmdline, tmp.cmdline, sizeof(p->cmdline));
    p->cmdline_len = tmp.cmdline_len;
    strncpy(task_current()->name, name, 31);
    for (int i = 0; i < NSIG_MAX; i++)
        if (p->sa[i].handler > 1) { p->sa[i].handler = 0; p->sa[i].flags = 0; p->sa[i].mask = 0; }
    p->sig_pending = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i] && p->cloexec[i]) { file_close(p->fds[i]); p->fds[i] = NULL; }
        p->cloexec[i] = 0;
    }
    *r = frame;
    return 0;
}

int proc_wait(int pid, int* status, int options) {
    proc_t* me = proc_current();
    for (;;) {
        bool have = false;
        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t* c = &procs[i];
            if (c->state == P_FREE || c->ppid != me->pid) continue;
            if (pid > 0 && c->pid != pid) continue;
            if (pid == 0 && c->pgid != me->pgid) continue;
            if (pid < -1 && c->pgid != -pid) continue;
            have = true;
            if (c->state == P_ZOMBIE) {
                if (status) *status = c->exit_status;
                int cp = c->pid;
                c->state = P_FREE;
                return cp;
            }
        }
        if (!have) return -ECHILD;
        if (options & 1 /*WNOHANG*/) return 0;
        if (proc_interrupted()) return -EINTR;
        task_yield();
    }
}

void proc_account_tick(proc_t* p, bool user) {
    if (user) p->utime++; else p->stime++;
}
