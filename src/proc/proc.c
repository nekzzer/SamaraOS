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
static void klog_num(uint64_t v, int base) { char b[16]; utoa(v, b, base); klog(b); }

/* ---------------- lookup ---------------- */

proc_t* proc_current(void) { return task_current()->proc; }
int     proc_pid(proc_t* p) { return p ? p->pid : 0; }
uint64_t proc_tls_base(proc_t* p) { return p->tls_base; }
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
        p->tgid = p->pid;
        p->sh = &p->shd;
        p->task = -1;
        p->start_ms = pit_uptime_ms();
        p->sh->umask = 022;
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
    uint64_t bytes = 0;
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
    uint64_t ptrs = (uint64_t)(nargs + 1 + envc + 1) * sizeof(char*);
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
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) elf_phdr_t;

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_DYNAMIC 2
#define PT_PHDR 6
#define PF_W    2

static bool elf_ok(const fs_node_t* n) {
    if (n->size < sizeof(elf_ehdr_t)) return false;
    const elf_ehdr_t* h = (const elf_ehdr_t*)n->data;
    if (!(h->ident[0] == 0x7F && h->ident[1] == 'E' && h->ident[2] == 'L' && h->ident[3] == 'F' &&
          h->ident[4] == 2 /*64-bit*/ && h->machine == 62 /*x86_64*/ &&
          h->phoff + (uint64_t)h->phnum * sizeof(elf_phdr_t) <= n->size)) return false;
    /* ET_EXEC or ET_DYN. dynamic ones get their PT_INTERP loaded too */
    return h->type == 2 || h->type == 3;
}

/* path from PT_INTERP or NULL for static stuff */
static const char* elf_interp(const fs_node_t* n) {
    const elf_ehdr_t* h = (const elf_ehdr_t*)n->data;
    const elf_phdr_t* ph = (const elf_phdr_t*)(n->data + h->phoff);
    for (int i = 0; i < h->phnum; i++)
        if (ph[i].type == PT_INTERP && ph[i].offset + ph[i].filesz <= n->size)
            return (const char*)n->data + ph[i].offset;
    return NULL;
}

/* Where static-pie images (e.g. binutils from musl.cc) are put. */
#define PIE_BASE 0x555555554000ul

typedef struct {
    uint64_t entry, brk, phdr, phnum;
    uint64_t base, start;    // interp base + where to actually jump
} image_t;

