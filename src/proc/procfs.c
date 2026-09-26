/* /proc, rebuilt from live kernel state.

   The files are ordinary ramfs nodes under /proc that are regenerated on
   every path lookup / directory read that touches /proc (see syscall.c), so
   procps-style tools (busybox ps, top, free, uptime, pidof, ...) read what
   the kernel sees at that moment. Nodes are updated in place; directories
   of exited processes are unlinked (open descriptors keep them alive). */

#include "proc/proc.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/vmm.h"
#include "core/clock.h"
#include "boot/pit.h"
#include "fs/fatfs.h"

extern uint32_t cpu_ticks_user, cpu_ticks_sys, cpu_ticks_idle, cpu_ctxt;

/* ---------------- tiny formatter ---------------- */

typedef struct { char* buf; uint32_t len, cap; } sb_t;

static void sb_putc(sb_t* b, char c) { if (b->len + 1 < b->cap) b->buf[b->len++] = c; }
static void sb_puts(sb_t* b, const char* s) { while (*s) sb_putc(b, *s++); }
static void sb_num(sb_t* b, uint32_t v) { char t[12]; utoa(v, t, 10); sb_puts(b, t); }
static void sb_int(sb_t* b, int32_t v) { char t[12]; itoa(v, t, 10); sb_puts(b, t); }
static void sb_pad(sb_t* b, uint32_t v, int w) {          /* right-aligned */
    char t[12]; utoa(v, t, 10);
    for (int i = (int)strlen(t); i < w; i++) sb_putc(b, ' ');
    sb_puts(b, t);
}
static void sb_frac(sb_t* b, uint32_t hundredths) {       /* "12.34" */
    sb_num(b, hundredths / 100);
    sb_putc(b, '.');
    sb_putc(b, (char)('0' + hundredths / 10 % 10));
    sb_putc(b, (char)('0' + hundredths % 10));
}

/* Linux USER_HZ: times in /proc are in 1/100 s; our PIT ticks are 1 ms. */
#define HZ_DIV 10

/* ---------------- node helpers ---------------- */

static fs_node_t* proc_root;

static fs_node_t* ensure(fs_node_t* dir, const char* name, fs_type_t type, uint16_t mode) {
    fs_node_t* n = fs_child(dir, name);
    if (n && n->type != type) { fs_detach(n); if (n->refs > 0) n->unlinked = true; else { fs_data_free(n); kfree(n); } n = NULL; }
    if (!n) {
        n = fs_create(dir, name, type);
        if (!n) return NULL;
    }
    n->mode = mode;
    return n;
}

static void put(fs_node_t* dir, const char* name, const sb_t* b) {
    fs_node_t* n = ensure(dir, name, FS_FILE, 0444);
    if (!n) return;
    fs_write(n, b->buf, b->len);
    n->mtime = clock_epoch();
}

static void remove_tree(fs_node_t* n) {
    while (n->child) remove_tree(n->child);
    fs_detach(n);
    if (n->refs > 0) { n->unlinked = true; return; }
    fs_data_free(n);
    kfree(n);
}

