#include "net/sock.h"
#include "net/net.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"
#include "proc/proc.h"

#define EAGAIN       11
#define ENOMEM       12
#define EINVAL       22
#define EMFILE       24
#define EPIPE        32
#define EADDRINUSE   98
#define ENETUNREACH  101
#define ECONNRESET   104
#define EISCONN      106
#define ENOTCONN     107
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EALREADY     114
#define EINPROGRESS  115
#define EINTR        4
#define EOPNOTSUPP   95
#define EDESTADDRREQ 89

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define MAX_SOCKS  64
#define RX_CAP     (64u * 1024u)
#define TX_CAP     (64u * 1024u)
#define MSS        1400
#define ACCEPT_MAX 16
#define UDP_QMAX   32

enum { S_CLOSED, S_LISTEN, S_SYN_SENT, S_SYN_RCVD, S_ESTABLISHED, S_FIN_WAIT1,
       S_FIN_WAIT2, S_CLOSING, S_TIME_WAIT, S_CLOSE_WAIT, S_LAST_ACK };

typedef struct dgram {
    struct dgram* next;
    uint32_t ip;
    uint16_t port;
    uint16_t len;
    uint8_t  data[];
} dgram_t;

struct sock {
    bool     used;
    int      refs;                 /* open file descriptions (0 = orphaned) */
    int      type;                 /* 1 stream, 2 dgram */
    int      state;
    uint32_t lip, rip;
    uint16_t lport, rport;
    bool     bound, connected;
    int      err;

    /* TCP */
    uint32_t iss, snd_una, snd_nxt, rcv_nxt, tx_seq;
    uint16_t snd_wnd;
    uint8_t* rx; uint32_t rx_head, rx_count;
    uint8_t* tx; uint32_t tx_len;
    bool     fin_rcvd, shut_wr, fin_sent;
    uint32_t rto, last_tx_ms, deadline_ms;
    int      retries;
    sock_t*  parent;
    sock_t*  acceptq[ACCEPT_MAX];
    int      aq_n, backlog;

    /* UDP */
    dgram_t* q_head;
    dgram_t* q_tail;
    int      q_n;
};

static sock_t socks[MAX_SOCKS];
static uint16_t next_eph = 49152;

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t be32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

typedef struct __attribute__((packed)) {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off, flags;
    uint16_t win, sum, urg;
} tcph_t;

typedef struct __attribute__((packed)) {
    uint16_t sport, dport, len, sum;
} udph_t;

/* ---------------- checksums ---------------- */

static uint32_t sum16(uint32_t s, const void* d, int len) {
    const uint8_t* p = (const uint8_t*)d;
    while (len > 1) { s += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) s += (uint32_t)p[0] << 8;
    return s;
}

static uint16_t l4_sum(uint32_t src, uint32_t dst, uint8_t proto, const void* d, int len) {
    uint8_t ph[12];
    uint32_t bs = be32(src), bd = be32(dst);
    memcpy(ph, &bs, 4); memcpy(ph + 4, &bd, 4);
    ph[8] = 0; ph[9] = proto; ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;
    uint32_t s = sum16(sum16(0, ph, 12), d, len);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* ---------------- allocation ---------------- */

static sock_t* alloc_sock(int type) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->type = type;
        s->refs = 1;
        s->rto = 300;
        if (type == 1) {
            s->rx = (uint8_t*)kmalloc(RX_CAP);
            s->tx = (uint8_t*)kmalloc(TX_CAP);
            if (!s->rx || !s->tx) {
                if (s->rx) kfree(s->rx);
                if (s->tx) kfree(s->tx);
                s->used = false;
                return NULL;
            }
        }
        return s;
    }
    return NULL;
}

static void free_sock(sock_t* s) {
    if (s->rx) kfree(s->rx);
    if (s->tx) kfree(s->tx);
    while (s->q_head) { dgram_t* d = s->q_head; s->q_head = d->next; kfree(d); }
    for (int i = 0; i < s->aq_n; i++) { s->acceptq[i]->parent = NULL; s->acceptq[i]->refs = 0; }
    s->used = false;
}

