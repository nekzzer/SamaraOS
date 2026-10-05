#include "net/sock.h"
#include "net/net.h"
#include "core/io.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"
#include "proc/proc.h"
#include "core/clock.h"

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

#define MAX_SOCKS  256
#define RX_CAP     (64u * 1024u)
#define TX_CAP     (64u * 1024u)
#define MSS        1400
#define ACCEPT_MAX 64
#define UDP_QMAX   32

enum { S_CLOSED, S_LISTEN, S_SYN_SENT, S_SYN_RCVD, S_ESTABLISHED, S_FIN_WAIT1,
       S_FIN_WAIT2, S_CLOSING, S_TIME_WAIT, S_CLOSE_WAIT, S_LAST_ACK };

typedef struct dgram {
    struct dgram* next;
    uint8_t  ip[16];
    uint16_t port;
    uint16_t len;
    uint8_t  ttl;
    uint32_t ts_s, ts_us;
    uint8_t  data[];
} dgram_t;

struct sock {
    bool     used;
    int      refs;                 /* open file descriptions (0 = orphaned) */
    int      type;                 /* 1 stream, 2 dgram, 3 raw */
    int      state;
    int      af, proto;            /* 2 or 10 as the user sees it */
    bool     v6only;
    uint8_t  lip[16], rip[16];     /* ipv4 is ::ffff:a.b.c.d, zero = any */
    uint16_t lport, rport;
    bool     bound, connected;
    int      err;
    uint32_t rcvtmo;               /* ms, 0 = forever */
    uint8_t  ttl, opts;            /* opts: 1 recvttl, 2 timestamp, 4 timestampns, 8 recvhoplimit */
    uint8_t  l_ttl;                /* what the last recv saw, for cmsg */
    uint32_t l_s, l_us;

