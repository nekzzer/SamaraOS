#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/smp.h"

/* First-fit free-list allocator, one list per arena. Each block is preceded
   by a header; blocks are chained in address order.

   Two arenas: the main one right after the kernel (identity-mapped, low
   physical memory - DMA buffers, driver state, everything that asks
   kmalloc), and an optional big one in high RAM reached through the direct
   map (kmalloc_big: file contents, which can run to hundreds of MB when a
   compiler is at work). kfree() finds the arena from the address. */

typedef struct block {
    size_t size;          /* size of payload */
    struct block* next;   /* next block in address order */
    int free;
} __attribute__((aligned(16))) block_t;

typedef struct {
    uint8_t* base;
    size_t   size;
    block_t* head;
    size_t   used;
} arena_t;

static arena_t low, big;
static spin_t hl;

#define ALIGN8(x) (((x) + 15ul) & ~15ul)

static void arena_init(arena_t* a, void* base, size_t size) {
    a->base = (uint8_t*)base;
    a->size = size;
    a->head = (block_t*)a->base;
    a->head->size = size - sizeof(block_t);
    a->head->next = NULL;
    a->head->free = 1;
    a->used = 0;
}

void heap_init(void* base, size_t size)    { arena_init(&low, base, size); }
void heap_add_big(void* base, size_t size) { arena_init(&big, base, size); }

static void* arena_alloc(arena_t* a, size_t n) {
    if (!n || !a->head) return NULL;
    n = ALIGN8(n);
    for (block_t* b = a->head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(block_t) + 16) {
            block_t* split = (block_t*)((uint8_t*)b + sizeof(block_t) + n);
            split->size = b->size - n - sizeof(block_t);
            split->next = b->next;
            split->free = 1;
            b->size = n;
            b->next = split;
        }
        b->free = 0;
        a->used += b->size + sizeof(block_t);
        return (uint8_t*)b + sizeof(block_t);
    }
    return NULL;
}

/* User processes run syscalls on their own tasks, so the allocator can be
   entered from two tasks at once: keep every list walk atomic. */
void* kmalloc(size_t n) {
    uint64_t f = spin_lock(&hl);
    void* p = arena_alloc(&low, n);
    spin_unlock(&hl, f);
    return p;
}

void (*heap_reclaim)(size_t need);

void* kmalloc_big(size_t n) {
    uint64_t f = spin_lock(&hl);
    void* p = arena_alloc(&big, n);
    spin_unlock(&hl, f);
    /* full: let ext2 throw out files it can read again, before eating the kernel heap */
    if (!p && heap_reclaim && big.head) {
        heap_reclaim(n);
        f = spin_lock(&hl);
        p = arena_alloc(&big, n);
        spin_unlock(&hl, f);
    }
    /* the kernel heap only if plenty stays free: apk filled the file arena,
       file data ate the kernel heap and everything after that went bad */
    if (!p && (!big.head || low.size - low.used > n + (24u << 20))) {
        f = spin_lock(&hl); p = arena_alloc(&low, n); spin_unlock(&hl, f);
    }
    return p;
}

static arena_t* arena_of(void* p) {
    uint8_t* q = (uint8_t*)p;
    if (big.head && q >= big.base && q < big.base + big.size) return &big;
    return &low;
}

void kfree(void* p) {
    if (!p) return;
    uint64_t f = spin_lock(&hl);
    arena_t* a = arena_of(p);
    block_t* b = (block_t*)((uint8_t*)p - sizeof(block_t));
    b->free = 1;
    a->used -= b->size + sizeof(block_t);
    /* coalesce forward */
    block_t* it = a->head;
    while (it) {
        if (it->free && it->next && it->next->free) {
            it->size += sizeof(block_t) + it->next->size;
            it->next = it->next->next;
            continue;
        }
        it = it->next;
    }
    spin_unlock(&hl, f);
}

/* grow a block in place if the one after it is free. a file written in
   chunks (apk unpacking libLLVM, 161 MB) used to need old + new copies at
   once, and that never fit in the file arena */
bool kgrow(void* p, size_t n) {
    if (!p) return false;
    uint64_t f = spin_lock(&hl);
    arena_t* a = arena_of(p);
    block_t* b = (block_t*)((uint8_t*)p - sizeof(block_t));
    n = ALIGN8(n);
    bool ok = b->size >= n;
    block_t* nx = b->next;
    if (!ok && nx && nx->free && b->size + sizeof(block_t) + nx->size >= n) {
        a->used -= b->size + sizeof(block_t);
        b->size += sizeof(block_t) + nx->size;
        b->next = nx->next;
        if (b->size >= n + sizeof(block_t) + 16) {
            block_t* split = (block_t*)((uint8_t*)b + sizeof(block_t) + n);
            split->size = b->size - n - sizeof(block_t);
            split->next = b->next;
            split->free = 1;
            b->size = n;
            b->next = split;
        }
        a->used += b->size + sizeof(block_t);
        ok = true;
    }
    spin_unlock(&hl, f);
    return ok;
}

size_t heap_used(void)      { return low.used; }
size_t heap_total(void)     { return low.size; }
size_t heap_big_used(void)  { return big.used; }
size_t heap_big_total(void) { return big.size; }