static int load_elf(uint64_t pd, const fs_node_t* n, image_t* img, uint64_t bias) {
    const elf_ehdr_t* h = (const elf_ehdr_t*)n->data;
    const elf_phdr_t* ph = (const elf_phdr_t*)(n->data + h->phoff);
    uint64_t top = 0;
    // ld.so sits up in the mmap area, the program below it
    uint64_t lim = bias >= USER_MMAP_BASE ? USER_STACK_TOP - USER_STACK_MAX : USER_MMAP_BASE;
    if (h->type != 3) bias = 0;
    img->phdr = 0;
    for (int i = 0; i < h->phnum; i++) {
        const elf_phdr_t* p = &ph[i];
        uint64_t va = p->vaddr + bias;
        if (p->type == PT_PHDR) img->phdr = va;
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        if (va < USER_BASE || va + p->memsz > lim ||
            p->filesz > p->memsz || p->offset + p->filesz > n->size) return -ENOEXEC;
        if (vmm_alloc_range(pd, va, p->memsz, true) < 0) return -ENOMEM;
        vmm_copy_to(pd, va, n->data + p->offset, p->filesz);
        vmm_copy_to(pd, va + p->filesz, NULL, p->memsz - p->filesz);
        if (va + p->memsz > top) top = va + p->memsz;
        if (!img->phdr && p->offset == 0) img->phdr = va + h->phoff;
    }
    /* Text read-only (so fork can share it), then re-open writable segments
       in case one shares a page with text. A static-pie writes its own
       relocations into RELRO data, which lives in a writable segment. */
    // DT_TEXTREL (22) or DF_TEXTREL in DT_FLAGS: ld.so patches the text, leave it rw.
    // non-PIC .a in a pie does that (htop + ncurses crashed on it)
    bool textrel = false;
    for (int i = 0; i < h->phnum; i++) {
        if (ph[i].type != PT_DYNAMIC || ph[i].offset + ph[i].filesz > n->size) continue;
        const uint64_t* d = (const uint64_t*)(n->data + ph[i].offset);
        for (uint64_t k = 0; k + 1 < ph[i].filesz / 8 && d[k]; k += 2)
            if (d[k] == 22 || (d[k] == 30 && (d[k + 1] & 4))) textrel = true;
    }
    for (int i = 0; i < h->phnum && !textrel; i++)
        if (ph[i].type == PT_LOAD && !(ph[i].flags & PF_W))
            vmm_set_writable(pd, ph[i].vaddr + bias, ph[i].memsz, false);
    for (int i = 0; i < h->phnum; i++)
        if (ph[i].type == PT_LOAD && (ph[i].flags & PF_W))
            vmm_set_writable(pd, ph[i].vaddr + bias, ph[i].memsz, true);
    img->entry = h->entry + bias;
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
#define AT_BASE 7
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
static int build_stack(uint64_t pd, const kargs_t* a, const image_t* img, uint64_t* sp_out) {
    uint64_t strbytes = 0;
    for (int i = 0; i < a->argc; i++) strbytes += strlen(a->argv[i]) + 1;
    for (int i = 0; i < a->envc; i++) strbytes += strlen(a->envp[i]) + 1;
    strbytes += 7 + 16;                                   /* "x86_64" + AT_RANDOM bytes */
    int naux = 17;
    uint64_t words = 1 + (uint64_t)a->argc + 1 + (uint64_t)a->envc + 1 + (uint64_t)naux * 2;
    uint64_t total = ((strbytes + 15) & ~15ul) + words * 8 + 16;
    total = (total + 15) & ~15ul;
    if (total > 192u * 1024u) return -E2BIG;

    uint64_t map = (total + 64u * 1024u + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (vmm_alloc_range(pd, USER_STACK_TOP - map, map, true) < 0) return -ENOMEM;

    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return -ENOMEM;
    memset(buf, 0, total);
    uint64_t base = USER_STACK_TOP - total;               /* user address of buf[0] */
    uint64_t* w = (uint64_t*)buf;
    uint64_t spos = total - ((strbytes + 15) & ~15ul);    /* strings start (offset) */

#define PUTSTR(s) ({ uint64_t _ua = base + spos; uint64_t _l = strlen(s) + 1; \
                     memcpy(buf + spos, (s), _l); spos += _l; _ua; })
    int k = 0;
    w[k++] = (uint64_t)a->argc;
    for (int i = 0; i < a->argc; i++) w[k++] = PUTSTR(a->argv[i]);
    w[k++] = 0;
    for (int i = 0; i < a->envc; i++) w[k++] = PUTSTR(a->envp[i]);
    w[k++] = 0;
    uint64_t plat = PUTSTR("x86_64");
    uint64_t rnd = base + spos;
    for (int i = 0; i < 16; i += 4) { uint32_t r = rnd32(); memcpy(buf + spos + i, &r, 4); }
    spos += 16;
#undef PUTSTR
    uint64_t execfn = a->argc ? w[1] : 0;
    const uint64_t aux[][2] = {
        { AT_PHDR, img->phdr }, { AT_PHENT, sizeof(elf_phdr_t) }, { AT_PHNUM, img->phnum },
        { AT_PAGESZ, PAGE_SIZE }, { AT_BASE, img->base }, { AT_ENTRY, img->entry }, { AT_UID, 0 }, { AT_EUID, 0 },
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

static void init_user_frame(regs_t* r, uint64_t entry, uint64_t sp) {
    memset(r, 0, sizeof(*r));
    r->cs = GDT_UCODE;
    r->ss = GDT_UDATA;
    r->rip = entry;
    r->rsp = sp;
    r->rflags = 0x202;
}

/* Resolve `path`, follow up to two "#!" levels, load the ELF into a fresh
   address space. On success *pd_out/frame are ready and `a` holds the final
   argv (caller frees). Nothing about the calling process changes. */
static uint16_t cmdline_of(char* out, uint32_t cap, const kargs_t* a) {
    uint32_t n = 0;
    for (int i = 0; i < a->argc; i++) {
        uint32_t l = strlen(a->argv[i]) + 1;
        if (n + l > cap) break;
        memcpy(out + n, a->argv[i], l);
        n += l;
    }
    return (uint16_t)n;
}

static void save_cmdline(proc_t* p, const kargs_t* a) {
    p->cmdline_len = cmdline_of(p->cmdline, sizeof(p->cmdline), a);
}

static int load_program(fs_node_t* cwd, const char* path, kargs_t* a,
                        uint64_t* pd_out, regs_t* frame, uint64_t* brk_out,
                        char* name_out, char* exe_out) {
    char cur[256];
    strncpy(cur, path, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    fs_node_t* n = NULL;
    for (int depth = 0;; depth++) {
        n = fs_resolve(cwd, cur);
        if (!n) return -ENOENT;
        if (n->lazy) return -ENOMEM;                  /* still on disk: no memory to read it */
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
    fs_path(n, exe_out, 128);

    uint64_t pd = vmm_new_space();
    if (!pd) return -ENOMEM;
    image_t img;
    int r = load_elf(pd, n, &img, PIE_BASE);
    img.base = 0;
    img.start = img.entry;
    const char* ip = elf_interp(n);
    if (r == 0 && ip) {
        /* dynamic: load ld-musl too and start there, it maps the .so's itself */
        char ipath[128];
        strncpy(ipath, ip, sizeof(ipath) - 1);
        ipath[sizeof(ipath) - 1] = 0;
        fs_node_t* in = fs_resolve(fs_root(), ipath);
        image_t ii;
        if (in && in->lazy) r = -ENOMEM;
        else if (!in || in->type == FS_DIR || !elf_ok(in) || elf_interp(in)) r = -ENOENT;
        else {
            const elf_ehdr_t* h = (const elf_ehdr_t*)in->data;
            const elf_phdr_t* ph = (const elf_phdr_t*)(in->data + h->phoff);
            uint64_t span = 0;
            for (int i = 0; i < h->phnum; i++)
                if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > span) span = ph[i].vaddr + ph[i].memsz;
            span = (span + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            uint64_t b = vmm_find_free(pd, USER_MMAP_BASE, USER_STACK_TOP - USER_STACK_MAX, span);
            if (!b) r = -ENOMEM;
            else r = load_elf(pd, in, &ii, b);
            if (r == 0) { img.base = b; img.start = ii.entry; }
        }
        // kprintf("interp %s at %x\n", ipath, img.base);
    }
    uint64_t sp = 0;
    if (r == 0) r = build_stack(pd, a, &img, &sp);
    if (r < 0) { vmm_destroy_space(pd); return r; }
    init_user_frame(frame, img.start, sp);
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
    int t = task_spawn_frame(p->name, ks, KSTACK_SZ, (uint64_t)r, p->pd, p);
    if (t < 0) { kfree(ks); return -EAGAIN; }
    p->task = t;
    return 0;
}

/* ---------------- spawn from the kernel shell ---------------- */

static int spawn(const char* path, char* const argv[], char* const envp[], bool detached) {
    extern fs_node_t* cwd;                               /* shell's cwd */
    kargs_t a;
    int r = kargs_build(&a, NULL, 0, argv, 0, envp);
    if (r < 0) return r;
    proc_t* p = alloc_proc();
    if (!p) { kargs_free(&a); return -EAGAIN; }
    regs_t frame;
    save_cmdline(p, &a);
    r = load_program(cwd, path, &a, &p->pd, &frame, &p->sh->brk_start, p->name, p->exe);
    kargs_free(&a);
    if (r < 0) { p->state = P_FREE; return r; }
    p->sh->brk = p->sh->brk_start;
    p->ppid = 0;
    p->pgid = p->sid = p->pid;
    p->kernel_waited = !detached;
    p->tty_detached = detached;
    p->ctty = detached ? -1 : 0;
    p->sh->cwd = cwd ? cwd : fs_root();
    file_t* t = file_new(detached ? F_NULL : F_TTY, 2 /*O_RDWR*/);
    if (!t) { vmm_destroy_space(p->pd); p->state = P_FREE; return -ENOMEM; }
    p->sh->fds[0] = p->sh->fds[1] = p->sh->fds[2] = t;
    t->refs = 3;
    if (!detached) {
        tty_reset();
        tty_set_fg_pgrp(p->pgid);
    }
    r = start_task(p, &frame);
    if (r < 0) {
        file_close(t); file_close(t); file_close(t);
        vmm_destroy_space(p->pd);
        p->state = P_FREE;
        return r;
    }
    return p->pid;
}

int proc_spawn(const char* path, char* const argv[], char* const envp[]) {
    return spawn(path, argv, envp, false);
}

int proc_spawn_detached(const char* path, char* const argv[], char* const envp[]) {
    return spawn(path, argv, envp, true);
}

void proc_detach(int pid) {
    uint32_t f = irq_save();
    proc_t* p = proc_by_pid(pid);
    if (p && p->state == P_ZOMBIE && p->kernel_waited) {
        p->state = P_FREE;                        /* already done: nobody will reap it */
    } else if (p && p->state == P_ALIVE) {
        p->kernel_waited = false;
        p->tty_detached = true;
        if (tty_fg_pgrp() == p->pgid) tty_set_fg_pgrp(0);
    }
    irq_restore(f);
}

void proc_kill_session(int sid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_ALIVE && procs[i].sid == sid) proc_send_signal(&procs[i], 9);
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

static proc_t* leader_of(proc_t* p) {
    if (!p->is_thread) return p;
    proc_t* l = proc_by_pid(p->tgid);
    return l ? l : p;
}

/* Futex waiters: at most one per proc (a proc sleeps in one syscall). */
static struct { bool active, woken; uint64_t pd, addr; } fwq[MAX_PROCS];

void futex_forget(proc_t* p) {
    fwq[p - procs].active = false;
    p->in_futex = false;
}

static void ready_task_of(proc_t* p) {
    task_t* t = task_at(p->task);
    if (t && t->proc == p && t->state == T_BLOCKED) { t->wake_ms = 0; t->state = T_READY; }
}

void futex_wake_addr(uint64_t pd, uint64_t addr, int n) {
    uint32_t f = irq_save();
    for (int i = 0; i < MAX_PROCS && n > 0; i++) {
        if (!fwq[i].active || fwq[i].woken || fwq[i].pd != pd || fwq[i].addr != addr) continue;
        fwq[i].woken = true;
        ready_task_of(&procs[i]);
        n--;
    }
    irq_restore(f);
}

/* One thread (not the leader) goes away. It is either not running or it is
   the current one right before it switches away for good. */
static void thread_kill(proc_t* q) {
    if (q->clear_child_tid && q->pd) {
        uint32_t z = 0;
        vmm_copy_to(q->pd, q->clear_child_tid, &z, 4);
        futex_wake_addr(q->pd, q->clear_child_tid, 1);
    }
    futex_forget(q);
    task_t* t = task_at(q->task);
    if (t && t->proc == q) {
        t->proc = NULL;
        t->cr3 = 0;
        t->state = T_DEAD;
    }
    q->state = P_FREE;
}

/* Release everything of a process that is not running right now, or of the
   current one right before it switches away for good. Takes all threads of
   the group down with it. */
static void teardown(proc_t* p, int status) {
    p = leader_of(p);
    proc_t* cur = proc_current();
    bool cur_in = cur && cur->sh == p->sh;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* q = &procs[i];
        if (q != p && q->state != P_FREE && q->is_thread && q->tgid == p->pid) thread_kill(q);
    }
    p->zleader = false;
    futex_forget(p);
    uwin_proc_exit(p->pid);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->sh->fds[i]) { file_close(p->sh->fds[i]); p->sh->fds[i] = NULL; }
    }
    task_t* t = task_at(p->task);
    if (t && t->proc == p) {
        t->proc = NULL;
        t->cr3 = 0;
        t->state = T_DEAD;
    }
    if (p->pd) {
        if (cur_in || (t && t == task_current())) task_set_cr3(0);
        /* A vfork child still running on our space inherits it. */
        proc_t* heir = NULL;
        for (int i = 0; i < MAX_PROCS; i++)
            if (procs[i].state == P_ALIVE && procs[i].vfork_shared && procs[i].ppid == p->pid
                && procs[i].pd == p->pd) heir = &procs[i];
        if (p->vfork_shared) p->vfork_shared = false; /* borrowed from the parent */
        else if (heir) heir->vfork_shared = false;
        else vmm_destroy_space(p->pd);
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
        if (parent && parent->state == P_ALIVE && parent->sh->sa[17].handler > 1)
            parent->sig_pending |= SIGBIT(17);                  /* SIGCHLD */
    }
    if (tty_fg_pgrp() == p->pgid && p->kernel_waited) tty_set_fg_pgrp(0);
}

/* exit_group, fatal signals: the whole process */
void proc_exit(int status) {
    cli();
    proc_t* p = proc_current();
    if (p) teardown(p, status);
    else task_current()->state = T_DEAD;
    for (;;) task_yield();
}

/* plain exit(2): only the calling thread */
void proc_thread_exit(int status) {
    cli();
    proc_t* p = proc_current();
    proc_t* l = leader_of(p);
    int live = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* q = &procs[i];
        if (q != p && q->state == P_ALIVE && q->sh == p->sh && !q->zleader) live++;
    }
    if (!live) proc_exit(l->zleader ? l->exit_status : status);
    if (p == l) {
        /* leader quits first: keeps the slot, fds and memory for the threads */
        p->zleader = true;
        p->exit_status = status;
        futex_forget(p);
        task_t* t = task_current();
        t->proc = NULL;
        t->cr3 = 0;
        t->state = T_DEAD;
    } else {
        thread_kill(p);
    }
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

/* process-directed signal: give it to a thread that doesnt block it */
static proc_t* pick_thread(proc_t* l, int sig) {
    proc_t* any = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* q = &procs[i];
        if (q->state != P_ALIVE || q->sh != l->sh || q->zleader) continue;
        if (!any) any = q;
        if (!(q->sig_mask & SIGBIT(sig))) return q;
    }
    return any ? any : l;
}

