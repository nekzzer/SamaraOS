#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "core/smp.h"
#include "core/io.h"

extern int snprintf(char* buf, size_t n, const char* fmt, ...);
extern const char* ksym_lookup(uint64_t a, uint64_t* off);

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
    struct block* prev;   /* fits in the old 32 byte header, padding was there anyway */
    int free;
    uint32_t mg;          /* header canary, tells double free from a trashed header */
#ifdef KDEBUG
    size_t req;           /* what the caller asked for, the rest up to size is redzone */
    void* ra;             /* who allocated it */
#endif
} __attribute__((aligned(16))) block_t;

#define MG_USED 0x5a11c0de
#define MG_FREE 0xf4eeb10c
#ifdef KDEBUG
#define RZ 16
#define RZ_B 0xa5
#define PZ_B 0xdd
#define PZ_MAX 1024
static void* cur_ra;
#define SETRA() (cur_ra = __builtin_return_address(0))
#else
#define SETRA()
#endif

typedef struct {
    uint8_t* base;
    size_t   size;
    block_t* head;
    block_t* rover;       /* where the last search stopped */
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
    a->head->prev = NULL;
    a->head->free = 1;
    a->head->mg = MG_FREE;
    a->rover = a->head;
    a->used = 0;
}

static void split_at(block_t* b, size_t n) {
    block_t* split = (block_t*)((uint8_t*)b + sizeof(block_t) + n);
    split->size = b->size - n - sizeof(block_t);
    split->next = b->next;
    split->prev = b;
    split->free = 1;
    split->mg = MG_FREE;
    if (split->next) split->next->prev = split;
    b->size = n;
    b->next = split;
#ifdef KDEBUG
    memset((uint8_t*)split + sizeof(block_t), PZ_B, split->size < PZ_MAX ? split->size : PZ_MAX);
#endif
}

/* b swallows the block after it */
static void merge_next(arena_t* a, block_t* b) {
    block_t* nx = b->next;
    b->size += sizeof(block_t) + nx->size;
    b->next = nx->next;
    if (b->next) b->next->prev = b;
    if (a->rover == nx) a->rover = b;
#ifdef KDEBUG
    if (b->free) memset((uint8_t*)b + sizeof(block_t), PZ_B, b->size < PZ_MAX ? b->size : PZ_MAX);
#endif
}

void heap_init(void* base, size_t size)    { arena_init(&low, base, size); }
void heap_add_big(void* base, size_t size) { arena_init(&big, base, size); }

/* next fit: first fit from the head walked over every live block each time,
   with ~50k fs nodes that was most of the kernel's time (glxgears on virgl) */
static void* arena_alloc(arena_t* a, size_t n) {
    if (!n || !a->head) return NULL;
#ifdef KDEBUG
    size_t req = n;
    n += RZ;
#endif
    n = ALIGN8(n);
    block_t* start = a->rover ? a->rover : a->head;
    block_t* b = start;
    do {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(block_t) + 16) split_at(b, n);
            b->free = 0;
            b->mg = MG_USED;
            a->used += b->size + sizeof(block_t);
            a->rover = b->next ? b->next : a->head;
#ifdef KDEBUG
            b->req = req;
            b->ra = cur_ra;
            memset((uint8_t*)b + sizeof(block_t) + req, RZ_B, b->size - req);
#endif
            return (uint8_t*)b + sizeof(block_t);
        }
        b = b->next ? b->next : a->head;
    } while (b != start);
    return NULL;
}

/* User processes run syscalls on their own tasks, so the allocator can be
   entered from two tasks at once: keep every list walk atomic. */
void* kmalloc(size_t n) {
    uint64_t f = spin_lock(&hl);
    SETRA();
    void* p = arena_alloc(&low, n);
    spin_unlock(&hl, f);
    return p;
}

void (*heap_reclaim)(size_t need);