    /* TCP */
    uint32_t iss, snd_una, snd_nxt, rcv_nxt, tx_seq;
    uint16_t snd_wnd;
    uint16_t adv_wnd;          /* receive window we last advertised */
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
static const uint8_t zero_ip[16];
static uint16_t next_eph = 49152;

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t be32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static bool is4(const uint8_t* a) {
    for (int i = 0; i < 10; i++) if (a[i]) return false;
    return a[10] == 0xFF && a[11] == 0xFF;
}
static bool zero16(const uint8_t* a) { for (int i = 0; i < 16; i++) if (a[i]) return false; return true; }
static void map4(uint8_t* o, uint32_t ip) {
    memset(o, 0, 10);
    o[10] = o[11] = 0xFF;
    o[12] = ip >> 24; o[13] = ip >> 16; o[14] = ip >> 8; o[15] = ip;
}
static uint32_t un4(const uint8_t* a) { return (uint32_t)a[12] << 24 | a[13] << 16 | a[14] << 8 | a[15]; }

/* does the packet's dst hit our local address */
static bool dst_ok(sock_t* s, const uint8_t* dst) {
    if (zero16(s->lip)) return s->af == 10 ? (!is4(dst) || !s->v6only) : is4(dst);
    return !memcmp(s->lip, dst, 16);
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

static uint16_t l4_sum(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* d, int len) {
    if (!is4(dst)) return ip6_sum(src, dst, proto, d, len);
    uint8_t ph[12];
    memcpy(ph, src + 12, 4); memcpy(ph + 4, dst + 12, 4);
    ph[8] = 0; ph[9] = proto; ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;
    uint32_t s = sum16(sum16(0, ph, 12), d, len);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static int ip_out(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* d, int len) {
    if (is4(dst)) { net_send_ip(un4(dst), proto, d, len); return 0; }
    return ip6_send(src, dst, proto, d, len);
}

/* ---------------- allocation ---------------- */

static sock_t* alloc_sock(int type, int af) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->type = type;
        s->af = af;
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

/* The NIC is polled and its receive ring holds 64 KiB (~42 full frames with
   headers): never let the peer have more in flight than fits, or bursts
   overflow the ring and every drop costs a retransmit timeout. */
#define RX_WND_MAX (48u * 1024u)
#define RX_MSS     1460                     /* advertised in SYNs (default would be 536) */

static uint16_t rx_window(sock_t* s) {
    uint32_t free = RX_CAP - s->rx_count;
    return (uint16_t)(free > RX_WND_MAX ? RX_WND_MAX : free);
}

static void tcp_segment(sock_t* s, uint32_t seq, uint8_t flags, const uint8_t* data, uint32_t len) {
    uint8_t buf[24 + MSS];
    tcph_t* h = (tcph_t*)buf;
    uint32_t hl = 20;
    if (flags & TCP_SYN) {                           /* MSS option */
        int mss = is4(s->rip) ? RX_MSS : RX_MSS - 20;
        buf[20] = 2; buf[21] = 4; buf[22] = mss >> 8; buf[23] = mss & 0xFF;
        hl = 24;
    }
    h->sport = be16(s->lport);
    h->dport = be16(s->rport);
    h->seq = be32(seq);
    h->ack = be32((flags & TCP_ACK) ? s->rcv_nxt : 0);
    h->off = (uint8_t)((hl / 4) << 4);
    h->flags = flags;
    s->adv_wnd = rx_window(s);
    h->win = be16(s->adv_wnd);
    h->sum = 0;
    h->urg = 0;
    if (len) memcpy(buf + hl, data, len);
    h->sum = be16(l4_sum(s->lip, s->rip, 6, buf, (int)(hl + len)));
    ip_out(s->lip, s->rip, 6, buf, (int)(hl + len));
}

static void send_rst(const uint8_t* src, const uint8_t* dst, const tcph_t* in, int datalen) {
    sock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.lip, dst, 16); memcpy(tmp.rip, src, 16);
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

static sock_t* find_tcp(const uint8_t* src, uint16_t sport, const uint8_t* dst, uint16_t dport) {
    sock_t* listener = NULL;
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != 1 || s->lport != dport) continue;
        if (s->state == S_LISTEN) {
            if (dst_ok(s, dst)) listener = s;
            continue;
        }
        if (s->state != S_CLOSED && s->rport == sport && !memcmp(s->rip, src, 16) &&
            dst_ok(s, dst)) return s;
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

static void tcp_in(const uint8_t* src, const uint8_t* dst, const uint8_t* seg, int len) {
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
        sock_t* c = alloc_sock(1, s->af);
        if (!c) return;
        c->refs = 0;
        c->parent = s;
        memcpy(c->lip, dst, 16); c->lport = dport;
        memcpy(c->rip, src, 16); c->rport = sport;
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

void sock_input_tcp(uint32_t src, uint32_t dst, const uint8_t* seg, int len) {
    uint8_t a[16], b[16];
    map4(a, src); map4(b, dst);
    tcp_in(a, b, seg, len);
}
void sock_input_tcp6(const uint8_t* src, const uint8_t* dst, const uint8_t* seg, int len) { tcp_in(src, dst, seg, len); }

/* ---------------- UDP ---------------- */

/* nobody on that udp port: icmp port unreachable. the ip header is rebuilt, we only get the payload here */
static void unreach4(const uint8_t* src, const uint8_t* dst, const uint8_t* d, int ulen) {
    uint8_t m[8 + 20 + 8];
    uint32_t x = 0;
    if (src[12] == 0 || dst[12] >= 224 || (dst[12] == 255 && dst[15] == 255)) return;
    memset(m, 0, sizeof(m));
    m[0] = 3; m[1] = 3;
    uint8_t* ih = m + 8;
    ih[0] = 0x45; ih[2] = 0; ih[3] = (uint8_t)(20 + ulen); ih[8] = 64; ih[9] = 17;
    memcpy(ih + 12, src + 12, 4); memcpy(ih + 16, dst + 12, 4);
    x = sum16(0, ih, 20);
    while (x >> 16) x = (x & 0xFFFF) + (x >> 16);
    ih[10] = (uint8_t)(~x >> 8); ih[11] = (uint8_t)~x;
    memcpy(m + 28, d, 8);
    x = sum16(0, m, sizeof(m));
    while (x >> 16) x = (x & 0xFFFF) + (x >> 16);
    m[2] = (uint8_t)(~x >> 8); m[3] = (uint8_t)~x;
    net_send_ip(un4(src), 1, m, sizeof(m));
}

static void udp_in(const uint8_t* src, const uint8_t* dst, const uint8_t* d, int len) {
    if (len < 8) return;
    const udph_t* h = (const udph_t*)d;
    uint16_t sport = be16(h->sport), dport = be16(h->dport);
    int ulen = be16(h->len);
    if (ulen < 8 || ulen > len) return;
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != 2 || s->proto == 58 || s->proto == 1 || s->lport != dport) continue;
        if (!dst_ok(s, dst) && !(is4(dst) && dst[12] == 127 && is4(s->lip))) continue;
        if (s->connected && (memcmp(s->rip, src, 16) || s->rport != sport)) continue;
        if (s->q_n >= UDP_QMAX) return;
        dgram_t* g = (dgram_t*)kmalloc(sizeof(dgram_t) + (uint32_t)(ulen - 8));
        if (!g) return;
        g->next = NULL; memcpy(g->ip, src, 16); g->port = sport; g->len = (uint16_t)(ulen - 8);
        memcpy(g->data, d + 8, (uint32_t)(ulen - 8));
        if (s->q_tail) s->q_tail->next = g; else s->q_head = g;
        s->q_tail = g;
        s->q_n++;
        return;
    }
    if (is4(src) && is4(dst)) unreach4(src, dst, d, ulen);
}

void sock_input_udp(uint32_t src, uint32_t dst, const uint8_t* d, int len) {
    uint8_t a[16], b[16];
    map4(a, src); map4(b, dst);
    udp_in(a, b, d, len);
}
void sock_input_udp6(const uint8_t* src, const uint8_t* dst, const uint8_t* d, int len) { udp_in(src, dst, d, len); }

static void q_icmp(sock_t* s, const uint8_t* src, const uint8_t* m, int len, int ttl) {
    if (s->q_n >= UDP_QMAX) return;
    dgram_t* g = (dgram_t*)kmalloc(sizeof(dgram_t) + (uint32_t)len);
    if (!g) return;
    g->next = NULL; memcpy(g->ip, src, 16); g->port = 0; g->len = (uint16_t)len;
    g->ttl = (uint8_t)ttl;
    clock_now_us(&g->ts_s, &g->ts_us);
    memcpy(g->data, m, (uint32_t)len);
    if (s->q_tail) s->q_tail->next = g; else s->q_head = g;
    s->q_tail = g;
    s->q_n++;
}

/* icmp v4: p is the whole ip packet (raw sockets want the header too) */
void sock_input_icmp(uint32_t src4, uint32_t dst4, const uint8_t* p, int ihl, int total) {
    const uint8_t* m = p + ihl;
    int len = total - ihl;
    uint8_t src[16], dst[16];
    map4(src, src4); map4(dst, dst4);
    if (len < 8) return;
    if (m[0] == 3 && len >= 8 + 20 + 8) {              /* dest unreach: tell tcp/udp sockets */
        const uint8_t* in = m + 8;
        int il = (in[0] & 15) * 4;
        if (il >= 20 && len >= 8 + il + 8) {
            const uint8_t* t = in + il;
            uint16_t sp = (t[0] << 8) | t[1], dp = (t[2] << 8) | t[3];
            uint8_t odst[16];
            map4(odst, (uint32_t)in[16] << 24 | in[17] << 16 | in[18] << 8 | in[19]);
            int e = m[1] == 3 ? ECONNREFUSED : m[1] == 2 ? ECONNREFUSED : m[1] == 1 ? 113 : ENETUNREACH;
            for (int i = 0; i < MAX_SOCKS; i++) {
                sock_t* s = &socks[i];
                if (!s->used || s->af != 2 || s->lport != sp || s->rport != dp || memcmp(s->rip, odst, 16)) continue;
                if (s->type == 1 && in[9] == 6 && s->state == S_SYN_SENT) { s->err = e; s->state = S_CLOSED; }
                else if (s->type == 2 && in[9] == 17 && s->connected && !s->proto) s->err = e;
                else if (s->type == 2 && in[9] == 17 && s->connected && s->proto == 17) s->err = e;
            }
        }
    }
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->proto != 1 || s->af != 2 || s->type == 1) continue;
        if (!zero16(s->lip) && memcmp(s->lip, dst, 16)) continue;
        if (s->type == 2) {
            if (m[0] != 0 || ((m[4] << 8) | m[5]) != s->lport) continue;
            if (s->connected && memcmp(s->rip, src, 16)) continue;
            q_icmp(s, src, m, len, p[8]);
        } else q_icmp(s, src, p, total, p[8]);
    }
}