static int send_sig(proc_t* p, int sig, bool exact);
int proc_send_signal(proc_t* p, int sig) { return send_sig(p, sig, false); }
/* tkill/tgkill: exactly this thread. qemu's coroutines hang otherwise */
int proc_send_signal_tid(proc_t* p, int sig) { return send_sig(p, sig, true); }

static int send_sig(proc_t* p, int sig, bool exact) {
    if (!p || p->state != P_ALIVE) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 0 || sig >= NSIG_MAX) return -EINVAL;
    if (sig != 9) {
        uint64_t h = p->sh->sa[sig].handler;
        if (h == 1) return 0;                               /* SIG_IGN */
        if (h > 1) {                                        /* caught: delivered on return to ring 3 */
            proc_t* d = p->is_thread || exact ? p : pick_thread(p, sig);
            d->sig_pending |= SIGBIT(sig);
            if (d->in_futex) ready_task_of(d);
            return 0;
        }
        if (sig_default_ignored(sig)) return 0;
    }
    uint32_t f = irq_save();
    proc_t* cur = proc_current();
    if (cur && cur->sh == p->sh) {
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
        if (p->state != P_ALIVE || p->pgid != pgid || p->is_thread) continue;
        if (me && p->sh == me->sh) { self = true; continue; }
        proc_send_signal(p, sig);
    }
    if (self) proc_send_signal(leader_of(me), sig);
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