void* kmalloc_big(size_t n) {
    uint64_t f = spin_lock(&hl);
    SETRA();
    void* p = arena_alloc(&big, n);
    spin_unlock(&hl, f);
    /* full: let ext2 throw out files it can read again, before eating the kernel heap */
    if (!p && heap_reclaim && big.head) {
        heap_reclaim(n);
        f = spin_lock(&hl);
        SETRA();
        p = arena_alloc(&big, n);
        spin_unlock(&hl, f);
    }
    /* the kernel heap only if plenty stays free: apk filled the file arena,
       file data ate the kernel heap and everything after that went bad */
    if (!p && (!big.head || low.size - low.used > n + (24u << 20))) {
        f = spin_lock(&hl); SETRA(); p = arena_alloc(&low, n); spin_unlock(&hl, f);
    }
    return p;
}

static arena_t* arena_of(void* p) {
    uint8_t* q = (uint8_t*)p;
    if (big.head && q >= big.base && q < big.base + big.size) return &big;
    return &low;
}

static char rep[4096];
static int rl, nerr, nchk, quiet;

static void say(const char* fmt, ...);
static void say_blk(const char* what, block_t* b);

#ifdef KDEBUG
static int rz_ok(block_t* b) {
    uint8_t* q = (uint8_t*)b + sizeof(block_t);
    for (size_t i = b->req; i < b->size; i++)
        if (q[i] != RZ_B) return 0;
    return 1;
}
static int pz_ok(block_t* b) {
    uint8_t* q = (uint8_t*)b + sizeof(block_t);
    size_t n = b->size < PZ_MAX ? b->size : PZ_MAX;
    for (size_t i = 0; i < n; i++)
        if (q[i] != PZ_B) return 0;
    return 1;
}
static int nfree;
#endif
static void heap_check_locked(void);

static void free_locked(void* p, void* ra) {
    arena_t* a = arena_of(p);
    block_t* b = (block_t*)((uint8_t*)p - sizeof(block_t));
    if (b->mg != MG_USED) {
        say_blk(b->mg == MG_FREE ? "double free" : "bad free", b);
        say("[heap]   freed from %lx\n", (unsigned long)ra);
        return;
    }
#ifdef KDEBUG
    if (!rz_ok(b)) say_blk("redzone smashed", b);
#endif
    b->free = 1;
    b->mg = MG_FREE;
    a->used -= b->size + sizeof(block_t);
    // only the neighbours, never two free blocks next to each other anyway.
    // used to walk the whole heap on every free
#ifdef KDEBUG
    memset(p, PZ_B, b->size < PZ_MAX ? b->size : PZ_MAX);
#endif
    if (b->next && b->next->free) merge_next(a, b);
    if (b->prev && b->prev->free) merge_next(a, b->prev);
#ifdef KDEBUG
    if (++nfree % 8192 == 0 && nerr < 20) heap_check_locked();
#endif
}

void kfree(void* p) {
    if (!p) return;
    uint64_t f = spin_lock(&hl);
    free_locked(p, __builtin_return_address(0));
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
#ifdef KDEBUG
    size_t rn = n;
    n = ALIGN8(n + RZ);
#endif
    bool ok = b->size >= n;
    block_t* nx = b->next;
    if (!ok && nx && nx->free && b->size + sizeof(block_t) + nx->size >= n) {
        a->used -= b->size + sizeof(block_t);
        merge_next(a, b);
        if (b->size >= n + sizeof(block_t) + 16) split_at(b, n);
        a->used += b->size + sizeof(block_t);
        ok = true;
    }
#ifdef KDEBUG
    if (ok && rn > b->req) {
        b->req = rn;
        memset((uint8_t*)b + sizeof(block_t) + rn, RZ_B, b->size - rn);
    }
#endif
    spin_unlock(&hl, f);
    return ok;
}

size_t heap_used(void)      { return low.used; }
size_t heap_total(void)     { return low.size; }
size_t heap_big_used(void)  { return big.used; }
size_t heap_big_total(void) { return big.size; }

static void say(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    extern int vsnprintf(char* buf, size_t n, const char* fmt, __builtin_va_list ap);
    char t[160];
    int n = vsnprintf(t, sizeof(t), fmt, ap);
    __builtin_va_end(ap);
    if (n > 150) n = 150;
    if (rl + n < (int)sizeof(rep)) { memcpy(rep + rl, t, n); rl += n; rep[rl] = 0; }
    t[n] = 0;
    if (quiet) return;
    for (char* c = t; *c; c++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *c); }
}

