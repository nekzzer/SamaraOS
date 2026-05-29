#ifndef SAMARA_IDT_H
#define SAMARA_IDT_H
#include "types.h"

struct interrupt_frame {
    uint32_t eip, cs, eflags, esp, ss;
};

void idt_init(void);
void idt_set_gate(int n, void* handler, uint16_t selector, uint8_t flags);

#endif