static bool port_in_use(int type, uint16_t port) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used && socks[i].type == type && socks[i].lport == port &&
            (socks[i].bound || socks[i].state == S_LISTEN)) return true;
    return false;
}

static uint16_t ephemeral(int type) {
    for (int n = 0; n < 16384; n++) {
        uint16_t p = next_eph++;
        if (next_eph < 49152) next_eph = 49152;
        if (!port_in_use(type, p)) return p;
    }
    return 0;
}

/* ---------------- TCP output ---------------- */

static uint16_t rx_window(sock_t* s) {
    uint32_t free = RX_CAP - s->rx_count;
    return (uint16_t)(free > 65535 ? 65535 : free);
}

static void tcp_segment(sock_t* s, uint32_t seq, uint8_t flags, const uint8_t* data, uint32_t len) {
    uint8_t buf[20 + MSS];
    tcph_t* h = (tcph_t*)buf;
    h->sport = be16(s->lport);
    h->dport = be16(s->rport);
    h->seq = be32(seq);
    h->ack = be32((flags & TCP_ACK) ? s->rcv_nxt : 0);
    h->off = 5 << 4;
    h->flags = flags;
    h->win = be16(rx_window(s));
    h->sum = 0;
    h->urg = 0;
    if (len) memcpy(buf + 20, data, len);
    h->sum = be16(l4_sum(s->lip, s->rip, 6, buf, 20 + (int)len));
    net_send_ip(s->rip, 6, buf, 20 + (int)len);
}

static void send_rst(uint32_t src, uint32_t dst, const tcph_t* in, int datalen) {
    sock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.lip = dst; tmp.rip = src;
    tmp.lport = be16(in->dport); tmp.rport = be16(in->sport);
    if (in->flags & TCP_ACK) {
        tcp_segment(&tmp, be32(in->ack), TCP_RST, NULL, 0);
    } else {
        tmp.rcv_nxt = be32(in->seq) + (uint32_t)datalen + ((in->flags & TCP_SYN) ? 1 : 0) +
                      ((in->flags & TCP_FIN) ? 1 : 0);
        tcp_segment(&tmp, 0, TCP_RST | TCP_ACK, NULL, 0);
    }
}

/* Push unsent data (and FIN when shut down) within the peer's window. */
static void tcp_output(sock_t* s) {
    if (s->state != S_ESTABLISHED && s->state != S_CLOSE_WAIT &&
        s->state != S_FIN_WAIT1 && s->state != S_LAST_ACK && s->state != S_CLOSING) return;
    uint32_t sent = s->snd_nxt - s->tx_seq;             /* bytes of tx already in flight */
    if (s->fin_sent && sent) sent--;
    uint32_t wnd = s->snd_wnd ? s->snd_wnd : 1;
    while (sent < s->tx_len) {
        uint32_t inflight = s->snd_nxt - s->snd_una;
        if (inflight >= wnd) break;
        uint32_t n = s->tx_len - sent;
        if (n > MSS) n = MSS;
        if (n > wnd - inflight) n = wnd - inflight;
        tcp_segment(s, s->snd_nxt, TCP_ACK | TCP_PSH, s->tx + sent, n);
        s->snd_nxt += n;
        sent += n;
        s->last_tx_ms = pit_uptime_ms();
    }
    if (s->shut_wr && !s->fin_sent && sent == s->tx_len) {
        tcp_segment(s, s->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
        s->snd_nxt++;
        s->fin_sent = true;
        s->last_tx_ms = pit_uptime_ms();
        if (s->state == S_ESTABLISHED) s->state = S_FIN_WAIT1;
        else if (s->state == S_CLOSE_WAIT) s->state = S_LAST_ACK;
    }
}

static void reap_if_done(sock_t* s) {
    if (s->refs <= 0 && !s->parent &&
        (s->state == S_CLOSED || (s->state == S_TIME_WAIT && pit_uptime_ms() >= s->deadline_ms)))
        free_sock(s);
}

/* ---------------- TCP input ---------------- */

static sock_t* find_tcp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport) {
    sock_t* listener = NULL;
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != 1 || s->lport != dport) continue;
        if (s->state == S_LISTEN) {
            if (!s->lip || s->lip == dst) listener = s;
            continue;
        }
        if (s->state != S_CLOSED && s->rport == sport && s->rip == src &&
            (!s->lip || s->lip == dst)) return s;
    }
    return listener;
}