bool proc_handle_fault(uint64_t addr, uint64_t err) {
    proc_t* p = proc_current();
    if (!p || (err & 1)) return false;                   /* protection faults are real */
    /* lazy anon page: frame now. PROT_NONE (no US) and writes to read-only
       ones stay faults, java counts on those SIGSEGVs */
    uint64_t pte = vmm_pte(p->pd, addr & ~(PAGE_SIZE - 1));
    if (pte & PTE_LAZY) {
        if (!(pte & PTE_US) || ((err & 2) && !(pte & PTE_RW))) return false;
        return vmm_fault_in(p->pd, addr);
    }
    if (addr < USER_STACK_TOP - USER_STACK_MAX || addr >= USER_STACK_TOP) return false;
    return vmm_alloc_range(p->pd, addr & ~(PAGE_SIZE - 1), PAGE_SIZE, true) == 0;
}

// top of the user stack on a segfault, addr2line the return addresses by hand.
// no gdb in here, this is how yutani got debugged
void proc_fault_stack(uint64_t esp) {
    proc_t* p = proc_current();
    if (!p) return;
    klog("[proc] stack @"); klog_num(esp, 16); klog(":");
    for (int i = 0; i < 128; i++) {
        uint64_t a = esp + i * 8;
        if (!(vmm_pte(p->pd, a) & PTE_P)) break;
        klog(i % 4 ? " " : "\r\n  "); klog_num(*(uint64_t*)a, 16);
    }
    klog("\r\n");
}

