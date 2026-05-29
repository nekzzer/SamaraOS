/* Replaces doomgeneric/z_zone.c with a thin wrapper over kmalloc/kfree.
   The original zone allocator has subtle bugs in our embedded environment;
   this is simpler and more predictable. We trade some level-reload memory
   reuse for stability. */

#include "z_zone.h"
#include "i_system.h"
#include "doomtype.h"

extern void* kmalloc(unsigned int n);
extern void  kfree(void* p);
extern void* memset(void* d, int v, unsigned int n);

#define ZMAGIC 0x5a4f4e45u   /* 'ZONE' */

typedef struct zhdr_s {
    unsigned int magic;
    void** user;
    int    tag;
    int    size;
    struct zhdr_s* prev;
    struct zhdr_s* next;
} zhdr_t;

static zhdr_t* z_head = 0;

void Z_Init(void) {
    z_head = 0;
}

void* Z_Malloc(int size, int tag, void* user) {
    if (size <= 0) size = 1;
    /* round up to 8 */
    size = (size + 7) & ~7;
    zhdr_t* h = (zhdr_t*)kmalloc(sizeof(zhdr_t) + size);
    if (!h) {
        I_Error("Z_Malloc: failed on %i bytes", size);
        return 0;
    }
    h->magic = ZMAGIC;
    h->user  = (void**)user;
    h->tag   = tag;
    h->size  = size;
    h->prev  = 0;
    h->next  = z_head;
    if (z_head) z_head->prev = h;
    z_head = h;
    void* result = (void*)(h + 1);
    if (h->user) *h->user = result;
    return result;
}

static zhdr_t* hdr_of(void* p) {
    if (!p) return 0;
    zhdr_t* h = ((zhdr_t*)p) - 1;
    if (h->magic != ZMAGIC) {
        I_Error("Z_Free/etc: bad magic on %p", p);
        return 0;
    }
    return h;
}

void Z_Free(void* p) {
    zhdr_t* h = hdr_of(p);
    if (!h) return;
    if (h->user) *h->user = 0;
    if (h->prev) h->prev->next = h->next; else z_head = h->next;
    if (h->next) h->next->prev = h->prev;
    h->magic = 0;
    kfree(h);
}

void Z_FreeTags(int lowtag, int hightag) {
    /* Walk list and free everything in [lowtag, hightag]. Iterate carefully
       since Z_Free unlinks. */
    zhdr_t* h = z_head;
    while (h) {
        zhdr_t* next = h->next;
        if (h->tag >= lowtag && h->tag <= hightag) {
            Z_Free((void*)(h + 1));
        }
        h = next;
    }
}

void Z_DumpHeap(int lowtag, int hightag) { (void)lowtag; (void)hightag; }
void Z_FileDumpHeap(FILE* f) { (void)f; }
void Z_CheckHeap(void) { }

void Z_ChangeTag2(void* p, int tag, char* file, int line) {
    (void)file; (void)line;
    zhdr_t* h = hdr_of(p);
    if (h) h->tag = tag;
}

void Z_ChangeUser(void* p, void** user) {
    zhdr_t* h = hdr_of(p);
    if (h) {
        h->user = user;
        if (user) *user = p;
    }
}

int Z_FreeMemory(void) { return 0; }
unsigned int Z_ZoneSize(void) { return 0; }