static void tcp_ack_in(sock_t* s, uint32_t ack) {
    if ((int32_t)(ack - s->snd_una) <= 0 || (int32_t)(ack - s->snd_nxt) > 0) return;
    uint32_t n = ack - s->tx_seq;
    if (n > s->tx_len) n = s->tx_len;              /* the rest acknowledges SYN/FIN */
    if (n) {
        memmove(s->tx, s->tx + n, s->tx_len - n);
        s->tx_len -= n;
        s->tx_seq += n;
    }
    s->snd_una = ack;
    s->retries = 0;
    s->rto = 300;
    s->last_tx_ms = pit_uptime_ms();
}

void sock_input_tcp(uint32_t src, uint32_t dst, const uint8_t* seg, int len) {
    if (len < 20) return;
    const tcph_t* h = (const tcph_t*)seg;
    int hl = (h->off >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint16_t sport = be16(h->sport), dport = be16(h->dport);
    uint32_t seq = be32(h->seq), ack = be32(h->ack);
    uint8_t fl = h->flags;
    const uint8_t* data = seg + hl;
    uint32_t dlen = (uint32_t)(len - hl);

    sock_t* s = find_tcp(src, sport, dst, dport);
    if (!s) { if (!(fl & TCP_RST)) send_rst(src, dst, h, (int)dlen); return; }

    if (s->state == S_LISTEN) {
        if (fl & (TCP_RST | TCP_ACK)) { if (!(fl & TCP_RST)) send_rst(src, dst, h, (int)dlen); return; }
        if (!(fl & TCP_SYN)) return;
        int pending = s->aq_n;
        for (int i = 0; i < MAX_SOCKS; i++)
            if (socks[i].used && socks[i].parent == s && socks[i].state == S_SYN_RCVD) pending++;
        if (pending >= s->backlog) return;               /* drop: peer retries */
        sock_t* c = alloc_sock(1);
        if (!c) return;
        c->refs = 0;
        c->parent = s;
        c->lip = dst; c->lport = dport;
        c->rip = src; c->rport = sport;
        c->iss = 0x20000000u + pit_uptime_ms() * 64 + (uint32_t)(c - socks) * 4096;
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->tx_seq = c->iss + 1;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = be16(h->win);
        c->state = S_SYN_RCVD;
        tcp_segment(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0);
        c->last_tx_ms = pit_uptime_ms();
        return;
    }

    if (fl & TCP_RST) {
        if (s->state == S_SYN_SENT) s->err = ECONNREFUSED;
        else if (s->state != S_TIME_WAIT && s->state != S_CLOSED) s->err = ECONNRESET;
        s->state = S_CLOSED;
        if (s->parent) {                                 /* never accepted: forget it */
            s->parent = NULL;
            free_sock(s);
        } else {
            reap_if_done(s);
        }
        return;
    }

    if (s->state == S_SYN_SENT) {
        if ((fl & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = be16(h->win);
            s->state = S_ESTABLISHED;
            s->retries = 0;
            tcp_segment(s, s->snd_nxt, TCP_ACK, NULL, 0);
            tcp_output(s);
        }
        return;
    }

    /* Anything past the handshake. */
    if (fl & TCP_ACK) {
        tcp_ack_in(s, ack);
        s->snd_wnd = be16(h->win);
        if (s->state == S_SYN_RCVD && ack == s->snd_nxt) {
            s->state = S_ESTABLISHED;
            sock_t* p = s->parent;
            if (p && p->used && p->state == S_LISTEN && p->aq_n < ACCEPT_MAX) {
                p->acceptq[p->aq_n++] = s;
            } else {
                s->parent = NULL;
                tcp_segment(s, s->snd_nxt, TCP_RST, NULL, 0);
                free_sock(s);
                return;
            }
        }
        bool fin_acked = s->fin_sent && s->snd_una == s->snd_nxt;
        if (fin_acked) {
            if (s->state == S_FIN_WAIT1) s->state = S_FIN_WAIT2;
            else if (s->state == S_CLOSING) { s->state = S_TIME_WAIT; s->deadline_ms = pit_uptime_ms() + 1000; }
            else if (s->state == S_LAST_ACK) { s->state = S_CLOSED; reap_if_done(s); return; }
        }
    }

    bool need_ack = false;
    if (dlen && (s->state == S_ESTABLISHED || s->state == S_FIN_WAIT1 || s->state == S_FIN_WAIT2)) {
        if (seq == s->rcv_nxt) {
            uint32_t room = RX_CAP - s->rx_count;
            uint32_t take = dlen < room ? dlen : room;
            for (uint32_t i = 0; i < take; i++)
                s->rx[(s->rx_head + s->rx_count + i) % RX_CAP] = data[i];
            s->rx_count += take;
            s->rcv_nxt += take;
        }
        need_ack = true;                                 /* in order or not: (dup)ACK */
    }
    if ((fl & TCP_FIN) && seq + dlen == s->rcv_nxt - 0 && !s->fin_rcvd &&
        (s->state == S_ESTABLISHED || s->state == S_FIN_WAIT1 || s->state == S_FIN_WAIT2)) {
        s->rcv_nxt++;
        s->fin_rcvd = true;
        need_ack = true;
        if (s->state == S_ESTABLISHED) s->state = S_CLOSE_WAIT;
        else if (s->state == S_FIN_WAIT1) s->state = (s->snd_una == s->snd_nxt) ? S_TIME_WAIT : S_CLOSING;
        else if (s->state == S_FIN_WAIT2) s->state = S_TIME_WAIT;
        if (s->state == S_TIME_WAIT) s->deadline_ms = pit_uptime_ms() + 1000;
    } else if ((fl & TCP_FIN) && s->fin_rcvd) {
        need_ack = true;                                 /* retransmitted FIN */
    }
    if (need_ack) tcp_segment(s, s->snd_nxt, TCP_ACK, NULL, 0);
    tcp_output(s);
}

/* Retransmission + timers, from netd and blocking waits. */
static void sock_tick(void) {
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != 1) continue;
        if (s->state == S_TIME_WAIT) { if (now >= s->deadline_ms) { s->state = S_CLOSED; reap_if_done(s); } continue; }
        bool waiting = s->state == S_SYN_SENT || s->state == S_SYN_RCVD || s->snd_una != s->snd_nxt;
        if (!waiting || now - s->last_tx_ms < s->rto) continue;
        if (++s->retries > 8) {
            s->err = s->state == S_SYN_SENT ? ETIMEDOUT : ECONNRESET;
            if (s->state != S_SYN_SENT) tcp_segment(s, s->snd_nxt, TCP_RST, NULL, 0);
            s->state = S_CLOSED;
            if (s->parent) { s->parent = NULL; free_sock(s); } else reap_if_done(s);
            continue;
        }
        s->rto = s->rto * 2 > 3000 ? 3000 : s->rto * 2;
        s->last_tx_ms = now;
        if (s->state == S_SYN_SENT) { tcp_segment(s, s->iss, TCP_SYN, NULL, 0); continue; }
        if (s->state == S_SYN_RCVD) { tcp_segment(s, s->iss, TCP_SYN | TCP_ACK, NULL, 0); continue; }
        /* Go back to the oldest unacknowledged byte. */
        bool fin_pending = s->fin_sent;
        s->snd_nxt = s->snd_una;
        s->fin_sent = false;
        if (fin_pending && s->state == S_FIN_WAIT1) s->state = S_ESTABLISHED;
        else if (fin_pending && s->state == S_LAST_ACK) s->state = S_CLOSE_WAIT;
        else if (fin_pending && s->state == S_CLOSING) s->state = S_ESTABLISHED;
        tcp_output(s);
    }
}

