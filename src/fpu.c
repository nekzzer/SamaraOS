#include "fpu.h"
#include "idt.h"
#include "vga.h"
#include "io.h"

static int g_fpu = 0;

/* CR0 bits */
#define CR0_MP 0x00000002u
#define CR0_EM 0x00000004u
#define CR0_TS 0x00000008u
#define CR0_NE 0x00000020u

__attribute__((interrupt))
static void nm_isr(struct interrupt_frame* f) {
    /* Device-not-available. We're single-tasking the FPU, just clear TS. */
    (void)f;
    __asm__ volatile ("clts");
}

__attribute__((interrupt))
static void mf_isr(struct interrupt_frame* f) {
    uint16_t sw = 0;
    __asm__ volatile ("fnstsw %0" : "=m"(sw));
    vga_printf("\n[#MF] x87 fault, status=0x%x eip=0x%x\n", sw, f->eip);
    __asm__ volatile ("fnclex");
}

int fpu_present(void) { return g_fpu; }

void fpu_init(void) {
    /* Probe: with EM=0, FNINIT followed by FNSTSW must write the status word.
       Pre-set the test word to a sentinel and check it changes. */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |=  (CR0_MP | CR0_NE);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    uint16_t sw = 0x55AA;
    __asm__ volatile ("fninit; fnstsw %0" : "=m"(sw));
    g_fpu = (sw == 0);

    if (g_fpu) {
        /* Set default control word: mask all exceptions, 53-bit precision,
           round-to-nearest. 0x027F is the IA-32 default at reset, fine here. */
        uint16_t cw = 0x027F;
        __asm__ volatile ("fldcw %0" : : "m"(cw));
    }

    idt_set_gate(7,  nm_isr, 0x08, 0x8E);   /* #NM Device Not Available */
    idt_set_gate(16, mf_isr, 0x08, 0x8E);   /* #MF x87 FP Error */
}
