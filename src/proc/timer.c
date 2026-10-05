/* posix timers, setitimer. millisecond resolution, checked from the 1 kHz tick */

#include "proc/proc.h"
#include "core/string.h"
#include "core/clock.h"
#include "boot/pit.h"
#include "core/vmm.h"

#define EINVAL 22
#define EFAULT 14
#define EAGAIN 11

static bool uok(uint64_t a, uint64_t n) { return a >= USER_BASE && a + n <= USER_TOP && a + n >= a; }

/* queue a signal without ever killing from here (we run in the tick):
   a default action gets executed by proc_deliver_signal on the way to ring 3 */
static void raise_sig(proc_t* p, int sig, bool exact) {
    if (!p || p->state != P_ALIVE || sig <= 0 || sig >= NSIG_MAX) return;
    if (p->sh->sa[sig].handler == 1 && !p->tracer) return;
    if (!exact && (p->sig_mask & SIGBIT(sig))) {
        for (int i = 0; i < proc_count(); i++) {
            proc_t* q = proc_at(i);
            if (q && q->state == P_ALIVE && q->sh == p->sh && !q->zleader && !(q->sig_mask & SIGBIT(sig))) { p = q; break; }
        }
    }
    p->sig_pending |= SIGBIT(sig);
    ready_task_of(p);
}

static uint32_t ts_ms(const int64_t* ts) {
    if (!ts[0] && !ts[1]) return 0;
    uint64_t ms = (uint64_t)ts[0] * 1000 + (ts[1] + 999999) / 1000000;
    return ms ? (ms > 0x7fffffff ? 0x7fffffff : (uint32_t)ms) : 1;
}

static void ms_ts(uint32_t ms, int64_t* ts) { ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000ll; }

static void fire(proc_t* l, struct ptimer* t, int id) {
    if (t->notify == 1) return;
    proc_t* tg = l;
    if (t->notify == 4) {
        tg = proc_by_pid(t->tid);
        if (!tg || tg->sh != l->sh) return;
    }
    tg->sq_sig = t->signo; tg->sq_tid = id; tg->sq_val = t->val; tg->sq_over = t->over;
    raise_sig(tg, t->signo, t->notify == 4);
}

void proc_timers_tick(uint32_t now) {
    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (!p || p->state != P_ALIVE) continue;
        if (p->alarm_at && (int32_t)(now - p->alarm_at) >= 0) proc_check_alarm(p, true);
        if (p->is_thread) continue;
        for (int k = 0; k < 16; k++) {
            struct ptimer* t = &p->sh->tm[k];
            if (!t->used || !t->armed || (int32_t)(now - t->at) < 0) continue;
            t->over = 0;
            if (t->iv) {
                t->over = (now - t->at) / t->iv;
                t->at += t->iv * (t->over + 1);
            } else t->armed = false;
            fire(p, t, k);
        }
    }
}

/* ms until the first alarm / posix timer fires, for the tickless idle */
uint32_t proc_next_timer(uint32_t now) {
    int32_t best = 0x7fffffff;
    for (int i = 0; i < proc_count(); i++) {
        proc_t* p = proc_at(i);
        if (!p || p->state != P_ALIVE) continue;
        if (p->alarm_at && (int32_t)(p->alarm_at - now) < best) best = (int32_t)(p->alarm_at - now);
        if (p->is_thread) continue;
        for (int k = 0; k < 16; k++) {
            struct ptimer* t = &p->sh->tm[k];
            if (t->used && t->armed && (int32_t)(t->at - now) < best) best = (int32_t)(t->at - now);
        }
    }
    return best < 0 ? 0 : (uint32_t)best;
}

/* cpu time itimers, called every tick for the running process */
void proc_cpu_timers(proc_t* p, bool user) {
    if (user && p->itv_at && --p->itv_at == 0) {
        p->itv_at = p->itv_iv;
        raise_sig(p, 26, true);
    }
    if (p->itp_at && --p->itp_at == 0) {
        p->itp_at = p->itp_iv;
        raise_sig(p, 27, true);
    }
}