/* ---------------- UDP ---------------- */

void sock_input_udp(uint32_t src, uint32_t dst, const uint8_t* d, int len) {
    if (len < 8) return;
    const udph_t* h = (const udph_t*)d;
    uint16_t sport = be16(h->sport), dport = be16(h->dport);
    int ulen = be16(h->len);
    if (ulen < 8 || ulen > len) return;
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != 2 || s->lport != dport) continue;
        if (s->lip && s->lip != dst && (dst >> 24) != 127) continue;
        if (s->connected && (s->rip != src || s->rport != sport)) continue;
        if (s->q_n >= UDP_QMAX) return;
        dgram_t* g = (dgram_t*)kmalloc(sizeof(dgram_t) + (uint32_t)(ulen - 8));
        if (!g) return;
        g->next = NULL; g->ip = src; g->port = sport; g->len = (uint16_t)(ulen - 8);
        memcpy(g->data, d + 8, (uint32_t)(ulen - 8));
        if (s->q_tail) s->q_tail->next = g; else s->q_head = g;
        s->q_tail = g;
        s->q_n++;
        return;
    }
}

/* ---------------- netd ---------------- */

static void pump(void) {
    uint32_t f = irq_save();
    net_poll();
    sock_tick();
    irq_restore(f);
}

static void netd(void) {
    net_init();                                        /* NIC may be absent: loopback still works */
    for (;;) {
        pump();
        task_sleep_ms(2);
    }
}