/* icmpv6 for raw sockets (and the ping kind of dgram ones) */
void sock_input_icmp6(const uint8_t* src, const uint8_t* dst, const uint8_t* m, int len, int hl) {
    if (m[0] == 1 && len >= 8 + 40 + 4 && (m[8 + 6] == 6 || m[8 + 6] == 17)) {      /* dest unreach for our syn / udp */
        const uint8_t* in = m + 8;
        uint16_t sp = (in[40] << 8) | in[41], dp = (in[42] << 8) | in[43];
        for (int i = 0; i < MAX_SOCKS; i++) {
            sock_t* s = &socks[i];
            if (!s->used || s->lport != sp || s->rport != dp || memcmp(s->rip, in + 24, 16)) continue;
            if (in[6] == 6 && s->type == 1 && s->state == S_SYN_SENT) { s->err = ENETUNREACH; s->state = S_CLOSED; }
            else if (in[6] == 17 && s->type == 2 && s->connected && s->proto != 58) s->err = m[1] == 4 ? ECONNREFUSED : ENETUNREACH;
        }
        return;
    }
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->proto != 58 || s->type == 1) continue;
        if (s->type == 2 && (m[0] != 129 || len < 8 || ((m[4] << 8) | m[5]) != s->lport)) continue;
        if (!zero16(s->lip) && memcmp(s->lip, dst, 16)) continue;
        if (s->connected && memcmp(s->rip, src, 16)) continue;
        q_icmp(s, src, m, len, hl);
    }
}

