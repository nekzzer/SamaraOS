.intel_syntax noprefix
.text

/* int setjmp(jmp_buf env);  env -> regs[6] = {ebx, esi, edi, ebp, esp, eip} */
.global setjmp
.type setjmp, @function
setjmp:
    mov    eax, [esp + 4]      /* env */
    mov    [eax +  0], ebx
    mov    [eax +  4], esi
    mov    [eax +  8], edi
    mov    [eax + 12], ebp
    lea    ecx, [esp + 4]      /* caller's esp (after return) */
    mov    [eax + 16], ecx
    mov    ecx, [esp]          /* return address */
    mov    [eax + 20], ecx
    xor    eax, eax
    ret
.size setjmp, .-setjmp

/* void longjmp(jmp_buf env, int val) */
.global longjmp
.type longjmp, @function
longjmp:
    mov    edx, [esp + 4]      /* env */
    mov    eax, [esp + 8]      /* val */
    test   eax, eax
    jne    1f
    mov    eax, 1
1:
    mov    ebx, [edx +  0]
    mov    esi, [edx +  4]
    mov    edi, [edx +  8]
    mov    ebp, [edx + 12]
    mov    esp, [edx + 16]
    mov    ecx, [edx + 20]
    jmp    ecx
.size longjmp, .-longjmp