void sock_init(void) { task_spawn("netd", netd); }

/* Wait for `cond` with packet processing; 0, -EAGAIN or -EINTR. */
#define WAIT_FOR(cond, nonblock)                                   \
    do {                                                           \
        pump();                                                    \
        while (!(cond)) {                                          \
            if (nonblock) return -EAGAIN;                          \
            if (proc_interrupted()) return -EINTR;                 \
            task_yield();                                          \
            pump();                                                \
        }                                                          \
    } while (0)

/* ---------------- API ---------------- */

sock_t* sock_create(int type, int* err) {
    if (type != 1 && type != 2) { *err = -EINVAL; return NULL; }
    sock_t* s = alloc_sock(type);
    if (!s) *err = -ENOMEM;
    return s;
}

void sock_ref(sock_t* s) { s->refs++; }
int  sock_type(sock_t* s) { return s->type; }

void sock_close(sock_t* s) {
    if (--s->refs > 0) return;
    if (s->type == 2 || s->state == S_CLOSED || s->state == S_SYN_SENT) { free_sock(s); return; }
    if (s->state == S_LISTEN) {
        for (int i = 0; i < s->aq_n; i++) {
            sock_t* c = s->acceptq[i];
            c->parent = NULL;
            tcp_segment(c, c->snd_nxt, TCP_RST, NULL, 0);
            free_sock(c);
        }
        s->aq_n = 0;
        for (int i = 0; i < MAX_SOCKS; i++)
            if (socks[i].used && socks[i].parent == s) { socks[i].parent = NULL; free_sock(&socks[i]); }
        free_sock(s);
        return;
    }
    /* Orphan: finish sending, then FIN; freed by the state machine. */
    s->shut_wr = true;
    tcp_output(s);
    reap_if_done(s);
}

int sock_bind(sock_t* s, uint32_t ip, uint16_t port) {
    if (s->bound) return -EINVAL;
    if (!port) port = ephemeral(s->type);
    else if (port_in_use(s->type, port)) return -EADDRINUSE;
    s->lip = ip;
    s->lport = port;
    s->bound = true;
    return 0;
}

int sock_listen(sock_t* s, int backlog) {
    if (s->type != 1) return -EOPNOTSUPP;
    if (s->state != S_CLOSED && s->state != S_LISTEN) return -EINVAL;
    if (!s->bound) { int r = sock_bind(s, 0, 0); if (r < 0) return r; }
    s->state = S_LISTEN;
    s->backlog = backlog <= 0 ? 1 : backlog > ACCEPT_MAX ? ACCEPT_MAX : backlog;
    return 0;
}

