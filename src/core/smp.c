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

static volatile uint32_t tk_next = 1, tk_serve = 0;   // boot cpu owns ticket 0

void tlb_service(void) {
    struct cpu* c = this_cpu();
    if (c->tlb_req) {
        __atomic_store_n(&c->tlb_req, 0, __ATOMIC_SEQ_CST);
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    }
}

void bkl_take(struct cpu* c) {
    uint32_t t = __atomic_fetch_add(&tk_next, 1, __ATOMIC_ACQUIRE);
    while (__atomic_load_n(&tk_serve, __ATOMIC_ACQUIRE) != t) {
        tlb_service();
        __asm__ volatile ("pause");
    }
    c->bkl = 1;
}

void bkl_drop(struct cpu* c) {
    c->bkl = 0;
    __atomic_store_n(&tk_serve, tk_serve + 1, __ATOMIC_RELEASE);
}

void tlb_shootdown(void) {
    struct cpu* me = this_cpu();
    uint32_t mask = 0;
    for (int i = 0; i < ncpu; i++) {
        struct cpu* c = &cpus[i];
        if (c == me || !c->online) continue;
        __atomic_store_n(&c->tlb_req, 1, __ATOMIC_SEQ_CST);
        lapic_ipi(c->apic_id, VEC_TLB);
        mask |= 1 << i;
    }
    for (int i = 0; i < ncpu; i++)
        while ((mask & (1 << i)) && __atomic_load_n(&cpus[i].tlb_req, __ATOMIC_ACQUIRE)) __asm__ volatile ("pause");
}

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