/* ---------------- netd ---------------- */

static void pump(void) {
    uint32_t f = irq_save();
    net_poll();
    sock_tick();
    irq_restore(f);
}

static void ser(const char* m) { while (*m) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m++); } }

static void netd(void) {
    net_init();                                        /* NIC may be absent: loopback still works */
    ser("samara: "); ser(net_status()); ser("\r\n");
    for (;;) {
        pump();
        task_sleep_ms(2);
    }
}

void sock_init(void) { task_spawn("netd", netd); }

/* Wait for `cond` with packet processing; 0, -EAGAIN or -EINTR. */
#define WAIT_FOR(cond, nonblock)                                   \
    do {                                                           \
        uint32_t t0_ = pit_uptime_ms();                            \
        pump();                                                    \
        while (!(cond)) {                                          \
            if (nonblock) return -EAGAIN;                          \
            if (s->rcvtmo && pit_uptime_ms() - t0_ >= s->rcvtmo) return -EAGAIN; \
            if (proc_interrupted()) return -EINTR;                 \
            task_yield();                                          \
            pump();                                                \
        }                                                          \
    } while (0)

/* ---------------- API ---------------- */

sock_t* sock_create(int af, int type, int proto, int* err) {
    if (type < 1 || type > 3) { *err = -EINVAL; return NULL; }
    sock_t* s = alloc_sock(type, af);
    if (!s) { *err = -ENOMEM; return NULL; }
    s->proto = proto;
    if (type == 2 && (proto == 58 || proto == 1) && !s->lport) s->lport = ephemeral(2);       /* ping socket: the port is the echo id */
    return s;
}

int sock_af(sock_t* s) { return s->af; }

int sock_v6only(sock_t* s, int set, int val) {
    if (set >= 0) s->v6only = val != 0;
    return s->v6only;
}

void sock_ref(sock_t* s) { s->refs++; }
int  sock_type(sock_t* s) { return s->type; }