static bool is_pid_name(const char* s) {
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

/* ---------------- per-process files ---------------- */

static char state_of(proc_t* p) {
    if (p->state == P_ZOMBIE) return 'Z';
    return p == proc_current() ? 'R' : 'S';
}

static void fill_pid_dir(fs_node_t* d, proc_t* p, char* mem, uint32_t cap) {
    sb_t b;
    uint32_t pages = p->pd ? vmm_count_pages(p->pd) : 0;
    uint32_t vsz_kb = pages * 4;
    uint32_t start = p->start_ms / HZ_DIV;
    uint32_t ut = p->utime / HZ_DIV, st = p->stime / HZ_DIV;
    char s = state_of(p);

    /* stat: the 52 fields procps parsers expect, in order. */
    b = (sb_t){ mem, 0, cap };
    sb_int(&b, p->pid); sb_puts(&b, " ("); sb_puts(&b, p->name); sb_puts(&b, ") ");
    sb_putc(&b, s); sb_putc(&b, ' ');
    sb_int(&b, p->ppid); sb_putc(&b, ' ');
    sb_int(&b, p->pgid); sb_putc(&b, ' ');
    sb_int(&b, p->sid); sb_puts(&b, " 1025 ");                 /* tty_nr: tty1 */
    sb_int(&b, p->pgid); sb_puts(&b, " 4194560 0 0 0 0 ");     /* tpgid flags minflt.. */
    sb_num(&b, ut); sb_putc(&b, ' '); sb_num(&b, st);
    sb_puts(&b, " 0 0 20 0 1 0 ");                             /* cutime cstime prio nice threads itreal */
    sb_num(&b, start); sb_putc(&b, ' ');
    sb_num(&b, vsz_kb * 1024); sb_putc(&b, ' ');
    sb_num(&b, pages);
    sb_puts(&b, " 4294967295 134512640 134512640 1073741824 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    put(d, "stat", &b);

    b = (sb_t){ mem, 0, cap };
    sb_num(&b, pages); sb_putc(&b, ' '); sb_num(&b, pages);
    sb_puts(&b, " 0 0 0 0 0\n");
    put(d, "statm", &b);

    b = (sb_t){ mem, 0, cap };
    for (uint32_t i = 0; i < p->cmdline_len; i++) sb_putc(&b, p->cmdline[i]);
    put(d, "cmdline", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, p->name); sb_putc(&b, '\n');
    put(d, "comm", &b);

    static const char* const long_state[] = { "R (running)", "S (sleeping)", "Z (zombie)" };
    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "Name:\t"); sb_puts(&b, p->name);
    sb_puts(&b, "\nUmask:\t0"); { char t[8]; utoa((uint32_t)p->umask, t, 8); sb_puts(&b, t); }
    sb_puts(&b, "\nState:\t"); sb_puts(&b, long_state[s == 'R' ? 0 : s == 'S' ? 1 : 2]);
    sb_puts(&b, "\nTgid:\t"); sb_int(&b, p->pid);
    sb_puts(&b, "\nPid:\t"); sb_int(&b, p->pid);
    sb_puts(&b, "\nPPid:\t"); sb_int(&b, p->ppid);
    sb_puts(&b, "\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nFDSize:\t64\n");
    int nfd = 0;
    for (int i = 0; i < MAX_FDS; i++) if (p->fds[i]) nfd++;
    sb_puts(&b, "VmSize:\t"); sb_pad(&b, vsz_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "VmRSS:\t"); sb_pad(&b, vsz_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "VmData:\t"); sb_pad(&b, (p->brk - p->brk_start) / 1024, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "Threads:\t1\nOpenFDs:\t"); sb_int(&b, nfd); sb_putc(&b, '\n');
    put(d, "status", &b);

    b = (sb_t){ mem, 0, cap };
    char path[256];
    fs_path(p->cwd, path, sizeof(path));
    sb_puts(&b, path); sb_putc(&b, '\n');
    put(d, "cwd", &b);
}

/* ---------------- global files ---------------- */

static void fill_globals(char* mem, uint32_t cap) {
    sb_t b;
    uint32_t heap_free = heap_total() - heap_used();
    uint32_t total_kb = (heap_total() + pmm_total_frames() * PAGE_SIZE) / 1024;
    uint32_t free_kb = (heap_free + pmm_free_frames() * PAGE_SIZE) / 1024;

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "MemTotal:       "); sb_pad(&b, total_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "MemFree:        "); sb_pad(&b, free_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "MemAvailable:   "); sb_pad(&b, free_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n");
    sb_puts(&b, "KernelHeap:     "); sb_pad(&b, heap_total() / 1024, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "UserPool:       "); sb_pad(&b, pmm_total_frames() * 4, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "SwapTotal:             0 kB\nSwapFree:              0 kB\nShmem:                 0 kB\n");
    sb_puts(&b, "SReclaimable:          0 kB\n");
    put(proc_root, "meminfo", &b);

    uint32_t up = pit_uptime_ms() / HZ_DIV;
    b = (sb_t){ mem, 0, cap };
    sb_frac(&b, up); sb_putc(&b, ' '); sb_frac(&b, cpu_ticks_idle / HZ_DIV); sb_putc(&b, '\n');
    put(proc_root, "uptime", &b);

    int nproc = 0, running = 0, last = 0;
    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (!p) continue;
        nproc++;
        if (p->state == P_ALIVE && p == proc_current()) running++;
        if (p->pid > last) last = p->pid;
    }
    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "0.00 0.00 0.00 "); sb_int(&b, running ? running : 1); sb_putc(&b, '/');
    sb_int(&b, nproc); sb_putc(&b, ' '); sb_int(&b, last); sb_putc(&b, '\n');
    put(proc_root, "loadavg", &b);

    uint32_t us = cpu_ticks_user / HZ_DIV, sy = cpu_ticks_sys / HZ_DIV, id = cpu_ticks_idle / HZ_DIV;
    b = (sb_t){ mem, 0, cap };
    for (int k = 0; k < 2; k++) {
        sb_puts(&b, k ? "cpu0 " : "cpu  ");
        sb_num(&b, us); sb_puts(&b, " 0 "); sb_num(&b, sy); sb_putc(&b, ' ');
        sb_num(&b, id); sb_puts(&b, " 0 0 0 0 0 0\n");
    }
    sb_puts(&b, "intr "); sb_num(&b, pit_ticks());
    sb_puts(&b, "\nctxt "); sb_num(&b, cpu_ctxt);
    sb_puts(&b, "\nbtime "); sb_num(&b, clock_epoch() - pit_uptime_ms() / 1000);
    sb_puts(&b, "\nprocesses "); sb_int(&b, last);
    sb_puts(&b, "\nprocs_running "); sb_int(&b, running ? running : 1);
    sb_puts(&b, "\nprocs_blocked 0\n");
    put(proc_root, "stat", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "Linux version 5.0.0-samara (SamaraOS 0.5) #1 i686\n");
    put(proc_root, "version", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "processor\t: 0\nvendor_id\t: SamaraOS\nmodel name\t: i686 (QEMU)\n"
                "cpu MHz\t\t: 1000.000\nflags\t\t: fpu pse tsc fxsr\nbogomips\t: 2000.00\n\n");
    put(proc_root, "cpuinfo", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "rootfs / ramfs rw 0 0\nproc /proc proc rw 0 0\n");
    b.len += (uint32_t)fatfs_mounts_text(b.buf + b.len, (int)(b.cap - b.len));
    put(proc_root, "mounts", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "nodev\tramfs\nnodev\tproc\n\tvfat\n");
    put(proc_root, "filesystems", &b);
}

