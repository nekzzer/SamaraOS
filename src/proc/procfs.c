#include "boot/gdt.h"
/* /proc, rebuilt from live kernel state.

   The files are ordinary ramfs nodes under /proc that are regenerated on
   every path lookup / directory read that touches /proc (see syscall.c), so
   procps-style tools (busybox ps, top, free, uptime, pidof, ...) read what
   the kernel sees at that moment. Nodes are updated in place; directories
   of exited processes are unlinked (open descriptors keep them alive). */

#include "proc/proc.h"
#include "proc/file.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/vmm.h"
#include "core/clock.h"
#include "boot/pit.h"
#include "fs/fatfs.h"
#include "net/net.h"
#include "core/prof.h"

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
    if (p->pt_state == 1) return 't';
    return p == proc_current() ? 'R' : 'S';
}

static int nthreads(proc_t* p) {
    int n = 0;
    for (int i = 0; i < proc_count(); i++) {
        proc_t* q = proc_at(i);
        if (q && q->state == P_ALIVE && q->tgid == p->tgid) n++;
    }
    return n ? n : 1;
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
    sb_puts(&b, " 0 0 20 0 "); sb_int(&b, nthreads(p)); sb_puts(&b, " 0 ");   /* cutime cstime prio nice threads itreal */
    sb_num(&b, start); sb_putc(&b, ' ');
    sb_num(&b, vsz_kb * 1024); sb_putc(&b, ' ');
    sb_num(&b, pages);
    // startstack was 0x40000000, the very end: java's find_vma never matched [stack]
    sb_puts(&b, " 4294967295 134512640 134512640 1073737728 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
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

    {   // gdb opens /proc/pid/exe to see if the tracee is 64 bit
        proc_t* lp = proc_by_pid(p->tgid);
        const char* x = lp && lp->exe[0] ? lp->exe : p->exe;
        fs_node_t* ol = fs_child(d, "exe");
        if (ol && (ol->type != FS_LINK || strcmp(ol->data, x))) { remove_tree(ol); ol = NULL; }
        if (!ol && x[0]) fs_symlink(d, "exe", x);
    }

    b = (sb_t){ mem, 0, cap };                                /* df and friends read /proc/self/mounts (void's /etc/mtab) */
    sb_puts(&b, "rootfs / ramfs rw 0 0\nproc /proc proc rw 0 0\n");
    b.len += (uint32_t)fatfs_mounts_text(b.buf + b.len, (int)(b.cap - b.len));
    put(d, "mounts", &b);

    static const char* const long_state[] = { "R (running)", "S (sleeping)", "Z (zombie)", "t (tracing stop)" };
    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "Name:\t"); sb_puts(&b, p->name);
    sb_puts(&b, "\nUmask:\t0"); { char t[8]; utoa((uint32_t)p->sh->umask, t, 8); sb_puts(&b, t); }
    sb_puts(&b, "\nState:\t"); sb_puts(&b, long_state[s == 'R' ? 0 : s == 'S' ? 1 : s == 'Z' ? 2 : 3]);
    sb_puts(&b, "\nTgid:\t"); sb_int(&b, p->tgid);
    sb_puts(&b, "\nPid:\t"); sb_int(&b, p->pid);
    sb_puts(&b, "\nPPid:\t"); sb_int(&b, p->ppid);
    sb_puts(&b, "\nTracerPid:\t"); sb_int(&b, p->tracer);
    sb_puts(&b, "\nPtDbg:\t"); sb_int(&b, p->pt_state); sb_putc(&b, ' '); sb_int(&b, p->pt_stopsig); sb_putc(&b, ' '); sb_int(&b, p->pt_rep); sb_putc(&b, ' '); sb_int(&b, p->pt_event);
    sb_puts(&b, "\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nFDSize:\t64\n");
    int nfd = 0;
    for (int i = 0; i < MAX_FDS; i++) if (p->sh->fds[i]) nfd++;
    sb_puts(&b, "VmSize:\t"); sb_pad(&b, vsz_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "VmRSS:\t"); sb_pad(&b, vsz_kb, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "VmData:\t"); sb_pad(&b, (p->sh->brk - p->sh->brk_start) / 1024, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "Threads:\t"); sb_int(&b, nthreads(p)); sb_puts(&b, "\nOpenFDs:\t"); sb_int(&b, nfd); sb_putc(&b, '\n');
    put(d, "status", &b);

    b = (sb_t){ mem, 0, cap };
    char path[256];
    fs_path(p->sh->cwd, path, sizeof(path));
    sb_puts(&b, path); sb_putc(&b, '\n');
    put(d, "cwd", &b);

    // no io accounting, htop just wants the file
    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "rchar: 0\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n");
    put(d, "io", &b);

    /* maps: rough, from what we know without walking page tables (too slow,
       this runs on every /proc lookup). mmap area is one blob */
    b = (sb_t){ mem, 0, cap };
    char hx[24];
