#include "paging.h"
#include "idt.h"
#include "vga.h"
#include "io.h"

/* Single page directory, 4 MiB PSE pages, identity map of the full 4 GiB. */
__attribute__((aligned(4096)))
static uint32_t pdir[1024];

/* PDE flags for a 4 MiB page: Present | RW | PageSize. */
#define PDE_P   0x001u
#define PDE_RW  0x002u
#define PDE_PS  0x080u

__attribute__((interrupt))
static void pf_isr(struct interrupt_frame* f, uint32_t err) {
    uint32_t fault_addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));
    vga_printf("\n[#PF] addr=0x%x err=0x%x eip=0x%x cs=0x%x\n",
               fault_addr, err, f->eip, f->cs);
    cli();
    for (;;) hlt();
}

void paging_init(void) {
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t phys = i << 22;                 /* 4 MiB stride */
        pdir[i] = phys | PDE_P | PDE_RW | PDE_PS;
    }

    idt_set_gate(14, pf_isr, 0x08, 0x8E);        /* #PF */

    __asm__ volatile (
        "mov %0, %%cr3\n\t"
        "mov %%cr4, %%eax\n\t"
        "or  $0x00000010, %%eax\n\t"             /* CR4.PSE */
        "mov %%eax, %%cr4\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80010000, %%eax\n\t"             /* CR0.PG | CR0.WP */
        "mov %%eax, %%cr0\n\t"
        : : "r"(pdir) : "eax", "memory"
    );
}

uint32_t paging_cr0(void) { uint32_t r; __asm__ volatile ("mov %%cr0, %0":"=r"(r)); return r; }
uint32_t paging_cr3(void) { uint32_t r; __asm__ volatile ("mov %%cr3, %0":"=r"(r)); return r; }
uint32_t paging_cr4(void) { uint32_t r; __asm__ volatile ("mov %%cr4, %0":"=r"(r)); return r; }