/* ---------------- refresh ---------------- */

void procfs_refresh(void) {
    uint32_t f = irq_save();
    proc_root = fs_resolve(fs_root(), "/proc");
    if (!proc_root) proc_root = fs_create(fs_root(), "/proc", FS_DIR);
    if (!proc_root) { irq_restore(f); return; }

    const uint32_t cap = 4096;
    char* mem = (char*)kmalloc(cap);
    if (!mem) { irq_restore(f); return; }

    /* Drop directories of processes that are gone. */
    fs_node_t* c = proc_root->child;
    while (c) {
        fs_node_t* next = c->next;
        if (c->type == FS_DIR && is_pid_name(c->name)) {
            proc_t* p = proc_by_pid(atoi(c->name));
            if (!p) remove_tree(c);
        }
        c = next;
    }

    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (!p) continue;
        char name[12];
        itoa(p->pid, name, 10);
        fs_node_t* d = ensure(proc_root, name, FS_DIR, 0555);
        if (d) fill_pid_dir(d, p, mem, cap);
    }

    /* /proc/self: a copy of the caller's directory (no symlinks here). */
    proc_t* me = proc_current();
    fs_node_t* self = fs_child(proc_root, "self");
    if (me) {
        fs_node_t* d = ensure(proc_root, "self", FS_DIR, 0555);
        if (d) fill_pid_dir(d, me, mem, cap);
    } else if (self) {
        remove_tree(self);
    }

    fill_globals(mem, cap);
    kfree(mem);
    irq_restore(f);
}
