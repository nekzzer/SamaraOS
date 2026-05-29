#ifndef _LIBC_SETJMP_H
#define _LIBC_SETJMP_H

/* layout: ebx, esi, edi, ebp, esp, eip */
typedef struct { unsigned int regs[6]; } jmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
