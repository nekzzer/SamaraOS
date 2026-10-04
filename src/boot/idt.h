#ifndef SAMARA_IDT_H
#define SAMARA_IDT_H
#include "core/types.h"
#include "core/task.h"

void idt_init(void);
/* plain handler: runs with the frame, returns to the interrupted code */
void idt_set_handler(int vec, void (*fn)(regs_t*));
/* handler that may switch tasks: returns the frame to resume */
void idt_set_sched(int vec, regs_t* (*fn)(regs_t*));
/* dpl 3 lets ring 3 use `int n` */
void idt_set_dpl(int vec, int dpl);

#endif