void proc_fault_kill(const char* what, int sig, uint64_t eip, uint64_t addr) {
    proc_t* p = proc_current();
    klog("[proc] pid ");
    klog_num(p ? (uint32_t)p->pid : 0, 10);
    klog(" ("); klog(p ? p->name : "?"); klog("): "); klog(what);
    klog(" rip=0x"); klog_num(eip, 16);
    klog(" addr=0x"); klog_num(addr, 16);
    klog("\r\n");
    if (!p) { cli(); for (;;) hlt(); }
    proc_exit(sig & 0x7F);
}

/* ---------------- fork / exec / wait ---------------- */

static int do_fork(regs_t* r, bool share) {
    proc_t* parent = proc_current();
    proc_t* c = alloc_proc();
    if (!c) return -EAGAIN;
    c->vfork_shared = share;
    c->pd = share ? parent->pd : vmm_clone_space(parent->pd);
    if (!c->pd) { c->state = P_FREE; return -ENOMEM; }
    c->ppid = parent->tgid;
    c->pgid = parent->pgid;
    c->sid = parent->sid;
    c->sh->brk_start = parent->sh->brk_start;
    c->sh->brk = parent->sh->brk;
    c->tls_base = parent->tls_base;
    c->sh->cwd = parent->sh->cwd;
    c->sh->umask = parent->sh->umask;
    c->tty_detached = parent->tty_detached;
    c->ctty = parent->ctty;
    c->sig_mask = parent->sig_mask;         /* alarms are not inherited */
    c->ss_sp = parent->ss_sp; c->ss_size = parent->ss_size;
    memcpy(c->sh->sa, parent->sh->sa, sizeof(c->sh->sa));
    memcpy(c->name, parent->name, sizeof(c->name));
    memcpy(c->exe, parent->exe, sizeof(c->exe));
    memcpy(c->cmdline, parent->cmdline, sizeof(c->cmdline));
    c->cmdline_len = parent->cmdline_len;
    for (int i = 0; i < MAX_FDS; i++) {
        c->sh->fds[i] = parent->sh->fds[i];
        c->sh->cloexec[i] = parent->sh->cloexec[i];
        file_ref(c->sh->fds[i]);
    }
    regs_t child = *r;
    child.rax = 0;
    int e = start_task(c, &child);
    if (e < 0) {
        for (int i = 0; i < MAX_FDS; i++) { file_close(c->sh->fds[i]); c->sh->fds[i] = NULL; }
        if (!share) vmm_destroy_space(c->pd);
        c->vfork_shared = false;
        c->state = P_FREE;
        return e;
    }
    int pid = c->pid;
    /* vfork: the child borrows our memory (and stack) until it execs or
       exits, so we must not run until then. */
    while (share && c->pid == pid && c->state == P_ALIVE && c->vfork_shared)
        task_yield();
    return pid;
}

