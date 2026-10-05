#include "core/wq.h"
#include "core/task.h"
#include "boot/pit.h"

#define NENT 2048
#define NW   128

struct wq_w {
    task_t* t;
    volatile int fired;
    bool used;
    wq_ent_t* ents;
};

struct wq_ent {
    wq_ent_t *next, *prev, *wnext;
    wq_t* q;
    wq_w_t* w;
    void (*cb)(void*);
    void* arg;
    bool used;
};

static wq_ent_t ents[NENT];
static wq_w_t ws[NW];
static volatile int lk;

static void lock(void) { while (__sync_lock_test_and_set(&lk, 1)) __asm__ volatile ("pause"); }
static void unlock(void) { __sync_lock_release(&lk); }

static void unlink(wq_ent_t* e) {
    if (!e->q) return;
    if (e->prev) e->prev->next = e->next; else e->q->head = e->next;
    if (e->next) e->next->prev = e->prev;
    e->q = NULL;
}

/* entries of tasks that got killed while waiting */
static void gc(void) {
    for (int i = 0; i < NW; i++) {
        wq_w_t* w = &ws[i];
        if (!w->used || (w->t->state != T_FREE && w->t->state != T_DEAD)) continue;
        for (wq_ent_t* e = w->ents; e; e = e->wnext) { unlink(e); e->used = false; }
        w->ents = NULL;
        w->used = false;
    }
}

wq_w_t* wq_waiter(void) {
    uint64_t f = irq_save();
    lock();
    wq_w_t* r = NULL;
    for (int pass = 0; pass < 2 && !r; pass++) {
        for (int i = 0; i < NW; i++)
            if (!ws[i].used) { r = &ws[i]; break; }
        if (!r) gc();
    }
    if (r) { r->used = true; r->t = task_current(); r->fired = 0; r->ents = NULL; }
    unlock();
    irq_restore(f);
    return r;
}

static wq_ent_t* ent_new(wq_t* q) {
    wq_ent_t* e = NULL;
    for (int pass = 0; pass < 2 && !e; pass++) {
        for (int i = 0; i < NENT; i++)
            if (!ents[i].used) { e = &ents[i]; break; }
        if (!e) gc();
    }
    if (!e) return NULL;
    e->used = true;
    e->q = q;
    e->prev = NULL;
    e->next = q->head;
    if (q->head) q->head->prev = e;
    q->head = e;
    return e;
}

wq_ent_t* wq_add(wq_t* q, wq_w_t* w) {
    uint64_t f = irq_save();
    lock();
    wq_ent_t* e = ent_new(q);
    if (e) { e->w = w; e->cb = NULL; e->wnext = w->ents; w->ents = e; }
    unlock();
    irq_restore(f);
    return e;
}

wq_ent_t* wq_add_cb(wq_t* q, void (*cb)(void*), void* arg) {
    uint64_t f = irq_save();
    lock();
    wq_ent_t* e = ent_new(q);
    if (e) { e->w = NULL; e->cb = cb; e->arg = arg; e->wnext = NULL; }
    unlock();
    irq_restore(f);
    return e;
}

/* cb entries only, the ones with a waiter go away with it */
void wq_del(wq_ent_t* e) {
    uint64_t f = irq_save();
    lock();
    if (e->used && !e->w) { unlink(e); e->used = false; }
    unlock();
    irq_restore(f);
}

void wq_waiter_free(wq_w_t* w) {
    uint64_t f = irq_save();
    lock();
    for (wq_ent_t* e = w->ents; e; e = e->wnext) { unlink(e); e->used = false; }
    w->ents = NULL;
    w->used = false;
    unlock();
    irq_restore(f);
}

void wq_wake(wq_t* q) {
    if (!q->head) return;
    uint64_t f = irq_save();
    lock();
    for (wq_ent_t* e = q->head; e; e = e->next) {
        if (e->cb) e->cb(e->arg);
        else { e->w->fired = 1; task_ready(e->w->t); }
    }
    unlock();
    irq_restore(f);
}

/* object goes away: wake everybody and let go of the queue */
void wq_drain(wq_t* q) {
    uint64_t f = irq_save();
    lock();
    for (wq_ent_t* e = q->head; e; e = e->next) {
        if (e->cb) e->cb(e->arg);
        else { e->w->fired = 1; task_ready(e->w->t); }
        e->q = NULL;
    }
    q->head = NULL;
    unlock();
    irq_restore(f);
}

bool wq_waiting(wq_t* q) { return q->head != NULL; }

void wq_sleep(wq_w_t* w, uint32_t ms) {
    uint64_t f = irq_save();
    task_t* t = w->t;
    if (t->id == 0) { w->fired = 0; task_yield(); irq_restore(f); return; }   /* the ui task never blocks */
    if (!ms || ms > 500) ms = 500;
    lock();
    bool go = !w->fired;
    if (go) {
        t->wake_ms = pit_uptime_ms() + ms;
        t->state = T_BLOCKED;
    }
    unlock();
    while (go && t->state == T_BLOCKED) task_yield();
    w->fired = 0;
    irq_restore(f);
}
