#include "core/pcache.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/smp.h"
#include "core/task.h"

/* one pcn per file that has been mapped. fr[i] = frame of page i (0 = not read yet),
   bit 0 of it is the clock "seen" bit. the cache holds one ref on every frame,
   each pte another, so refcnt == 1 means nobody maps it and it can go */
typedef struct pcn {
    struct pcn* next;
    fs_node_t* n;
    uint64_t* fr;
    uint32_t cap;
    bool shared;           /* somebody maps it MAP_SHARED: frames are the truth until synced */
} pcn_t;

static pcn_t* head;
static pcn_t* hand_n;
static uint32_t hand_i;
static spin_t pcl;
static uint64_t npages, nshared;

uint64_t pc_pages(void) { return npages; }

uint64_t pc_idle_pages(void) {
    uint64_t r = 0;
    uint64_t f = spin_lock(&pcl);
    for (pcn_t* c = head; c; c = c->next)
        for (uint32_t i = 0; i < c->cap; i++)
            if (c->fr[i] && pmm_refcnt(c->fr[i] & ~1ul) == 1) r++;
    spin_unlock(&pcl, f);
    return r;
}

static fs_node_t* real(fs_node_t* n) { return n->hl ? n->hl : n; }

static void fill(fs_node_t* n, uint64_t fr, uint32_t i) {
    uint64_t off = (uint64_t)i * PAGE_SIZE;
    if (!n->data || off >= n->size) return;
    uint64_t k = n->size - off;
    memcpy(P2V(fr), n->data + off, k > PAGE_SIZE ? PAGE_SIZE : k);
}

