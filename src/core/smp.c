#include "core/smp.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/vmm.h"
#include "core/task.h"
#include "core/io.h"
#include "boot/acpi.h"
#include "boot/apic.h"
#include "boot/idt.h"
#include "boot/fpu.h"
#include "drivers/vga.h"

volatile uint32_t tk_next = 1, tk_serve = 0;   // boot cpu owns ticket 0

void tlb_service(void) {
    struct cpu* c = this_cpu();
    if (c->tlb_req) {
        __atomic_store_n(&c->tlb_req, 0, __ATOMIC_SEQ_CST);
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    }
    if (c->unload) {
        c->cr3 = task_kernel_cr3();
        __asm__ volatile ("mov %0, %%cr3" : : "r"(c->cr3) : "memory");
        __atomic_store_n(&c->unload, 0, __ATOMIC_SEQ_CST);
    }
}

/* about to free page tables: no other cpu may still have them in cr3, a miss
   would walk freed memory. a task of that space still in ring 3 there is dead
   already, the ipi path sends it to the reaper */
void tlb_unload(uint64_t pd) {
    struct cpu* me = this_cpu();
    uint32_t mask = 0;
    for (int i = 0; i < ncpu; i++) {
        struct cpu* c = &cpus[i];
        if (c == me || !c->online || c->cr3 != pd) continue;
        __atomic_store_n(&c->unload, 1, __ATOMIC_SEQ_CST);
        lapic_ipi(c->apic_id, VEC_TLB);
        mask |= 1 << i;
    }
    for (int i = 0; i < ncpu; i++)
        while ((mask & (1 << i)) && __atomic_load_n(&cpus[i].unload, __ATOMIC_ACQUIRE)) { tlb_service(); __asm__ volatile ("pause"); }
}

static void dbg(const char* m, int v);
static int bkl_cpu = -1;
static void* bkl_pc;

void bkl_take(struct cpu* c) {
    void* ra = __builtin_return_address(0);
    uint32_t t = __atomic_fetch_add(&tk_next, 1, __ATOMIC_ACQUIRE);
#ifdef LOCKDEP
    if (c->bkl) { dbg("lockdep: bkl twice, cpu ", c->id); dbg(" from ", (int)(uint64_t)ra); }
    uint32_t n = 0;
    uint64_t t0 = 0;
#endif
    while (__atomic_load_n(&tk_serve, __ATOMIC_ACQUIRE) != t) {
        tlb_service();
        __asm__ volatile ("pause");
#ifdef LOCKDEP
        if (!(++n & 0x3FFFFF) && (!t0 ? (t0 = tsc_ms(), 0) : tsc_ms() - t0 > 3000)) {
            dbg("lockdep: bkl hang, cpu ", c->id); dbg(" owner cpu ", bkl_cpu);
            dbg(" owner pc ", (int)(uint64_t)bkl_pc); dbg(" my pc ", (int)(uint64_t)ra);
            t0 = tsc_ms();
        }
#endif
    }
    c->bkl = 1;
    bkl_cpu = c->id;
    bkl_pc = ra;
}

uint64_t spin_lock(spin_t* l) {
    uint64_t f = irq_save();
    struct cpu* c = this_cpu();
    void* ra = __builtin_return_address(0);
#ifdef LOCKDEP
    if (l->v && l->cpu == c->id) {
        dbg("lockdep: recursive lock ", (int)(uint64_t)l); dbg(" cpu ", c->id);
        dbg(" held from ", (int)(uint64_t)l->pc); dbg(" again from ", (int)(uint64_t)ra);
    }
    uint32_t n = 0;
    uint64_t t0 = 0;
#endif
    while (__atomic_exchange_n(&l->v, 1, __ATOMIC_ACQUIRE)) {
        while (l->v) {
            tlb_service();
            __asm__ volatile ("pause");
#ifdef LOCKDEP
            if (!(++n & 0x3FFFFF) && (!t0 ? (t0 = tsc_ms(), 0) : tsc_ms() - t0 > 3000)) {
                dbg("lockdep: spin hang ", (int)(uint64_t)l); dbg(" cpu ", c->id);
                dbg(" owner cpu ", l->cpu); dbg(" owner pc ", (int)(uint64_t)l->pc);
                dbg(" my pc ", (int)(uint64_t)ra);
                t0 = tsc_ms();
            }
#endif
        }
    }
    l->cpu = c->id;
    l->pc = ra;
    return f;
}

