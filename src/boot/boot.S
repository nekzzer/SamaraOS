/* multiboot1 entry: 32 bit stub that builds boot page tables and jumps to long mode */
.set KOFF, 0xffffffff80000000

.section .multiboot, "a"
.align 4
.long 0x1BADB002, 3, -(0x1BADB002 + 3)

.section .boot.data, "aw"
.align 16
gdt64:
    .quad 0
    .quad 0x00209A0000000000        /* 64 bit code */
    .quad 0x0000920000000000        /* data */
gdt64_ptr:
    .word 23
    .long gdt64

.section .bss
.align 4096
.global boot_pml4, boot_pdpt_a, boot_pdpt_k, boot_pd
boot_pml4:   .skip 4096
boot_pdpt_a: .skip 4096
boot_pdpt_k: .skip 4096
boot_pd:     .skip 4096 * 4

.section .boot.text, "ax"
.code32
.global _start
_start:
    cli
    cld
    mov %eax, %edi
    mov %ebx, %esi

    /* 4 GiB of 2 MiB pages, same tables are the identity map, the direct map and the kernel image */
    mov $boot_pd_phys, %ebx
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $21, %eax
    or $0x83, %eax
    mov %eax, (%ebx, %ecx, 8)
    movl $0, 4(%ebx, %ecx, 8)
    inc %ecx
    cmp $2048, %ecx
    jne 1b

    mov $boot_pdpt_a_phys, %ebx
    mov $boot_pd_phys, %eax
    or $3, %eax
    mov $4, %ecx
2:  mov %eax, (%ebx)
    add $8, %ebx
    add $4096, %eax
    dec %ecx
    jnz 2b

    mov $boot_pdpt_k_phys, %ebx
    mov $boot_pd_phys, %eax
    or $3, %eax
    mov %eax, 510*8(%ebx)
    add $4096, %eax
    mov %eax, 511*8(%ebx)

    mov $boot_pml4_phys, %ebx
    mov $boot_pdpt_a_phys, %eax
    or $3, %eax
    mov %eax, (%ebx)
    mov %eax, 256*8(%ebx)
    mov $boot_pdpt_k_phys, %eax
    or $3, %eax
    mov %eax, 511*8(%ebx)

    mov %ebx, %cr3
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

    lgdt gdt64_ptr
    ljmp $0x08, $long_low

.code64
long_low:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    movabs $long_high, %rax
    jmp *%rax

.text
.section .text.start, "ax"
long_high:
    movabs $boot_stack + 4194304, %rsp
    xor %ebp, %ebp
    call kmain
1:  cli
    hlt
    jmp 1b
