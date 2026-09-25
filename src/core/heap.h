#ifndef SAMARA_HEAP_H
#define SAMARA_HEAP_H
#include "core/types.h"

void  heap_init(void* base, size_t size);
void* kmalloc(size_t n);
void  kfree(void* p);
size_t heap_used(void);
size_t heap_total(void);

#endif