int proc_fork(regs_t* r)  { return do_fork(r, false); }
int proc_vfork(regs_t* r) { return do_fork(r, true); }

#define CLONE_VM       0x100
#define CLONE_VFORK    0x4000
#define CLONE_THREAD   0x10000
#define CLONE_SETTLS   0x80000
#define CLONE_PARENT_SETTID  0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID   0x1000000

static bool uword_ok(uint64_t a) { return a >= USER_BASE && a < USER_TOP - 4 && !(a & 3); }

/* tid pointers from clone: only writable mapped pages, no raw stores */
static void put_tid(uint64_t pd, uint64_t a, int v) {
    if (uword_ok(a) && (vmm_pte(pd, a) & PTE_RW)) vmm_copy_to(pd, a, &v, 4);
}

int proc_clone(regs_t* r) {
    /* x86_64 order: flags, stack, ptid, ctid, tls */
    uint64_t fl = r->rdi, stk = r->rsi, ptid = r->rdx, ctid = r->r10, tls = r->r8;
    proc_t* parent = proc_current();
    if (!(fl & CLONE_THREAD)) {
        int pid = proc_fork(r);
        if (pid <= 0) return pid;
        proc_t* ch = proc_by_pid(pid);
        task_t* t = ch ? task_at(ch->task) : NULL;
        if (t && stk) ((regs_t*)t->rsp)->rsp = stk;
        if (fl & CLONE_PARENT_SETTID) put_tid(parent->pd, ptid, pid);
        if ((fl & CLONE_CHILD_SETTID) && ch) put_tid(ch->pd, ctid, pid);
        return pid;
    }
    if (!(fl & CLONE_VM)) return -EINVAL;
    proc_t* l = leader_of(parent);
    proc_t* c = alloc_proc();
    if (!c) return -EAGAIN;
    c->is_thread = true;
    c->tgid = l->pid;
    c->sh = l->sh;
    c->pd = parent->pd;
    c->ppid = 0;                            /* not waitable, getppid asks the leader */
    c->pgid = parent->pgid;
    c->sid = parent->sid;
    c->tty_detached = parent->tty_detached;
    c->ctty = parent->ctty;
    c->sig_mask = parent->sig_mask;
    c->tls_base = parent->tls_base;
    memcpy(c->name, parent->name, sizeof(c->name));
    memcpy(c->exe, parent->exe, sizeof(c->exe));
    memcpy(c->cmdline, parent->cmdline, sizeof(c->cmdline));
    c->cmdline_len = parent->cmdline_len;
    if (fl & CLONE_SETTLS) {
        c->tls_base = tls;                  /* x86_64: the value itself, not a user_desc */
    }
    if (fl & CLONE_PARENT_SETTID) put_tid(c->pd, ptid, c->pid);
    if (fl & CLONE_CHILD_SETTID) put_tid(c->pd, ctid, c->pid);
    if ((fl & CLONE_CHILD_CLEARTID) && uword_ok(ctid)) c->clear_child_tid = ctid;
    regs_t child = *r;
    child.rax = 0;
    if (stk) child.rsp = stk;
    int e = start_task(c, &child);
    if (e < 0) { c->state = P_FREE; return e; }
    return c->pid;
}

