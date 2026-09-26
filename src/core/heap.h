#ifndef SAMARA_HEAP_H
#define SAMARA_HEAP_H
#include "core/types.h"

void  heap_init(void* base, size_t size);
void  heap_add_big(void* base, size_t size);   /* high-RAM arena (direct map) */
void* kmalloc(size_t n);
void* kmalloc_big(size_t n);                   /* file data: big arena, else main */
void  kfree(void* p);
size_t heap_used(void);
size_t heap_total(void);
size_t heap_big_used(void);
size_t heap_big_total(void);

#endif
