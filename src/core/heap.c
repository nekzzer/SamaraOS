#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"

/* Simple first-fit free-list allocator. Each block is preceded by a header. */

typedef struct block {
    size_t size;          /* size of payload */
    struct block* next;   /* next free block (when free); ignored when used */
    int free;
} block_t;

static uint8_t* heap_base;
static size_t   heap_size_b;
static block_t* head;
static size_t   used_b = 0;

#define ALIGN8(x) (((x) + 7u) & ~7u)

void heap_init(void* base, size_t size) {
    heap_base = (uint8_t*)base;
    heap_size_b = size;
    head = (block_t*)heap_base;
    head->size = size - sizeof(block_t);
    head->next = NULL;
    head->free = 1;
    used_b = 0;
}

static void* kmalloc_locked(size_t n);

/* User processes run syscalls on their own tasks, so the allocator can be
   entered from two tasks at once: keep every list walk atomic. */
void* kmalloc(size_t n) {
    uint32_t f = irq_save();
    void* p = kmalloc_locked(n);
    irq_restore(f);
    return p;
}

static void* kmalloc_locked(size_t n) {
    if (!n) return NULL;
    n = ALIGN8(n);
    block_t* prev = NULL;
    block_t* b = head;
    while (b) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(block_t) + 16) {
                block_t* split = (block_t*)((uint8_t*)b + sizeof(block_t) + n);
                split->size = b->size - n - sizeof(block_t);
                split->next = b->next;
                split->free = 1;
                b->size = n;
                b->next = split;
            }
            b->free = 0;
            used_b += b->size + sizeof(block_t);
            (void)prev;
            return (uint8_t*)b + sizeof(block_t);
        }
        prev = b;
        b = b->next;
    }
    return NULL;
}

void kfree(void* p) {
    if (!p) return;
    uint32_t f = irq_save();
    block_t* b = (block_t*)((uint8_t*)p - sizeof(block_t));
    b->free = 1;
    used_b -= b->size + sizeof(block_t);
    /* coalesce forward */
    block_t* it = head;
    while (it) {
        if (it->free && it->next && it->next->free) {
            it->size += sizeof(block_t) + it->next->size;
            it->next = it->next->next;
            continue;
        }
        it = it->next;
    }
    irq_restore(f);
}

size_t heap_used(void)  { return used_b; }
size_t heap_total(void) { return heap_size_b; }