/* ---------------- futex ---------------- */

#define ETIMEDOUT 110

/* tmo: ms to wait, 0xFFFFFFFF = forever. cmd is op without PRIVATE/CLOCK bits. */
int futex_op(uint64_t uaddr, int op, uint32_t val, uint64_t tmo, uint64_t uaddr2, uint32_t val3) {
    proc_t* p = proc_current();
    int cmd = op & 127;
    if (!uword_ok(uaddr)) return -EFAULT;
    switch (cmd) {
    case 0: case 9: {                                   /* WAIT, WAIT_BITSET */
        uint32_t f = irq_save();
        if (*(volatile uint32_t*)uaddr != val) { irq_restore(f); return -EAGAIN; }
        uint32_t end = tmo == 0xFFFFFFFFu ? 0 : pit_uptime_ms() + (tmo ? tmo : 1);
        int w = p - procs;
        fwq[w].active = true; fwq[w].woken = false; fwq[w].pd = p->pd; fwq[w].addr = uaddr;
        p->in_futex = true;
        task_t* t = task_current();
        int ret;
        for (;;) {
            t->wake_ms = end;
            t->state = T_BLOCKED;
            while (t->state == T_BLOCKED) task_yield();
            if (fwq[w].woken) { ret = 0; break; }
            if (proc_signal_deliverable(p)) { ret = -EINTR; break; }
            if (end && (int32_t)(pit_uptime_ms() - end) >= 0) { ret = -ETIMEDOUT; break; }
        }
        futex_forget(p);
        irq_restore(f);
        return ret;
    }
    case 1: case 10: {                                  /* WAKE, WAKE_BITSET */
        int n = (int)val < 0 ? 0x7FFFFFFF : (int)val, k = 0;
        uint32_t f = irq_save();
        for (int i = 0; i < MAX_PROCS && k < n; i++) {
            if (!fwq[i].active || fwq[i].woken || fwq[i].pd != p->pd || fwq[i].addr != uaddr) continue;
            fwq[i].woken = true;
            ready_task_of(&procs[i]);
            k++;
        }
        irq_restore(f);
        return k;
    }
    case 3: case 4: {                                   /* REQUEUE, CMP_REQUEUE: tmo = val2 */
        if (!uword_ok(uaddr2)) return -EFAULT;
        uint32_t f = irq_save();
        if (cmd == 4 && *(volatile uint32_t*)uaddr != val3) { irq_restore(f); return -EAGAIN; }
        int n = (int)val < 0 ? 0x7FFFFFFF : (int)val, k = 0, moved = 0;
        int m = (int)tmo < 0 ? 0x7FFFFFFF : (int)tmo;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (!fwq[i].active || fwq[i].woken || fwq[i].pd != p->pd || fwq[i].addr != uaddr) continue;
            if (k < n) { fwq[i].woken = true; ready_task_of(&procs[i]); k++; }
            else if (moved < m) { fwq[i].addr = uaddr2; moved++; }
        }
        irq_restore(f);
        return k + moved;
    }
    }
    return -38;                                         /* ENOSYS: PI futexes, WAKE_OP */
}

