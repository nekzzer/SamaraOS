#include "core/vmm.h"
#include "core/swap.h"
#include "core/pcache.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/io.h"
#include "core/smp.h"
#include "proc/proc.h"

/* memory pressure: below `low` free frames the next syscall or fault reclaims up to `high`
   (page cache first, then anon pages to swap). nothing left to take: oom kill.
   all of it runs with the big lock held, so spaces can't die under the scan */
static uint64_t low, high, crit;
static uint64_t hand[MAX_PROCS];
static int hand_pid[MAX_PROCS];
static int rr;
static uint8_t* bounce;
static int busy;
uint32_t mem_oom_kills, mem_swapouts;

static void klog(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }
static void klogn(uint64_t v) { char b[24]; utoa(v, b, 10); klog(b); }

void mem_init(void) {
    uint64_t t = pmm_total_frames();
    low = t / 50;
    high = t / 25;
    crit = t / 200;
    if (low < 256) low = 256;
    if (high < 512) high = 512;
    if (crit < 64) crit = 64;
    bounce = kmalloc(SWB_MAX * PAGE_SIZE);
}

uint64_t mem_low(void) { return low; }

// swapped pages of area a come back, for swapoff. -1 = no memory
int vmm_swap_drain(int a) {
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* p = proc_at(i);
        if (!p || p->is_thread || !p->pd || p->vfork_shared) continue;
        uint64_t va = 0;
        while ((va = vmm_swap_find(p->pd, va, a))) {
            if (!vmm_swap_in(p->pd, va)) return -1;
            va += PAGE_SIZE;
        }
    }
    return 0;
}

static int flush(swb_t* b, int n) {
    for (int i = 0; i < n; ) {
        int j = i + 1;
        while (j < n && b[j].slot == b[j - 1].slot + 1) j++;
        for (int k = i; k < j; k++) memcpy(bounce + (k - i) * PAGE_SIZE, P2V(b[k].fr), PAGE_SIZE);
        if (swap_write(b[i].slot, j - i, bounce) < 0)
            for (int k = i; k < j; k++) { swap_put(b[k].slot); pmm_unref(b[k].fr); b[k].fr = 0; }
        i = j;
    }
    // write failures dropped their frame above, take them out before commit
    int m = 0;
    for (int i = 0; i < n; i++) if (b[i].fr) b[m++] = b[i];
    int got = vmm_swap_commit(b, m);
    mem_swapouts += got;
    return got;
}

static uint64_t swap_out(uint64_t want) {
    uint64_t got = 0;
    // 3 sweeps: the first one only clears accessed bits on a busy system
    for (int round = 0; round < 3 * MAX_PROCS && got < want && swap_free(); round++) {
        int i = (rr + round) % MAX_PROCS;
        proc_t* p = proc_at(i);
        if (!p || p->is_thread || !p->pd || p->vfork_shared || p->state != P_ALIVE) continue;
        if (hand_pid[i] != p->pid) { hand_pid[i] = p->pid; hand[i] = 0; }
        for (int scans = 0; scans < 1024 && got < want && swap_free(); scans++) {
            swb_t b[SWB_MAX];
            int n = 0;
            int end = vmm_swap_scan(p->pd, &hand[i], 2048, b, &n);
            if (n) got += flush(b, n);
            if (end) { hand[i] = 0; break; }
        }
    }
    rr = (rr + 1) % MAX_PROCS;
    return got;
}

static bool oom_exempt(proc_t* p) {
    return p->pid <= 1 || !strcmp(p->name, "init") || !strcmp(p->name, "samara-wl") || !strcmp(p->name, "Xwayland");
}

static proc_t* oom_pick(void) {
    proc_t* best = NULL;
    uint64_t bs = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t* p = proc_at(i);
        if (!p || p->state != P_ALIVE || p->is_thread || !p->pd || oom_exempt(p)) continue;
        uint64_t s = vmm_count_pages(p->pd);
        if (s > bs) { bs = s; best = p; }
    }
    return best;
}

uint64_t mem_reclaim(void) {
    if (busy) return 0;
    uint64_t fl = irq_save();
    struct cpu* c = this_cpu();
    int took = !c->bkl, nb = c->cur->nobkl;
    if (took) bkl_take(c);
    busy = 1;
    uint64_t fr = pmm_free_frames(), got = 0;
    uint64_t want = fr < high ? high - fr : 0;
    if (want) {
        got = pc_reclaim(want);
        if (got < want && swap_active()) got += swap_out(want - got);
    }
    busy = 0;
    if (took) { bkl_drop(c); c->cur->nobkl = nb; }
    irq_restore(fl);
    return got;
}

// true when it is worth retrying the allocation
bool mem_oom(void) {
    proc_t* me = proc_current();
    for (int t = 0; t < 3; t++) {
        mem_reclaim();
        if (pmm_free_frames() > crit) return true;
    }
    proc_t* v = oom_pick();
    if (!v) return false;
    klog("oom: kill pid "); klogn(v->pid); klog(" ("); klog(v->name); klog(") rss ");
    klogn(vmm_count_pages(v->pd) * 4); klog("k\r\n");
    mem_oom_kills++;
    proc_send_signal(v, 9);
    if (v == me || (me && v->tgid == me->tgid)) return false;
    // let it die, its pages come back when the space goes
    // irqs are off in the fault path, open them a bit so the victim can run
    for (int i = 0; i < 4000000 && pmm_free_frames() <= crit; i++) __asm__ volatile ("sti; pause; cli");
    return pmm_free_frames() > 0;
}

void mem_check(void) {
    if (pmm_free_frames() >= low) return;
    mem_reclaim();
    if (pmm_free_frames() < crit) mem_oom();
}