void sock_close(sock_t* s) {
    if (--s->refs > 0) return;
    if (s->type != 1 || s->state == S_CLOSED || s->state == S_SYN_SENT) { free_sock(s); return; }
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

int sock_bind(sock_t* s, const uint8_t* ip, uint16_t port) {
    if (s->bound) return -EINVAL;
    if (s->type == 3) { memcpy(s->lip, ip, 16); s->bound = true; return 0; }
    if (!port) port = ephemeral(s->type);
    else if (port_in_use(s->type, port)) return -EADDRINUSE;
    memcpy(s->lip, ip, 16);
    s->lport = port;
    s->bound = true;
    return 0;
}

int sock_listen(sock_t* s, int backlog) {
    if (s->type != 1) return -EOPNOTSUPP;
    if (s->state != S_CLOSED && s->state != S_LISTEN) return -EINVAL;
    if (!s->bound) { int r = sock_bind(s, zero_ip, 0); if (r < 0) return r; }
    s->state = S_LISTEN;
    s->backlog = backlog <= 0 ? 1 : backlog > ACCEPT_MAX ? ACCEPT_MAX : backlog;
    return 0;
}

sock_t* sock_accept(sock_t* s, bool nonblock, int* err, uint8_t* ip, uint16_t* port) {
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
    memcpy(ip, c->rip, 16);
    *port = c->rport;
    return c;
}

int sock_connect(sock_t* s, const uint8_t* ip, uint16_t port, bool nonblock) {
    if (s->type != 1) {                                /* UDP: default destination */
        if (!s->bound) sock_bind(s, zero_ip, 0);
        memcpy(s->rip, ip, 16); s->rport = port; s->connected = true;
        return 0;
    }
    uint8_t src[16];
    if (s->state == S_ESTABLISHED) return -EISCONN;
    if (s->state == S_SYN_SENT) {
        if (nonblock) return -EALREADY;
    } else {
        if (s->state != S_CLOSED) return -EINVAL;
        if (is4(ip) && s->af == 10 && s->v6only) return -ENETUNREACH;
        if (!is4(ip) && (!ip6_src_for(ip, src))) return -ENETUNREACH;      /* fast, so getaddrinfo falls back to v4 */
        if (!s->bound) { int r = sock_bind(s, zero_ip, 0); if (r < 0) return r; }
        memcpy(s->rip, ip, 16);
        s->rport = port;
        if (!is4(ip)) { if (zero16(s->lip) || is4(s->lip)) memcpy(s->lip, src, 16); }
        else if (zero16(s->lip) || un4(ip) >> 24 == 127) map4(s->lip, net_src_for(un4(ip)));
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
              const uint8_t* to_ip, const uint16_t* to_port) {
    if (s->type != 1) {
        const uint8_t* ip = to_ip ? to_ip : s->rip;
        uint16_t port = to_port ? *to_port : s->rport;
        uint8_t src[16];
        if (s->err) { int e = s->err; s->err = 0; return -e; }
        if (!to_ip && !s->connected) return -EDESTADDRREQ;
        if (len > 1472) return -90;                    /* EMSGSIZE */
        if (is4(ip)) {
            uint32_t d = un4(ip);
            if (!zero16(s->lip) && is4(s->lip) && d >> 24 != 127) memcpy(src, s->lip, 16);
            else map4(src, net_src_for(d));
        } else if (!zero16(s->lip) && !is4(s->lip)) memcpy(src, s->lip, 16);
        else if (!ip6_src_for(ip, src)) return -ENETUNREACH;
        if (s->type == 3 || s->proto == 58 || s->proto == 1) {   /* icmp: whole message from the user */
            uint8_t m[1472];
            if (len < 8) return -EINVAL;
            memcpy(m, buf, len);
            if (s->type == 2) { m[4] = s->lport >> 8; m[5] = (uint8_t)s->lport; }
            if (s->proto == 58) {
                m[2] = m[3] = 0;
                uint16_t c = l4_sum(src, ip, 58, m, (int)len);
                m[2] = c >> 8; m[3] = (uint8_t)c;
            } else if (s->type == 2) {                 /* ping socket v4, id changed so redo the sum */
                uint32_t x = 0;
                m[2] = m[3] = 0;
                x = sum16(0, m, (int)len);
                while (x >> 16) x = (x & 0xFFFF) + (x >> 16);
                m[2] = (uint8_t)(~x >> 8); m[3] = (uint8_t)~x;
            }
            uint32_t f = irq_save();
            net_ttl = s->ttl;
            int r = ip_out(src, ip, (uint8_t)s->proto, m, (int)len);
            net_ttl = 0;
            irq_restore(f);
            pump();
            return r < 0 ? r : (int)len;
        }
        if (!s->bound) sock_bind(s, zero_ip, 0);
        uint8_t pkt[8 + 1472];
        udph_t* h = (udph_t*)pkt;
        h->sport = be16(s->lport);
        h->dport = be16(port);
        h->len = be16((uint16_t)(8 + len));
        h->sum = 0;
        memcpy(pkt + 8, buf, len);
        uint16_t cs = l4_sum(src, ip, 17, pkt, 8 + (int)len);
        h->sum = be16(cs ? cs : 0xFFFF);
        uint32_t f = irq_save();
        int r = ip_out(src, ip, 17, pkt, 8 + (int)len);
        irq_restore(f);
        pump();
        return r < 0 ? r : (int)len;
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
              uint8_t* from_ip, uint16_t* from_port) {
    if (s->type != 1) {
        WAIT_FOR(s->q_head || s->err, nonblock);
        if (!s->q_head) { int e = s->err; s->err = 0; return -e; }
        dgram_t* g = s->q_head;
        uint32_t n = g->len < len ? g->len : len;
        memcpy(buf, g->data, n);
        if (from_ip) memcpy(from_ip, g->ip, 16);
        if (from_port) *from_port = g->port;
        s->l_ttl = g->ttl; s->l_s = g->ts_s; s->l_us = g->ts_us;
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
    for (uint32_t i = 0; i < n; i++) buf[i] = s->rx[(s->rx_head + i) % RX_CAP];
    if (!peek) {
        s->rx_head = (s->rx_head + n) % RX_CAP;
        s->rx_count -= n;
        /* Window update once it opened by 2 segments: a sender will not
           fill a window smaller than its MSS and would sit in its persist
           timer (seconds) instead. */
        if (s->state != S_CLOSED && rx_window(s) >= (uint32_t)s->adv_wnd + 2 * RX_MSS)
            tcp_segment(s, s->snd_nxt, TCP_ACK, NULL, 0);
    }
    if (from_ip) memcpy(from_ip, s->rip, 16);
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

void sock_name(sock_t* s, bool peer, uint8_t* ip, uint16_t* port) {
    memcpy(ip, peer ? s->rip : s->lip, 16);
    *port = peer ? s->rport : s->lport;
}

int sock_take_error(sock_t* s) { int e = s->err; s->err = 0; return e; }

bool sock_readable(sock_t* s) {
    if (s->type != 1) return s->q_head != NULL || s->err;
    if (s->state == S_LISTEN) return s->aq_n > 0;
    return s->rx_count || s->fin_rcvd || s->err || s->state == S_CLOSED;
}

bool sock_writable(sock_t* s) {
    if (s->type != 1) return true;
    if (s->state == S_SYN_SENT || s->state == S_LISTEN || s->state == S_SYN_RCVD) return false;
    return s->tx_len < TX_CAP || s->err || s->state == S_CLOSED;
}

bool sock_hup(sock_t* s) {
    return s->type == 1 && (s->state == S_CLOSED || (s->fin_rcvd && s->shut_wr));
}

void sock_rcvtmo(sock_t* s, uint32_t ms) { s->rcvtmo = ms; }

/* setsockopt bits that only matter for the icmp cmsgs */
void sock_opt(sock_t* s, int level, int name, int val) {
    if (level == 0 && name == 12) s->opts = val ? s->opts | 1 : s->opts & ~1;                 /* IP_RECVTTL */
    else if (level == 0 && name == 2) s->ttl = val;                                          /* IP_TTL */
    else if (level == 1 && name == 29) s->opts = val ? s->opts | 2 : s->opts & ~2;           /* SO_TIMESTAMP */
    else if (level == 1 && name == 35) s->opts = val ? s->opts | 4 : s->opts & ~4;           /* SO_TIMESTAMPNS */
    else if (level == 41 && name == 51) s->opts = val ? s->opts | 8 : s->opts & ~8;          /* IPV6_RECVHOPLIMIT */
    else if (level == 41 && (name == 16 || name == 52)) s->ttl = val;
}

/* control messages for the datagram recv'd last, returns bytes */
int sock_cmsg(sock_t* s, uint8_t* out, int cap) {
    int n = 0;
    uint64_t* h;
    if (s->type == 1) return 0;
    if ((s->opts & 1) && s->af == 2 && s->l_ttl && n + 24 <= cap) {
        h = (uint64_t*)(out + n);
        h[0] = 20; ((int*)h)[2] = 0; ((int*)h)[3] = 2; ((int*)h)[4] = s->l_ttl; ((int*)h)[5] = 0;
        n += 24;
    }
    if ((s->opts & 8) && s->af == 10 && s->l_ttl && n + 24 <= cap) {
        h = (uint64_t*)(out + n);
        h[0] = 20; ((int*)h)[2] = 41; ((int*)h)[3] = 52; ((int*)h)[4] = s->l_ttl; ((int*)h)[5] = 0;
        n += 24;
    }
    if ((s->opts & 6) && s->l_s && n + 32 <= cap) {
        h = (uint64_t*)(out + n);
        h[0] = 32; ((int*)h)[2] = 1; ((int*)h)[3] = (s->opts & 4) ? 35 : 29;
        h[2] = s->l_s; h[3] = (s->opts & 4) ? s->l_us * 1000ull : s->l_us;
        n += 32;
    }
    return n;
}