sock_t* sock_accept(sock_t* s, bool nonblock, int* err, uint32_t* ip, uint16_t* port) {
    if (s->state != S_LISTEN) { *err = -EINVAL; return NULL; }
    pump();
    while (!s->aq_n) {
        if (nonblock) { *err = -EAGAIN; return NULL; }
        if (proc_interrupted()) { *err = -EINTR; return NULL; }
        task_yield();
        pump();
    }
    sock_t* c = s->acceptq[0];
    memmove(s->acceptq, s->acceptq + 1, (uint32_t)(--s->aq_n) * sizeof(sock_t*));
    c->parent = NULL;
    c->refs = 1;
    *ip = c->rip;
    *port = c->rport;
    return c;
}

int sock_connect(sock_t* s, uint32_t ip, uint16_t port, bool nonblock) {
    if (s->type == 2) {                                /* UDP: default destination */
        if (!s->bound) sock_bind(s, 0, 0);
        s->rip = ip; s->rport = port; s->connected = true;
        return 0;
    }
    if (s->state == S_ESTABLISHED) return -EISCONN;
    if (s->state == S_SYN_SENT) {
        if (nonblock) return -EALREADY;
    } else {
        if (s->state != S_CLOSED) return -EINVAL;
        if (!s->bound) { int r = sock_bind(s, 0, 0); if (r < 0) return r; }
        s->rip = ip;
        s->rport = port;
        if (!s->lip || (ip >> 24) == 127) s->lip = net_src_for(ip);
        s->iss = 0x10000000u + pit_uptime_ms() * 64 + (uint32_t)(s - socks) * 4096;
        s->snd_una = s->iss;
        s->snd_nxt = s->iss + 1;
        s->tx_seq = s->iss + 1;
        s->state = S_SYN_SENT;
        s->err = 0;
        s->retries = 0;
        s->last_tx_ms = pit_uptime_ms();
        tcp_segment(s, s->iss, TCP_SYN, NULL, 0);
        if (nonblock) return -EINPROGRESS;
    }
    pump();
    while (s->state == S_SYN_SENT) {
        if (proc_interrupted()) return -EINTR;
        task_yield();
        pump();
    }
    if (s->state == S_ESTABLISHED || s->state == S_CLOSE_WAIT) return 0;
    int e = s->err ? s->err : ECONNREFUSED;
    s->err = 0;
    return -e;
}

int sock_send(sock_t* s, const uint8_t* buf, uint32_t len, bool nonblock,
              const uint32_t* to_ip, const uint16_t* to_port) {
    if (s->type == 2) {
        uint32_t ip = to_ip ? *to_ip : s->rip;
        uint16_t port = to_port ? *to_port : s->rport;
        if (!to_ip && !s->connected) return -EDESTADDRREQ;
        if (len > 1472) return -90;                    /* EMSGSIZE */
        if (!s->bound) sock_bind(s, 0, 0);
        uint8_t pkt[8 + 1472];
        udph_t* h = (udph_t*)pkt;
        h->sport = be16(s->lport);
        h->dport = be16(port);
        h->len = be16((uint16_t)(8 + len));
        h->sum = 0;
        memcpy(pkt + 8, buf, len);
        uint32_t src = (s->lip && (ip >> 24) != 127) ? s->lip : net_src_for(ip);
        uint16_t cs = l4_sum(src, ip, 17, pkt, 8 + (int)len);
        h->sum = be16(cs ? cs : 0xFFFF);
        uint32_t f = irq_save();
        net_send_ip(ip, 17, pkt, 8 + (int)len);
        irq_restore(f);
        pump();
        return (int)len;
    }
    if (s->state == S_SYN_SENT || s->state == S_SYN_RCVD) return -ENOTCONN;
    uint32_t done = 0;
    while (done < len) {
        if (s->err) { int e = s->err; s->err = 0; return done ? (int)done : -e; }
        if (s->shut_wr || (s->state != S_ESTABLISHED && s->state != S_CLOSE_WAIT)) {
            if (done) return (int)done;
            proc_t* me = proc_current();
            if (me) proc_send_signal(me, 13);          /* SIGPIPE */
            return -EPIPE;
        }
        uint32_t room = TX_CAP - s->tx_len;
        if (!room) {
            if (nonblock) return done ? (int)done : -EAGAIN;
            if (proc_interrupted()) return done ? (int)done : -EINTR;
            task_yield();
            pump();
            continue;
        }
        uint32_t n = len - done < room ? len - done : room;
        memcpy(s->tx + s->tx_len, buf + done, n);
        s->tx_len += n;
        done += n;
        uint32_t f = irq_save();
        tcp_output(s);
        irq_restore(f);
    }
    pump();
    return (int)done;
}