static int itimer(int which, const int64_t* nv, int64_t* ov) {
    proc_t* p = proc_current();
    uint32_t now = pit_uptime_ms();
    uint32_t *at, *iv;
    if (which == 0) { at = &p->alarm_at; iv = &p->alarm_interval; }
    else if (which == 1) { at = &p->itv_at; iv = &p->itv_iv; }
    else if (which == 2) { at = &p->itp_at; iv = &p->itp_iv; }
    else return -EINVAL;
    if (ov) {
        uint32_t left = *at ? (which ? *at : *at - now) : 0;
        if (which == 0 && *at && (int32_t)left <= 0) left = 1;
        ms_ts(*iv, ov);
        ms_ts(left, ov + 2);
        ov[1] /= 1000; ov[3] /= 1000;                       /* timeval: usec */
    }
    if (nv) {
        int64_t a[2] = { nv[0], nv[1] * 1000 }, b[2] = { nv[2], nv[3] * 1000 };
        uint32_t i = ts_ms(a), v = ts_ms(b);
        *at = v ? (which ? v : now + v) : 0;
        *iv = v ? i : 0;
    }
    return 0;
}

int64_t sys_timer(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    proc_t* p = proc_current();
    proc_t* l = p->is_thread ? proc_by_pid(p->tgid) : p;
    if (!l) l = p;
    pshared_t* sh = p->sh;
    switch (nr) {
        case 222: {                                          /* timer_create(clk, sevp, timerid*) */
            if (!uok(c, 4)) return -EFAULT;
            if ((int)a > 7 || (int)a == 2 || (int)a == 3) return -EINVAL;
            int id = 0;
            while (id < 16 && sh->tm[id].used) id++;
            if (id == 16) return -EAGAIN;
            struct ptimer* t = &sh->tm[id];
            memset(t, 0, sizeof(*t));
            t->clk = (int)a;
            t->signo = 14; t->notify = 0; t->val = (uint64_t)id;
            if (b) {
                if (!uok(b, 64)) return -EFAULT;
                const uint8_t* s = (const uint8_t*)b;
                memcpy(&t->val, s, 8);
                t->signo = *(const int*)(s + 8);
                t->notify = *(const int*)(s + 12);
                t->tid = *(const int*)(s + 16);
                if (t->notify == 2) { t->notify = 4; t->tid = p->pid; t->signo = 32; }
                if (t->notify != 1 && (t->signo <= 0 || t->signo >= NSIG_MAX)) return -EINVAL;
                if (t->notify == 4 && !t->tid) t->tid = p->pid;
            }
            t->used = true;
            *(int*)c = id;
            return 0;
        }
        case 223: {                                          /* timer_settime(id, flags, new, old) */
            if (a >= 16 || !sh->tm[a].used) return -EINVAL;
            struct ptimer* t = &sh->tm[a];
            uint32_t now = pit_uptime_ms();
            if (!uok(c, 32) || (d && !uok(d, 32))) return -EFAULT;
            if (d) {
                int64_t* o = (int64_t*)d;
                ms_ts(t->iv, o);
                ms_ts(t->armed ? (t->at - now > 0x7fffffff ? 1 : t->at - now) : 0, o + 2);
            }
            const int64_t* nv = (const int64_t*)c;
            uint32_t v = ts_ms(nv + 2), i = ts_ms(nv);
            if (v && (b & 1)) {                              /* absolute */
                uint64_t want = (uint64_t)nv[2] * 1000 + nv[3] / 1000000;
                uint64_t cur;
                if (t->clk == 0 || t->clk == 5 || t->clk == 8) { uint32_t s, ns; clock_now(&s, &ns); cur = (uint64_t)s * 1000 + ns / 1000000; }
                else cur = now;
                v = want > cur ? (uint32_t)(want - cur) : 1;
            }
            t->armed = v != 0;
            t->at = now + v;
            t->iv = v ? i : 0;
            t->over = 0;
            return 0;
        }
        case 224: {
            if (a >= 16 || !sh->tm[a].used) return -EINVAL;
            if (!uok(b, 32)) return -EFAULT;
            struct ptimer* t = &sh->tm[a];
            int64_t* o = (int64_t*)b;
            uint32_t left = t->armed ? t->at - pit_uptime_ms() : 0;
            if (left > 0x7fffffff) left = 1;
            ms_ts(t->iv, o);
            ms_ts(left, o + 2);
            return 0;
        }
        case 225:
            if (a >= 16 || !sh->tm[a].used) return -EINVAL;
            return sh->tm[a].over;
        case 226:
            if (a >= 16 || !sh->tm[a].used) return -EINVAL;
            sh->tm[a].used = sh->tm[a].armed = false;
            return 0;
        case 36:                                             /* getitimer(which, cur) */
            if (!uok(b, 32)) return -EFAULT;
            return itimer((int)a, NULL, (int64_t*)b);
        case 38:                                             /* setitimer(which, new, old) */
            if ((b && !uok(b, 32)) || (c && !uok(c, 32))) return -EFAULT;
            return itimer((int)a, (const int64_t*)b, (int64_t*)c);
    }
    return -EINVAL;
}
