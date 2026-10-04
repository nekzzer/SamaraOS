#ifndef _LIBC_SETJMP_H
#define _LIBC_SETJMP_H

/* layout: rbx, rbp, r12-r15, rsp, rip */
typedef struct { unsigned long regs[8]; } jmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