void spin_unlock(spin_t* l, uint64_t f) {
    l->cpu = -1;
    __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

void bkl_drop(struct cpu* c) {
    c->bkl = 0;
    __atomic_store_n(&tk_serve, tk_serve + 1, __ATOMIC_RELEASE);
}

int bkl_enter(void) {
    uint64_t f = irq_save();
    struct cpu* c = this_cpu();
    int t = !c->bkl;
    if (t) { bkl_take(c); c->cur->nobkl = 0; }      // isr_leave would drop it again otherwise
    irq_restore(f);
    return t;
}

void bkl_leave(int taken) {
    if (!taken) return;
    uint64_t f = irq_save();
    this_cpu()->cur->nobkl = 1;
    bkl_drop(this_cpu());
    irq_restore(f);
}

/* we sit at a yield point: let a waiting cpu in, if any. ticket lock so it gets it */
void bkl_yield(struct cpu* c) {
    if (tk_next - tk_serve < 2) return;
    bkl_drop(c);
    bkl_take(c);
}

/* pd != 0: only the cpus that have that space loaded. whoever loads it later flushes by
   loading it. the fence pairs with the cr3 write on the other side */
void tlb_shootdown_pd(uint64_t pd) {
    struct cpu* me = this_cpu();
    uint32_t mask = 0;
    __sync_synchronize();
    for (int i = 0; i < ncpu; i++) {
        struct cpu* c = &cpus[i];
        if (c == me || !c->online || (pd && c->cr3 != pd)) continue;
        __atomic_store_n(&c->tlb_req, 1, __ATOMIC_SEQ_CST);
        lapic_ipi(c->apic_id, VEC_TLB);
        mask |= 1 << i;
    }
    for (int i = 0; i < ncpu; i++)
        while ((mask & (1 << i)) && __atomic_load_n(&cpus[i].tlb_req, __ATOMIC_ACQUIRE)) { tlb_service(); __asm__ volatile ("pause"); }
}

void tlb_shootdown(void) { tlb_shootdown_pd(0); }

void kick_idle(void) {
    struct cpu* me = this_cpu();
    for (int i = 0; i < ncpu; i++)
        if (&cpus[i] != me && cpus[i].online && cpus[i].in_idle) { lapic_ipi(cpus[i].apic_id, VEC_KICK); return; }
}

static void dbg(const char* m, int v) {
    while (*m) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m++); }
    for (int i = 28; i >= 0; i -= 4) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, "0123456789abcdef"[(v >> i) & 15]); }
    outb(0x3F8, '\n');
}

/* ---- ap startup ---- */

extern uint8_t tramp_start[], tramp_end[], tramp_cr3[], tramp_stack[];
extern uint64_t boot_pml4[], boot_pdpt_a[];
static uint64_t ap_pml4[512] __attribute__((aligned(4096)));
static volatile int ap_id;

void idt_load(void);

void ap_main(void) {
    int id = ap_id;
    dbg("ap ", id);
    gdt_init_ap(id);
    idt_load();
    __asm__ volatile ("mov %0, %%cr3" : : "r"(task_kernel_cr3()) : "memory");
    cpus[id].cr3 = task_kernel_cr3();
    fpu_cpu_init();
    apic_cpu_init();
    cpus[id].apic_id = lapic_id();
    task_ap_start(id);                    // never comes back
}

void smp_init(void) {
    dbg("apic_on ", apic_on); dbg("acpi ", acpi.ncpu);
    if (!apic_on || acpi.ncpu < 2) return;
    memcpy(ap_pml4, boot_pml4, 4096);
    ap_pml4[0] = V2P(boot_pdpt_a) | 3;       // low identity map for the trampoline
    uint8_t* low = (uint8_t*)P2V(0x8000);
    memcpy(low, tramp_start, tramp_end - tramp_start);
    *(uint32_t*)(low + (tramp_cr3 - tramp_start)) = (uint32_t)V2P(ap_pml4);
    int me = lapic_id(), n = 1;
    cpus[0].apic_id = me;
    for (int i = 0; i < acpi.ncpu && n < MAX_CPUS; i++) {
        int a = acpi.cpu_apic[i];
        if (a == me) continue;
        uint8_t* stk = kmalloc(16384);
        if (!stk) break;
        *(uint64_t*)(low + (tramp_stack - tramp_start)) = (uint64_t)stk + 16384;
        ap_id = n;
        cpus[n].apic_id = a;
        lapic_ipi_raw(a, 0x4500);               // INIT
        uint64_t t0 = tsc_ms();
        while (tsc_ms() - t0 < 10) __asm__ volatile ("pause");
        lapic_ipi_raw(a, 0x4608);               // SIPI, page 8 -> 0x8000
        t0 = tsc_ms();
        while (tsc_ms() - t0 < 2) __asm__ volatile ("pause");
        lapic_ipi_raw(a, 0x4608);
        t0 = tsc_ms();
        while (!cpus[n].online && tsc_ms() - t0 < 500) __asm__ volatile ("pause");
        if (cpus[n].online) n++;
        else vga_printf("cpu %d (apic %d) did not start\n", n, a);
    }
    dbg("started ", n);
    ncpu = n;
    vga_printf("smp: %d cpus\n", n);
}