int sock_recv(sock_t* s, uint8_t* buf, uint32_t len, bool nonblock, bool peek,
              uint32_t* from_ip, uint16_t* from_port) {
    if (s->type == 2) {
        WAIT_FOR(s->q_head, nonblock);
        dgram_t* g = s->q_head;
        uint32_t n = g->len < len ? g->len : len;
        memcpy(buf, g->data, n);
        if (from_ip) *from_ip = g->ip;
        if (from_port) *from_port = g->port;
        if (!peek) {
            s->q_head = g->next;
            if (!s->q_head) s->q_tail = NULL;
            s->q_n--;
            kfree(g);
        }
        return (int)n;
    }
    if (s->state == S_LISTEN || s->state == S_SYN_SENT) return -ENOTCONN;
    WAIT_FOR(s->rx_count || s->fin_rcvd || s->err || s->state == S_CLOSED, nonblock);
    if (!s->rx_count) {
        if (s->err) { int e = s->err; s->err = 0; return -e; }
        return 0;                                          /* orderly EOF */
    }
    uint32_t n = s->rx_count < len ? s->rx_count : len;
    bool was_full = rx_window(s) < MSS;
    for (uint32_t i = 0; i < n; i++) buf[i] = s->rx[(s->rx_head + i) % RX_CAP];
    if (!peek) {
        s->rx_head = (s->rx_head + n) % RX_CAP;
        s->rx_count -= n;
        if (was_full && s->state != S_CLOSED)              /* window update */
            tcp_segment(s, s->snd_nxt, TCP_ACK, NULL, 0);
    }
    if (from_ip) *from_ip = s->rip;
    if (from_port) *from_port = s->rport;
    return (int)n;
}

int sock_shutdown(sock_t* s, int how) {
    if (s->type != 1) return 0;
    if (s->state != S_ESTABLISHED && s->state != S_CLOSE_WAIT && s->state != S_FIN_WAIT1 &&
        s->state != S_FIN_WAIT2) return -ENOTCONN;
    if (how == 1 || how == 2) {
        s->shut_wr = true;
        uint32_t f = irq_save();
        tcp_output(s);
        irq_restore(f);
    }
    return 0;
}

void sock_name(sock_t* s, bool peer, uint32_t* ip, uint16_t* port) {
    *ip = peer ? s->rip : s->lip;
    *port = peer ? s->rport : s->lport;
}

int sock_take_error(sock_t* s) { int e = s->err; s->err = 0; return e; }

bool sock_readable(sock_t* s) {
    if (s->type == 2) return s->q_head != NULL;
    if (s->state == S_LISTEN) return s->aq_n > 0;
    return s->rx_count || s->fin_rcvd || s->err || s->state == S_CLOSED;
}

bool sock_writable(sock_t* s) {
    if (s->type == 2) return true;
    if (s->state == S_SYN_SENT || s->state == S_LISTEN || s->state == S_SYN_RCVD) return false;
    return s->tx_len < TX_CAP || s->err || s->state == S_CLOSED;
}

bool sock_hup(sock_t* s) {
    return s->type == 1 && (s->state == S_CLOSED || (s->fin_rcvd && s->shut_wr));
}