int proc_execve(regs_t* r, const char* path, char* const argv[], char* const envp[]) {
    proc_t* p = proc_current();
    if (p->is_thread) return -EAGAIN;                /* FIXME exec from a non-leader thread */
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != P_FREE && procs[i].is_thread && procs[i].tgid == p->pid) thread_kill(&procs[i]);
    kargs_t a;
    char kpath[256];
    strncpy(kpath, path, sizeof(kpath) - 1);
    kpath[sizeof(kpath) - 1] = 0;
    int e = kargs_build(&a, NULL, 0, argv, 0, envp);
    if (e < 0) return e;
    char cmdline[sizeof(p->cmdline)];               /* not a whole proc_t: kernel stack */
    uint16_t cmdline_len = cmdline_of(cmdline, sizeof(cmdline), &a);
    uint64_t pd, brk;
    regs_t frame;
    char name[32], exe[128];
    e = load_program(p->sh->cwd, kpath, &a, &pd, &frame, &brk, name, exe);
    kargs_free(&a);
    if (e < 0) return e;

    uint64_t old = p->pd;
    p->pd = pd;
    task_set_cr3(pd);
    if (p->vfork_shared) p->vfork_shared = false;     /* old space is the parent's */
    else vmm_destroy_space(old);
    p->sh->brk_start = p->sh->brk = brk;
    p->tls_base = 0;
    wrmsr_fs(0);
    memcpy(p->name, name, sizeof(p->name));
    memcpy(p->exe, exe, sizeof(p->exe));
    memcpy(p->cmdline, cmdline, sizeof(p->cmdline));
    p->cmdline_len = cmdline_len;
    strncpy(task_current()->name, name, 31);
    for (int i = 0; i < NSIG_MAX; i++)
        if (p->sh->sa[i].handler > 1) { p->sh->sa[i].handler = 0; p->sh->sa[i].flags = 0; p->sh->sa[i].mask = 0; }
    p->sig_pending = 0;
    p->ss_sp = p->ss_size = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->sh->fds[i] && p->sh->cloexec[i]) { file_close(p->sh->fds[i]); p->sh->fds[i] = NULL; }
        p->sh->cloexec[i] = 0;
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
            if (c->state == P_FREE || c->ppid != me->tgid) continue;
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
