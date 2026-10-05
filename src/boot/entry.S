/* interrupt stubs, syscall entry. every path builds the same regs_t frame
   (see core/task.h) and leaves through iretq, even syscalls */

.altmacro
.macro isr_stub n
.balign 16
.if (\n == 8) || (\n == 10) || (\n == 11) || (\n == 12) || (\n == 13) || (\n == 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    pushq $\n
.else
    pushq $0
    pushq $\n
.endif
    jmp isr_common
.endm

.macro push_regs
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro pop_regs
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
.endm

.text
.global isr_stubs
.balign 16
isr_stubs:
.set i, 0
.rept 256
    isr_stub %i
    .set i, i + 1
.endr

isr_common:
    push_regs
    testb $3, 144(%rsp)
    jz 1f
    swapgs
1:  mov %rsp, %rdi
    cld
    call isr_dispatch
    mov %rax, %rsp
    mov %rax, %rdi
    call isr_leave
isr_exit:
    testb $3, 144(%rsp)
    jz 2f
    swapgs
2:  pop_regs
    add $16, %rsp
    iretq

.global syscall_entry
syscall_entry:
    swapgs
    mov %rsp, %gs:8
    mov %gs:0, %rsp
    pushq $0x23
    pushq %gs:8
    push %r11
    pushq $0x2B
    push %rcx
    pushq $0
    pushq $0x100
    push_regs
    mov %rsp, %rdi
    cld
    call syscall_enter
    mov %rsp, %rdi
    call isr_leave
    jmp isr_exit

/* first entry of a fresh task: rdi = frame to restore */
.global frame_enter
frame_enter:
    mov %rdi, %rsp
    jmp isr_exit
