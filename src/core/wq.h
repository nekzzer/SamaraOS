#ifndef SAMARA_WQ_H
#define SAMARA_WQ_H
#include "core/types.h"

/* Wait queues. A blocked task (or a callback) sits on the queue of the thing
   it waits for, the producer calls wq_wake(). Entries and waiters come from
   static pools, never from a kernel stack: a killed task can't leave a
   dangling pointer in a list. Waiters recheck their condition after waking,
   spurious wakes are fine. */

struct wq_ent;
typedef struct wq { struct wq_ent* head; } wq_t;
typedef struct wq_w wq_w_t;
typedef struct wq_ent wq_ent_t;

wq_w_t*   wq_waiter(void);                          /* for the current task, NULL if the pool is empty */
void      wq_waiter_free(wq_w_t* w);                /* unlinks all its entries too */
wq_ent_t* wq_add(wq_t* q, wq_w_t* w);               /* NULL: pool is empty, poll instead */
wq_ent_t* wq_add_cb(wq_t* q, void (*cb)(void*), void* arg);   /* cb runs in the waker, keep it tiny */
void      wq_del(wq_ent_t* e);
void      wq_wake(wq_t* q);
void      wq_sleep(wq_w_t* w, uint32_t ms);         /* 0 = until woken (rechecks every 500 ms anyway) */
void      wq_drain(wq_t* q);
/* one turn of a wait loop (start with *wp = NULL, declare it with WQ_W): the first call
   only queues the waiter so the caller rechecks, later ones sleep */
void      wq_wait(wq_t* q, wq_w_t** wp, uint32_t ms);
void      wq_wfree(wq_w_t** wp);
#define WQ_W(n) wq_w_t* n __attribute__((cleanup(wq_wfree))) = NULL
bool      wq_waiting(wq_t* q);

#endif