static void say_blk(const char* what, block_t* b) {
    nerr++;
    say("[heap] %s blk %lx size %lu", what, (unsigned long)b, (unsigned long)b->size);
#ifdef KDEBUG
    uint64_t off;
    const char* nm = b->mg == MG_USED ? ksym_lookup((uint64_t)b->ra, &off) : NULL;
    if (b->mg == MG_USED) say(" req %lu alloc %lx %s", (unsigned long)b->req, (unsigned long)b->ra, nm ? nm : "?");
#endif
    say("\n");
}

/* walks every block of both arenas, caller holds hl */
static void heap_check_locked(void) {
    arena_t* as[2] = { &low, &big };
    nchk++;
    for (int k = 0; k < 2; k++) {
        arena_t* a = as[k];
        if (!a->head) continue;
        size_t used = 0, nb = 0;
        block_t* pv = NULL;
        for (block_t* b = a->head; b; pv = b, b = b->next) {
            nb++;
            if ((uint8_t*)b < a->base || (uint8_t*)b >= a->base + a->size || b->prev != pv) { say_blk("bad link", b); break; }
            if (b->mg != (b->free ? MG_FREE : MG_USED)) { say_blk("bad magic", b); break; }
            if (b->next && (uint8_t*)b + sizeof(block_t) + b->size != (uint8_t*)b->next) { say_blk("bad size", b); break; }
            if (pv && pv->free && b->free) say_blk("unmerged", b);
            if (!b->free) {
                used += b->size + sizeof(block_t);
#ifdef KDEBUG
                if (!rz_ok(b)) say_blk("redzone smashed", b);
#endif
            }
#ifdef KDEBUG
            else if (!pz_ok(b)) say_blk("use after free", b);
#endif
            if (nerr > 20) break;
        }
        if (used != a->used) say("[heap] arena %d used %lu, counted %lu\n", k, (unsigned long)a->used, (unsigned long)used);
    }
}

/* plant some bugs and see if they get caught, then forget the reports. all under the lock, nobody can grab the blocks */
static void heap_selftest(void) {
    int hit = 0, want = 1, l0 = rl, e0 = nerr, n0 = nerr;
    char c0 = rep[rl];
    quiet = 1;
    uint8_t* p = arena_alloc(&low, 40);
    free_locked(p, 0);
    free_locked(p, 0);                // double free
    if (nerr > e0) hit++;
#ifdef KDEBUG
    want = 3;
    e0 = nerr;
    p = arena_alloc(&low, 40);
    p[40] = 1;                        // one byte past the end
    free_locked(p, 0);
    if (nerr > e0) hit++;
    e0 = nerr;
    uint8_t* g1 = arena_alloc(&low, 16);      // keeps p from merging backwards, next fit puts p right after g1
    p = arena_alloc(&low, 200);
    uint8_t* g2 = arena_alloc(&low, 16);
    free_locked(p, 0);
    p[10] = 0x42;                     // use after free
    heap_check_locked();
    if (nerr > e0) hit++;
    p[10] = PZ_B;
    free_locked(g1, 0);
    free_locked(g2, 0);
#endif
    nerr = n0;
    rl = l0; rep[rl] = c0;
    quiet = 0;
    say("heap: selftest %d/%d caught\n", hit, want);
}

void heap_cmd(const char* s, uint32_t n) {
    if (n >= 8 && !strncmp(s, "selftest", 8)) {
        uint64_t f = spin_lock(&hl);
        heap_selftest();
        spin_unlock(&hl, f);
        return;
    }
    if (n >= 5 && !strncmp(s, "check", 5)) {
        uint64_t f = spin_lock(&hl);
        rl = 0; rep[0] = 0; nerr = 0;
        heap_check_locked();
        if (!nerr) say("heap: check ok\n");
        spin_unlock(&hl, f);
    }
}

int heap_report(char* buf, int cap) {
    int n = snprintf(buf, cap, "low %lu/%lu big %lu/%lu\nkdebug %d checks %d errors %d\n%s",
        (unsigned long)low.used, (unsigned long)low.size, (unsigned long)big.used, (unsigned long)big.size,
#ifdef KDEBUG
        1,
#else
        0,
#endif
        nchk, nerr, rep);
    return n < cap ? n : cap - 1;
}