// pcl held. a ref goes to the caller
static uint64_t get_nl(fs_node_t* n, uint32_t i) {
    pcn_t* c = n->pc;
    uint32_t need = (n->size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (i >= need) return 0;
    if (!c) {
        c = kmalloc(sizeof(*c));
        if (!c) return 0;
        memset(c, 0, sizeof(*c));
        c->n = n;
        c->next = head;
        head = c;
        n->pc = c;
    }
    if (i >= c->cap) {
        uint32_t nc = need > i + 1 ? need : i + 1;
        uint64_t* a = kmalloc((size_t)nc * 8);
        if (!a) return 0;
        memset(a, 0, (size_t)nc * 8);
        if (c->cap) { memcpy(a, c->fr, (size_t)c->cap * 8); kfree(c->fr); }
        c->fr = a;
        c->cap = nc;
    }
    uint64_t fr = c->fr[i] & ~1ul;
    if (!fr) {
        fr = pmm_alloc();
        if (!fr) return 0;
        fill(n, fr, i);
        npages++;
    }
    c->fr[i] = fr | 1;
    pmm_ref(fr);
    return fr;
}

static void drop_nl(pcn_t* c) {
    for (uint32_t i = 0; i < c->cap; i++)
        if (c->fr[i]) { pmm_unref(c->fr[i] & ~1ul); npages--; }
    if (c->shared) nshared--;
    for (pcn_t** pp = &head; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    if (hand_n == c) hand_n = NULL;
    c->n->pc = NULL;
    if (c->fr) kfree(c->fr);
    kfree(c);
}

int pc_map(uint64_t pd, uint64_t va, fs_node_t* n, uint64_t off, uint64_t len, int mode) {
    n = real(n);
    bool w = mode & PCM_W, sh = mode & PCM_SHARED;
    uint64_t np = (len + PAGE_SIZE - 1) / PAGE_SIZE, first = off / PAGE_SIZE;
    uint64_t fsz = (n->size + PAGE_SIZE - 1) / PAGE_SIZE;
    fs_need(n);
    for (uint64_t k = 0; k < np; k++) {
        if (first + k >= fsz) {                  /* past the end: zero page on touch, like before */
            vmm_lazy_range(pd, va + k * PAGE_SIZE, (np - k) * PAGE_SIZE, w, true);
            break;
        }
        uint64_t f = spin_lock(&pcl);
        uint64_t fr = get_nl(n, (uint32_t)(first + k));
        if (fr && sh && !n->pc->shared) { n->pc->shared = true; nshared++; }
        spin_unlock(&pcl, f);
        if (!fr) return -1;
        int r = vmm_map_cache(pd, va + k * PAGE_SIZE, fr, w, sh);
        pmm_unref(fr);
        if (r < 0) return -1;
    }
    return 0;
}

void pc_changed(fs_node_t* n) {
    n = real(n);
    if (!n->pc) return;
    uint64_t f = spin_lock(&pcl);
    pcn_t* c = n->pc;
    if (c && c->shared) {
        for (uint32_t i = 0; i < c->cap; i++) {
            uint64_t fr = c->fr[i] & ~1ul;
            if (!fr) continue;
            memset(P2V(fr), 0, PAGE_SIZE);
            fill(n, fr, i);
        }
    } else if (c) drop_nl(c);
    spin_unlock(&pcl, f);
}

void pc_free(fs_node_t* n) {
    if (!n->pc) return;
    uint64_t f = spin_lock(&pcl);
    if (n->pc) drop_nl(n->pc);
    spin_unlock(&pcl, f);
}

// frames -> data, for pcn c. pcl held, returns true when something changed
static bool sync_nl(pcn_t* c, uint64_t first, uint64_t end) {
    fs_node_t* n = c->n;
    bool ch = false;
    if (!n->data || !n->cap) return false;     // borrowed boot data: nobody writes it
    if (end > c->cap) end = c->cap;
    for (uint64_t i = first; i < end; i++) {
        uint64_t fr = c->fr[i] & ~1ul, off = (uint64_t)i * PAGE_SIZE;
        if (!fr || off >= n->size) continue;
        uint64_t k = n->size - off;
        if (k > PAGE_SIZE) k = PAGE_SIZE;
        if (memcmp(n->data + off, P2V(fr), k)) { memcpy(n->data + off, P2V(fr), k); ch = true; }
    }
    return ch;
}

void pc_sync_range(fs_node_t* n, uint64_t off, uint64_t len) {
    n = real(n);
    if (!n->pc || !n->pc->shared || !len || off >= n->size) return;
    if (len > n->size - off) len = n->size - off;
    uint64_t f = spin_lock(&pcl);
    bool ch = n->pc && sync_nl(n->pc, off / PAGE_SIZE, (off + len + PAGE_SIZE - 1) / PAGE_SIZE);
    spin_unlock(&pcl, f);
    if (ch) { n->mtime = fs_now(); fs_dirty(n); }
}

void pc_sync(fs_node_t* n) { pc_sync_range(n, 0, real(n)->size); }

void pc_sync_all(void) {
    if (!nshared) return;
    uint64_t f = spin_lock(&pcl);
    for (pcn_t* c = head; c; c = c->next) {
        if (!c->shared) continue;
        if (sync_nl(c, 0, c->cap)) { c->n->mtime = fs_now(); fs_dirty(c->n); }
        // don't restart from the head for every dirty file
        bool used = false;
        for (uint32_t i = 0; i < c->cap && !used; i++)
            if (c->fr[i] && pmm_refcnt(c->fr[i] & ~1ul) > 1) used = true;
        if (!used) { c->shared = false; nshared--; }
    }
    spin_unlock(&pcl, f);
}

uint64_t pc_reclaim(uint64_t want) {
    uint64_t got = 0, steps = 0;
    uint64_t f = spin_lock(&pcl);
    if (!hand_n) { hand_n = head; hand_i = 0; }
    while (got < want && hand_n && steps < npages * 2 + 64) {
        pcn_t* c = hand_n;
        steps++;
        if (hand_i >= c->cap) { hand_n = c->next ? c->next : head; hand_i = 0; if (hand_n == c && !c->cap) break; continue; }
        uint64_t e = c->fr[hand_i], fr = e & ~1ul;
        uint32_t i = hand_i++;
        if (!fr) continue;
        if (e & 1) { c->fr[i] = fr; continue; }
        if (pmm_refcnt(fr) != 1) continue;
        if (c->shared) continue;       // frames may hold writes that aren't in data yet
        c->fr[i] = 0;
        pmm_unref(fr);
        npages--;
        got++;
    }
    spin_unlock(&pcl, f);
    return got;
}