#define HX(v) do { utoa((v), hx, 16); for (int _i = (int)strlen(hx); _i < 8; _i++) sb_putc(&b, '0'); sb_puts(&b, hx); } while (0)
    if (p->sh->brk_start > 0x400000u) {
        HX(0x400000u); sb_putc(&b, '-'); HX(p->sh->brk_start);
        sb_puts(&b, " r-xp 00000000 00:00 0          "); sb_puts(&b, p->name); sb_putc(&b, '\n');
    }
    if (p->sh->brk > p->sh->brk_start) {
        HX(p->sh->brk_start); sb_putc(&b, '-'); HX((p->sh->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        sb_puts(&b, " rw-p 00000000 00:00 0          [heap]\n");
    }
    HX(USER_STACK_TOP - USER_STACK_MAX); sb_putc(&b, '-'); HX(USER_STACK_TOP);
    sb_puts(&b, " rw-p 00000000 00:00 0          [stack]\n");
#undef HX
    put(d, "maps", &b);

    /* task/<tid>/ for every thread of the group, htop reads the main one from there */
    if (strcmp(d->parent->name, "task")) {
        fs_node_t* t = ensure(d, "task", FS_DIR, 0555);
        if (!t) return;
        fs_node_t* c = t->child;
        while (c) {
            fs_node_t* next = c->next;
            proc_t* q = proc_by_pid(atoi(c->name));
            if (!q || q->tgid != p->tgid) remove_tree(c);
            c = next;
        }
        for (int i = 0; i < proc_count(); i++) {
            proc_t* q = proc_at(i);
            if (!q || q->tgid != p->tgid) continue;
            char name[12];
            itoa(q->pid, name, 10);
            fs_node_t* td = ensure(t, name, FS_DIR, 0555);
            if (td) fill_pid_dir(td, q, mem, cap);
        }
    }
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
    sb_puts(&b, "Buffers:               0 kB\n");
    sb_puts(&b, "Cached:         "); sb_pad(&b, heap_big_used() / 1024, 8); sb_puts(&b, " kB\n");   /* file data */
    sb_puts(&b, "SwapCached:            0 kB\n");
    sb_puts(&b, "KernelHeap:     "); sb_pad(&b, heap_total() / 1024, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "FileArena:      "); sb_pad(&b, heap_big_total() / 1024, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "UserPool:       "); sb_pad(&b, pmm_total_frames() * 4, 8); sb_puts(&b, " kB\n");
    sb_puts(&b, "SwapTotal:             0 kB\nSwapFree:              0 kB\nShmem:                 0 kB\n");
    sb_puts(&b, "SReclaimable:          0 kB\n");
    put(proc_root, "meminfo", &b);

    uint32_t up = pit_uptime_ms() / HZ_DIV;
    b = (sb_t){ mem, 0, cap };
    sb_frac(&b, up); sb_putc(&b, ' '); sb_frac(&b, cpus[0].t_idle / HZ_DIV); sb_putc(&b, '\n');
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

    b = (sb_t){ mem, 0, cap };
    b.len = (uint32_t)flk_text(mem, (int)cap);
    put(proc_root, "locks", &b);

    uint32_t us = 0, sy = 0, id = 0;
    for (int i = 0; i < ncpu; i++) { us += cpus[i].t_user; sy += cpus[i].t_sys; id += cpus[i].t_idle; }
    us /= HZ_DIV; sy /= HZ_DIV; id /= HZ_DIV;
    b = (sb_t){ mem, 0, cap };
    for (int k = 0; k <= ncpu; k++) {
        uint32_t u = us, s2 = sy, i2 = id;
        if (k) { u = cpus[k-1].t_user / HZ_DIV; s2 = cpus[k-1].t_sys / HZ_DIV; i2 = cpus[k-1].t_idle / HZ_DIV; sb_puts(&b, "cpu"); sb_int(&b, k - 1); sb_putc(&b, ' '); }
        else sb_puts(&b, "cpu  ");
        sb_num(&b, u); sb_puts(&b, " 0 "); sb_num(&b, s2); sb_putc(&b, ' ');
        sb_num(&b, i2); sb_puts(&b, " 0 0 0 0 0 0\n");
    }
    sb_puts(&b, "intr "); sb_num(&b, pit_ticks());
    sb_puts(&b, "\nctxt "); sb_num(&b, cpu_ctxt);
    sb_puts(&b, "\nbtime "); sb_num(&b, clock_epoch() - pit_uptime_ms() / 1000);
    sb_puts(&b, "\nprocesses "); sb_int(&b, last);
    sb_puts(&b, "\nprocs_running "); sb_int(&b, running ? running : 1);
    sb_puts(&b, "\nprocs_blocked 0\n");
    put(proc_root, "stat", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "Linux version 5.0.0-samara (SamaraOS 0.5) #1 x86_64\n");
    put(proc_root, "version", &b);

    b = (sb_t){ mem, 0, cap };
    for (int k = 0; k < ncpu; k++) {
        sb_puts(&b, "processor\t: "); sb_int(&b, k);
        sb_puts(&b, "\nvendor_id\t: SamaraOS\nmodel name\t: x86_64 (QEMU)\n"
                    "cpu MHz\t\t: 1000.000\nflags\t\t: fpu pse tsc fxsr\nbogomips\t: 2000.00\n\n");
    }
    put(proc_root, "cpuinfo", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "rootfs / ramfs rw 0 0\nproc /proc proc rw 0 0\n");
    b.len += (uint32_t)fatfs_mounts_text(b.buf + b.len, (int)(b.cap - b.len));
    put(proc_root, "mounts", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "nodev\tramfs\nnodev\tproc\n\tvfat\n");
    put(proc_root, "filesystems", &b);

    fs_node_t* nd = ensure(proc_root, "net", FS_DIR, 0555);
    if (!nd) return;
    static const char hx[] = "0123456789abcdef";
    b = (sb_t){ mem, 0, cap };
    for (int i = -1; i < net_ifcount(); i++) {
        uint8_t a6[A6_MAX][16], pl[A6_MAX], sc[A6_MAX];
        const char* name = "lo"; const uint8_t* mac; uint32_t ip, mask, gw, rx, tx;
        if (i >= 0) net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
        int n = ip6_addrs(i, a6, pl, sc, A6_MAX);
        for (int k = 0; k < n; k++) {
            for (int j = 0; j < 16; j++) { sb_putc(&b, hx[a6[k][j] >> 4]); sb_putc(&b, hx[a6[k][j] & 15]); }
            sb_putc(&b, ' '); sb_putc(&b, hx[(i + 2) >> 4]); sb_putc(&b, hx[(i + 2) & 15]);
            sb_putc(&b, ' '); sb_putc(&b, hx[pl[k] >> 4]); sb_putc(&b, hx[pl[k] & 15]);
            sb_putc(&b, ' '); sb_putc(&b, hx[sc[k] >> 4]); sb_putc(&b, hx[sc[k] & 15]);
            sb_puts(&b, " 80 "); sb_puts(&b, name); sb_putc(&b, '\n');
        }
    }
    put(nd, "if_inet6", &b);

    b = (sb_t){ mem, 0, cap };
    ip6_route_t r[16];
    int nr = ip6_routes(r, 16);
    for (int k = 0; k < nr; k++) {
        const uint8_t* addrs[2] = { r[k].dst, r[k].gw };
        for (int w = 0; w < 2; w++) {
            for (int j = 0; j < 16; j++) { sb_putc(&b, hx[addrs[w][j] >> 4]); sb_putc(&b, hx[addrs[w][j] & 15]); }
            if (!w) { sb_putc(&b, ' '); sb_putc(&b, hx[r[k].dlen >> 4 & 15]); sb_putc(&b, hx[r[k].dlen & 15]); sb_puts(&b, " "); }
            if (!w) { for (int j = 0; j < 32; j++) sb_putc(&b, '0'); sb_puts(&b, " 00 "); }
        }
        const char* name = "lo";
        if (r[k].ifi >= 0) { const uint8_t* mac; uint32_t ip, mask, gw, rx, tx; net_ifinfo(r[k].ifi, &name, &mac, &ip, &mask, &gw, &rx, &tx); }
        sb_puts(&b, " 00000000 00000000 00000000 ");
        for (int j = 7; j >= 0; j--) sb_putc(&b, hx[r[k].flags >> (j * 4) & 15]);
        sb_putc(&b, ' '); sb_puts(&b, name); sb_putc(&b, '\n');
    }
    put(nd, "ipv6_route", &b);

    b = (sb_t){ mem, 0, cap };
    sb_puts(&b, "Inter-|   Receive                                                |  Transmit\n"
                " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    for (int i = -1; i < net_ifcount(); i++) {
        const char* name = "lo"; const uint8_t* mac; uint32_t ip, mask, gw, rx = 0, tx = 0;
        if (i >= 0) net_ifinfo(i, &name, &mac, &ip, &mask, &gw, &rx, &tx);
        sb_puts(&b, "  "); sb_puts(&b, name); sb_puts(&b, ": 0 ");
        sb_num(&b, rx); sb_puts(&b, " 0 0 0 0 0 0 0 "); sb_num(&b, tx); sb_puts(&b, " 0 0 0 0 0 0\n");
    }
    put(nd, "dev", &b);

    fs_node_t* sn = ensure(proc_root, "sys", FS_DIR, 0555);
    if (sn && (sn = ensure(sn, "net", FS_DIR, 0555)) && (sn = ensure(sn, "ipv4", FS_DIR, 0555))) {
        b = (sb_t){ mem, 0, cap };
        sb_puts(&b, "0\t2147483647\n");
        put(sn, "ping_group_range", &b);
    }
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
            if (!p || p->is_thread) remove_tree(c);     // threads live in task/ only
        }
        c = next;
    }

    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (!p || p->is_thread) continue;
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
    {
        fs_node_t* sd = ensure(proc_root, "samara", FS_DIR, 0555);
        // only rebuilt for a read: dump is big
        static int shown = -1;
        if (sd && prof_on) {
            sb_t pb = { "running\n", 8, 9 };
            put(sd, "prof", &pb);
            shown = -1;
        } else if (sd && shown != prof_gen) {
            shown = prof_gen;
            sb_t pb = { kmalloc(300000), 0, 300000 };
            if (pb.buf) {
                pb.len = prof_dump(pb.buf, pb.cap);
                put(sd, "prof", &pb);
                kfree(pb.buf);
            }
        }
        if (sd) sd->mode = 0555;
        fs_node_t* pn = sd ? fs_child(sd, "prof") : NULL;
        if (pn) pn->mode = 0666;
    }
    irq_restore(f);
}
