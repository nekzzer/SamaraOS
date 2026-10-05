/* ap startup, copied to 0x8000 by smp_init. everything is addressed by hand
   (T + offset) since this is linked up in the kernel but runs down there */
.set T, 0x8000
.text
.code16
.global tramp_start, tramp_end, tramp_cr3, tramp_stack
tramp_start:
    cli
    cld
    mov %cs, %ax
    mov %ax, %ds
    lgdtl (tgdt_ptr - tramp_start)
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $0x08, $(T + (tp32 - tramp_start))

.code32
tp32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov (T + (tramp_cr3 - tramp_start)), %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $0x20, %eax                  /* PAE */
    mov %eax, %cr4
    mov $0xC0000080, %ecx
    rdmsr
    or $0x101, %eax                 /* LME + SCE */
    wrmsr
    mov %cr0, %eax
    or $0x80010001, %eax            /* PG | WP | PE */
    mov %eax, %cr0
    ljmp $0x18, $(T + (tp64 - tramp_start))

.code64
tp64:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    mov (T + (tramp_stack - tramp_start)), %rsp
    xor %ebp, %ebp
    movabs $ap_main, %rax
    call *%rax
1:  hlt
    jmp 1b

.balign 8
tgdt:
    .quad 0
    .quad 0x00CF9A000000FFFF        /* 32 bit code */
    .quad 0x00CF92000000FFFF        /* data */
    .quad 0x00AF9A000000FFFF        /* 64 bit code */
tgdt_ptr:
    .word 31
    .long T + (tgdt - tramp_start)
.balign 8
tramp_cr3:   .quad 0
tramp_stack: .quad 0
tramp_end:
