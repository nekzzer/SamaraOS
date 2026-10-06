/* seccomp: prctl bits + classic bpf filters, run from syscall_enter (task.c) */

#include "proc/proc.h"
#include "core/heap.h"
#include "core/string.h"

#define EPERM  1
#define EFAULT 14
#define EINVAL 22
#define ENOMEM 12
#define ENOSYS 38

bool user_ok(const void* p, uint32_t len);

struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct sfilt { int refs, len; struct sfilt* prev; struct sock_filter* insn; };

#define RET_KILL_PROC 0x80000000u
#define RET_KILL      0x00000000u
#define RET_TRAP      0x00030000u
#define RET_ERRNO     0x00050000u
#define RET_USER_NOTIF 0x7fc00000u
#define RET_TRACE     0x7ff00000u
#define RET_LOG       0x7ffc0000u
#define RET_ALLOW     0x7fff0000u

struct sfilt* sc_dup(struct sfilt* f) {
    if (f) __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
    return f;
}

void sc_put(struct sfilt* f) {
    while (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        struct sfilt* pv = f->prev;
        kfree(f->insn);
        kfree(f);
        f = pv;
    }
}

// bpf opcodes
#define LD 0x00
#define LDX 0x01
#define ST 0x02
#define STX 0x03
#define ALU 0x04
#define JMP 0x05
#define RET 0x06
#define MISC 0x07

static bool valid(struct sock_filter* f, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t c = f[i].code;
        switch (c & 7) {
        case LD:
            if ((c & 0xe0) == 0x40 || (c & 0xe0) == 0xa0) return false;      /* ind, msh */
            if ((c & 0xe0) == 0x20 && f[i].k >= 64) return false;           /* ABS */
            if ((c & 0xe0) == 0x60 && f[i].k >= 16) return false;           /* MEM */
            break;
        case LDX:
            if ((c & 0xe0) == 0x60 && f[i].k >= 16) return false;
            break;
        case ST: case STX:
            if (f[i].k >= 16) return false;
            break;
        case ALU:
            if ((c & 0xf0) == 0x30 && !(c & 8) && !f[i].k) return false;     /* DIV by 0 */
            if ((c & 0xf0) == 0x90 && !(c & 8) && !f[i].k) return false;     /* MOD by 0 */
            break;
        case JMP:
            if ((c & 0xf0) == 0) { if (f[i].k >= (uint32_t)(n - i - 1)) return false; }
            else if (i + 1 + f[i].jt >= n || i + 1 + f[i].jf >= n) return false;
            break;
        case RET: case MISC: break;
        }
    }
    return (f[n - 1].code & 7) == RET;
}

static uint32_t run(struct sfilt* sf, const uint8_t* d) {
    struct sock_filter* f = sf->insn;
    uint32_t A = 0, X = 0, M[16];
    memset(M, 0, sizeof(M));
    for (int pc = 0; pc < sf->len; pc++) {
        struct sock_filter* i = &f[pc];
        uint16_t c = i->code;
        uint32_t k = i->k;
        switch (c & 7) {
        case LD:
            switch (c & 0xe0) {
            case 0x20: {                                    /* ABS */
                int sz = (c & 0x18) == 0 ? 4 : (c & 0x18) == 0x08 ? 2 : 1;
                if (k + sz > 64) return RET_KILL;
                A = 0;
                for (int b = 0; b < sz; b++) A |= (uint32_t)d[k + b] << (8 * b);   /* seccomp_data is host order */
                break;
            }
            case 0x00: A = k; break;
            case 0x60: A = M[k & 15]; break;
            case 0x80: A = 64; break;
            default: return RET_KILL;
            }
            break;
        case LDX:
            switch (c & 0xe0) {
            case 0x00: X = k; break;
            case 0x60: X = M[k & 15]; break;
            case 0x80: X = 64; break;
            default: return RET_KILL;
            }
            break;
        case ST: M[k & 15] = A; break;
        case STX: M[k & 15] = X; break;
        case ALU: {
            uint32_t s = (c & 8) ? X : k;
            switch (c & 0xf0) {
            case 0x00: A += s; break;
            case 0x10: A -= s; break;
            case 0x20: A *= s; break;
            case 0x30: if (!s) return RET_KILL; A /= s; break;
            case 0x40: A |= s; break;
            case 0x50: A &= s; break;
            case 0x60: A <<= (s & 31); break;
            case 0x70: A >>= (s & 31); break;
            case 0x80: A = -A; break;
            case 0x90: if (!s) return RET_KILL; A %= s; break;
            case 0xa0: A ^= s; break;
            default: return RET_KILL;
            }
            break;
        }
        case JMP: {
            uint32_t s = (c & 8) ? X : k;
            switch (c & 0xf0) {
            case 0x00: pc += k; break;
            case 0x10: pc += A == s ? i->jt : i->jf; break;
            case 0x20: pc += A > s ? i->jt : i->jf; break;
            case 0x30: pc += A >= s ? i->jt : i->jf; break;
            case 0x40: pc += (A & s) ? i->jt : i->jf; break;
            default: return RET_KILL;
            }
            break;
        }
        case RET: return (c & 0x18) == 0x10 ? A : k;
        case MISC:
            if ((c & 0xf8) == 0) X = A; else A = X;
            break;
        }
    }
    return RET_KILL;
}

/* 0 = let it run, 1 = answered (rax set), 2 = SIGSYS, 3 = dies */
int sc_check(regs_t* r) {
    proc_t* p = proc_current();
    uint64_t nr = r->rax;
    if (p->sc_mode == 1) return (nr == 0 || nr == 1 || nr == 60 || nr == 231 || nr == 15) ? 0 : 3;
    uint8_t d[64];
    int32_t n = (int32_t)nr;
    uint32_t arch = 0xC000003E;
    uint64_t ip = r->rip, a[6] = { r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9 };
    memcpy(d, &n, 4); memcpy(d + 4, &arch, 4); memcpy(d + 8, &ip, 8); memcpy(d + 16, a, 48);
    uint32_t best = RET_ALLOW;
    for (struct sfilt* f = p->sf; f; f = f->prev) {
        uint32_t v = run(f, d);
        if ((int32_t)(v & 0xffff0000) < (int32_t)(best & 0xffff0000)) best = v;
    }
    uint32_t act = best & 0xffff0000, data = best & 0xffff;
    if (act == RET_ALLOW || act == RET_LOG) return 0;
    if (act == RET_ERRNO) { r->rax = (uint64_t)-(int64_t)(data > 4095 ? 4095 : data); return 1; }
    if (act == RET_TRAP) { p->sys_nr = (int)nr; p->sys_ip = ip; p->sys_err = (int)data; return 2; }
    if (act == RET_KILL || act == RET_KILL_PROC) return 3;
    r->rax = (uint64_t)-ENOSYS;                              /* trace, user notif: nobody is listening */
    return 1;
}

/* caller holds the bkl */
void sc_trap(regs_t* r) {
    proc_t* p = proc_current();
    r->rax = (uint64_t)-ENOSYS;
    p->sig_mask &= ~SIGBIT(31);
    if (p->sh->sa[31].handler <= 1) proc_exit(31);
    p->sys_sig = true;
    p->sig_pending |= SIGBIT(31);
    proc_deliver_signal(r, -1, 0);
}

static int load(uint64_t uprog, struct sfilt** out) {
    if (!user_ok((void*)uprog, 16)) return -EFAULT;
    uint16_t len = *(uint16_t*)uprog;
    uint64_t up = *(uint64_t*)(uprog + 8);
    if (!len || len > 4096) return -EINVAL;
    if (!user_ok((void*)up, len * 8u)) return -EFAULT;
    struct sock_filter* f = (struct sock_filter*)kmalloc(len * 8u);
    struct sfilt* sf = (struct sfilt*)kmalloc(sizeof(*sf));
    if (!f || !sf) { kfree(f); kfree(sf); return -ENOMEM; }
    memcpy(f, (void*)up, len * 8u);
    if (!valid(f, len)) { kfree(f); kfree(sf); return -EINVAL; }
    sf->refs = 1; sf->len = len; sf->insn = f; sf->prev = NULL;
    *out = sf;
    return 0;
}

int sc_seccomp(uint64_t op, uint64_t fl, uint64_t uargs) {
    proc_t* p = proc_current();
    if (op == 0) {
        if (fl) return -EINVAL;
        p->sc_mode = 1;
        return 0;
    }
    if (op == 2) {                                          /* get_action_avail */
        if (!user_ok((void*)uargs, 4)) return -EFAULT;
        uint32_t a = *(uint32_t*)uargs;
        return (a == RET_KILL_PROC || a == RET_KILL || a == RET_TRAP || a == RET_ERRNO || a == RET_TRACE || a == RET_LOG || a == RET_ALLOW) ? 0 : -EINVAL;
    }
    if (op != 1) return -EINVAL;
    if (fl & ~0x1full) return -EINVAL;
    if (fl & 8) return -EINVAL;                             /* NEW_LISTENER */
    if (p->sc_mode == 1) return -EINVAL;
    struct sfilt* nf;
    int e = load(uargs, &nf);
    if (e < 0) return e;
    nf->prev = p->sf;                                       /* ours moves under the new one */
    p->sf = nf;
    p->sc_mode = 2;
    if (fl & 1) {                                           /* TSYNC: every other thread runs the same chain */
        for (int i = 0; i < proc_count(); i++) {
            proc_t* q = proc_at(i);
            if (!q || q == p || q->state != P_ALIVE || q->tgid != p->tgid) continue;
            struct sfilt* old = q->sf;
            q->sf = sc_dup(nf);
            sc_put(old);
            q->sc_mode = 2;
            q->nnp = true;
        }
    }
    return 0;
}

int sc_prctl(uint64_t op, uint64_t a, uint64_t b) {
    proc_t* p = proc_current();
    switch (op) {
    case 38: p->nnp = true; return 0;                       /* PR_SET_NO_NEW_PRIVS */
    case 39: return p->nnp;
    case 21: return p->sc_mode;                             /* PR_GET_SECCOMP */
    case 22:
        if (a == 1) return sc_seccomp(0, 0, 0);
        if (a == 2) return sc_seccomp(1, 0, b);
        return -EINVAL;
    }
    return 0;
}
