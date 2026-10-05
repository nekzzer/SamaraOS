#include "net/sock.h"
#include "net/net.h"
#include "core/io.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/task.h"
#include "boot/pit.h"
#include "proc/proc.h"
#include "core/clock.h"
#include "core/wq.h"

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

#define MAX_SOCKS  1024
#define BUF_DEF    (128u * 1024u)
#define BUF_MAX    (4u * 1024u * 1024u)
#define BUF_MIN    (16u * 1024u)
#define MSS_MAX    1460
#define LO_MSS     16000                /* loopback: no real mtu, see net.c loop_pkt */
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
    wq_t     wq;
    uint32_t rdy;                  /* smask() when the last waiter looked */
    uint8_t  ttl, opts;            /* opts: 1 recvttl, 2 timestamp, 4 timestampns, 8 recvhoplimit */
    uint8_t  l_ttl;                /* what the last recv saw, for cmsg */
    uint32_t l_s, l_us;

    /* TCP */
    uint32_t iss, snd_una, snd_nxt, snd_max, rcv_nxt, tx_seq;
    uint32_t snd_wnd;
    uint32_t adv_wnd;          /* receive window we last advertised */
    uint8_t  snd_ws, rcv_ws;
    bool     ws_ok, ts_ok, sack_ok;
    uint16_t mss;              /* payload per segment, without options */
    uint32_t ts_recent;
    uint8_t* rx; uint32_t rx_cap, rx_head, rx_count;
    uint8_t* tx; uint32_t tx_cap, tx_head, tx_len;
    uint32_t ooo[6][2]; int ooo_n;              /* received above rcv_nxt, newest first */
    uint32_t sb[8][2]; int sb_n;                /* what the peer sacked, sorted */
    uint32_t cwnd, ssthresh, recover, rexmit_hi, dupacks, acked_acc;
    uint8_t  rec;                               /* 0 no, 1 fast recovery, 2 after rto */
    uint8_t  cc;                                /* 0 reno, 1 cubic */
    uint32_t cb_wmax, cb_k, cb_t0, cb_cnt;
    uint32_t srtt, rttvar, rto, rtt_seq, rtt_t0;
    bool     rtt_on, timer_on;
    bool     fin_rcvd, shut_wr, fin_sent;
    uint32_t last_tx_ms, deadline_ms, rto_at;
    int      retries;
    bool     nodelay, cork, ka_on;
    uint32_t ka_idle, ka_intvl, ka_cnt, last_rx_ms, ka_at;
    int      ka_n;
    bool     ack_now;
    uint32_t ack_at;
    uint8_t  ack_segs, quick;
    uint32_t n_retrans, last_data_ms, last_ack_ms;
    uint32_t want_rx, want_tx;                  /* SO_RCVBUF/SO_SNDBUF, 0 = default */
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

static bool tcp_bufs(sock_t* s, uint32_t rc, uint32_t tc) {
    s->rx = (uint8_t*)kmalloc_big(rc);
    s->tx = (uint8_t*)kmalloc_big(tc);
    if (!s->rx || !s->tx) {
        if (s->rx) kfree(s->rx);
        if (s->tx) kfree(s->tx);
        s->rx = s->tx = NULL;
        return false;
    }
    s->rx_cap = rc; s->tx_cap = tc;
    return true;
}

static uint32_t buf_clamp(uint32_t v) { return v < BUF_MIN ? BUF_MIN : v > BUF_MAX ? BUF_MAX : v; }

/* resize the rings, keeps what is queued; stays as it was if there is no memory */
static void tcp_setbuf(sock_t* s, uint32_t rc, uint32_t tc) {
    rc = buf_clamp(rc); tc = buf_clamp(tc);
    if (!s->rx) return;
    if (rc != s->rx_cap && rc >= s->rx_count) {
        uint8_t* r = (uint8_t*)kmalloc_big(rc);
        if (r) {
            for (uint32_t i = 0; i < s->rx_count; i++) r[i] = s->rx[(s->rx_head + i) % s->rx_cap];
            kfree(s->rx);
            s->rx = r; s->rx_cap = rc; s->rx_head = 0;
            s->ooo_n = 0;
        }
    }
    if (tc != s->tx_cap && tc >= s->tx_len) {
        uint8_t* r = (uint8_t*)kmalloc_big(tc);
        if (r) {
            for (uint32_t i = 0; i < s->tx_len; i++) r[i] = s->tx[(s->tx_head + i) % s->tx_cap];
            kfree(s->tx);
            s->tx = r; s->tx_cap = tc; s->tx_head = 0;
        }
    }
}

static sock_t* alloc_sock(int type, int af) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (s->used) continue;
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->type = type;
        s->af = af;
        s->refs = 1;
        s->rto = 1000;
        s->mss = 536;
        s->ka_idle = 7200; s->ka_intvl = 75; s->ka_cnt = 9;
        if (type == 1 && !tcp_bufs(s, BUF_DEF, BUF_DEF)) {
            s->used = false;
            return NULL;
        }
        return s;
    }
    return NULL;
}

static void free_sock(sock_t* s) {
    wq_drain(&s->wq);
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

/* The NIC is polled and its receive ring is small (rtl8139: 64 KiB, ~42 full
   frames): never let the peer have more in flight than fits, or bursts
   overflow the ring and every drop costs a retransmit. net_rxwnd knows. */

typedef struct {
    int mss, ws;
    bool sackok, ts;
    uint32_t tsval, tsecr;
    int nsack;
    uint32_t sack[4][2];
} topt_t;

static inline int32_t sd(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void parse_opts(const uint8_t* p, int len, topt_t* o) {
    memset(o, 0, sizeof(*o));
    o->ws = -1;
    while (len > 0) {
        uint8_t k = p[0];
        if (k == 0) break;
        if (k == 1) { p++; len--; continue; }
        if (len < 2 || p[1] < 2 || p[1] > len) break;
        int l = p[1];
        if (k == 2 && l == 4) o->mss = (p[2] << 8) | p[3];
        else if (k == 3 && l == 3) o->ws = p[2] > 14 ? 14 : p[2];
        else if (k == 4 && l == 2) o->sackok = true;
        else if (k == 8 && l == 10) {
            o->ts = true;
            o->tsval = (uint32_t)p[2] << 24 | p[3] << 16 | p[4] << 8 | p[5];
            o->tsecr = (uint32_t)p[6] << 24 | p[7] << 16 | p[8] << 8 | p[9];
        } else if (k == 5 && (l - 2) % 8 == 0) {
            for (int i = 2; i + 8 <= l && o->nsack < 4; i += 8, o->nsack++) {
                o->sack[o->nsack][0] = (uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3];
                o->sack[o->nsack][1] = (uint32_t)p[i + 4] << 24 | p[i + 5] << 16 | p[i + 6] << 8 | p[i + 7];
            }
        }
        p += l; len -= l;
    }
}

static uint32_t wnd_cap(sock_t* s) {
    if (is4(s->rip)) return net_rxwnd(un4(s->rip));
    for (int i = 0; i < 15; i++) if (s->rip[i]) return net_rxwnd(0);
    return s->rip[15] == 1 ? net_rxwnd(0x7F000001) : net_rxwnd(0);
}

static uint32_t rx_window(sock_t* s) {
    uint32_t fr = s->rx_cap - s->rx_count;
    uint32_t cap = wnd_cap(s);
    uint32_t w = fr > cap ? cap : fr;
    return (w >> s->rcv_ws) << s->rcv_ws;
}

static bool is_lo(const uint8_t* a) { return is4(a) && a[12] == 127; }
static int link_mss(const uint8_t* rip) { return is_lo(rip) ? LO_MSS : is4(rip) ? MSS_MAX : MSS_MAX - 20; }

static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void tcp_segment(sock_t* s, uint32_t seq, uint8_t flags, uint32_t len) {
    uint8_t sbuf[60 + MSS_MAX + 16];
    uint8_t* buf = sbuf;
    if (len > MSS_MAX) { buf = net_buf_get(1); if (!buf) return; }
    tcph_t* h = (tcph_t*)buf;
    uint8_t* o = buf + 20;
    uint32_t now = pit_uptime_ms();
    if (flags & TCP_SYN) {
        int mss = link_mss(s->rip);
        *o++ = 2; *o++ = 4; *o++ = mss >> 8; *o++ = mss & 0xFF;
        if (s->ts_ok) {
            if (s->sack_ok) { *o++ = 4; *o++ = 2; } else { *o++ = 1; *o++ = 1; }
            *o++ = 8; *o++ = 10; put32(o, now + 1); put32(o + 4, s->ts_recent); o += 8;
        } else if (s->sack_ok) { *o++ = 4; *o++ = 2; *o++ = 1; *o++ = 1; }
        if (s->ws_ok) { *o++ = 1; *o++ = 3; *o++ = 3; *o++ = s->rcv_ws; }
    } else {
        if (s->ts_ok) {
            *o++ = 1; *o++ = 1; *o++ = 8; *o++ = 10;
            put32(o, now + 1); put32(o + 4, s->ts_recent); o += 8;
        }
        if (s->sack_ok && s->ooo_n && !len) {
            int n = s->ooo_n > (s->ts_ok ? 3 : 4) ? (s->ts_ok ? 3 : 4) : s->ooo_n;
            *o++ = 1; *o++ = 1; *o++ = 5; *o++ = 2 + 8 * n;
            for (int i = 0; i < n; i++) { put32(o, s->ooo[i][0]); put32(o + 4, s->ooo[i][1]); o += 8; }
        }
    }
    uint32_t hl = (uint32_t)(o - buf);
    h->sport = be16(s->lport);
    h->dport = be16(s->rport);
    h->seq = be32(seq);
    h->ack = be32((flags & TCP_ACK) ? s->rcv_nxt : 0);
    h->off = (uint8_t)((hl / 4) << 4);
    h->flags = flags;
    s->adv_wnd = rx_window(s);
    if (flags & TCP_SYN) { uint32_t w = s->adv_wnd > 65535 ? 65535 : s->adv_wnd; h->win = be16((uint16_t)w); }
    else h->win = be16((uint16_t)(s->adv_wnd >> s->rcv_ws));
    h->sum = 0;
    h->urg = 0;
    if (flags & TCP_ACK) { s->ack_now = false; s->ack_at = 0; s->ack_segs = 0; s->last_ack_ms = now; }
    if (len) {
        uint32_t off = (s->tx_head + (seq - s->tx_seq)) % s->tx_cap;
        uint32_t n1 = s->tx_cap - off < len ? s->tx_cap - off : len;
        memcpy(buf + hl, s->tx + off, n1);
        if (len > n1) memcpy(buf + hl + n1, s->tx, len - n1);
        s->last_data_ms = now;
    }
    if (!is_lo(s->rip)) h->sum = be16(l4_sum(s->lip, s->rip, 6, buf, (int)(hl + len)));
    ip_out(s->lip, s->rip, 6, buf, (int)(hl + len));
    if (buf != sbuf) net_buf_put(buf, 1);
}

static void send_rst(const uint8_t* src, const uint8_t* dst, const tcph_t* in, int datalen) {
    sock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.lip, dst, 16); memcpy(tmp.rip, src, 16);
    tmp.lport = be16(in->dport); tmp.rport = be16(in->sport);
    if (in->flags & TCP_ACK) {
        tcp_segment(&tmp, be32(in->ack), TCP_RST, 0);
    } else {
        tmp.rcv_nxt = be32(in->seq) + (uint32_t)datalen + ((in->flags & TCP_SYN) ? 1 : 0) +
                      ((in->flags & TCP_FIN) ? 1 : 0);
        tcp_segment(&tmp, 0, TCP_RST | TCP_ACK, 0);
    }
}

/* scoreboard: what the peer sacked, sorted, merged */
static void sb_add(sock_t* s, uint32_t a, uint32_t b) {
    uint32_t t[9][2];
    int n = 0;
    bool done = false;
    if (sd(a, s->snd_una) < 0 || sd(b, a) <= 0 || sd(b, s->snd_max) > 0) return;
    for (int i = 0; i < s->sb_n; i++) {
        uint32_t x = s->sb[i][0], y = s->sb[i][1];
        if (sd(y, a) < 0) { t[n][0] = x; t[n][1] = y; n++; }
        else if (sd(x, b) > 0) {
            if (!done) { t[n][0] = a; t[n][1] = b; n++; done = true; }
            t[n][0] = x; t[n][1] = y; n++;
        } else {
            if (sd(x, a) < 0) a = x;
            if (sd(y, b) > 0) b = y;
        }
    }
    if (!done) { t[n][0] = a; t[n][1] = b; n++; }
    if (n > 8) n = 8;
    memcpy(s->sb, t, (uint32_t)n * 8);
    s->sb_n = n;
}

static void sb_trim(sock_t* s) {
    int n = 0;
    for (int i = 0; i < s->sb_n; i++) {
        if (sd(s->sb[i][1], s->snd_una) <= 0) continue;
        s->sb[n][0] = sd(s->sb[i][0], s->snd_una) < 0 ? s->snd_una : s->sb[i][0];
        s->sb[n][1] = s->sb[i][1];
        n++;
    }
    s->sb_n = n;
}

static uint32_t sack_in(sock_t* s, uint32_t a, uint32_t b) {
    uint32_t t = 0;
    for (int i = 0; i < s->sb_n; i++) {
        uint32_t x = s->sb[i][0], y = s->sb[i][1];
        if (sd(x, a) < 0) x = a;
        if (sd(y, b) > 0) y = b;
        if (sd(y, x) > 0) t += y - x;
    }
    return t;
}

/* bytes we think are still in the network */
static uint32_t flight(sock_t* s) {
    if (s->rec == 1 && s->sack_ok && s->sb_n) {
        uint32_t high = s->sb[s->sb_n - 1][1];
        uint32_t lim = sd(s->rexmit_hi, high) < 0 ? s->rexmit_hi : high;
        uint32_t r = 0;
        if (sd(lim, s->snd_una) > 0) r = (lim - s->snd_una) - sack_in(s, s->snd_una, lim);
        return (s->snd_max - high) + r;
    }
    uint32_t f = s->snd_nxt - s->snd_una;
    if (s->sack_ok) { uint32_t k = sack_in(s, s->snd_una, s->snd_nxt); f = f > k ? f - k : 0; }
    return f;
}

/* resend one segment from seq, not past end */
static uint32_t retx_one(sock_t* s, uint32_t seq, uint32_t end) {
    uint32_t ring_end = s->tx_seq + s->tx_len;
    uint32_t n = end - seq;
    if (sd(end, ring_end) > 0) n = ring_end - seq;
    if (sd(ring_end, seq) <= 0) return 0;
    if (n > s->mss) n = s->mss;
    tcp_segment(s, seq, TCP_ACK, n);
    s->n_retrans++;
    s->rtt_on = false;
    return n;
}

static void arm_rto(sock_t* s) {
    if (s->timer_on) return;
    s->timer_on = true;
    s->rto_at = pit_uptime_ms() + s->rto;
}

/* Push unsent data (and FIN when shut down) within the windows. */
static void tcp_output(sock_t* s) {
    if (s->state != S_ESTABLISHED && s->state != S_CLOSE_WAIT &&
        s->state != S_FIN_WAIT1 && s->state != S_LAST_ACK && s->state != S_CLOSING) return;
    uint32_t sent;
    for (int guard = 0; guard < 1024; guard++) {
        uint32_t fl = flight(s);
        uint32_t room = s->cwnd > fl ? s->cwnd - fl : 0;
        if (!room) break;
        if (s->rec == 1 && s->sack_ok && s->sb_n) {
            uint32_t a = sd(s->rexmit_hi, s->snd_una) > 0 ? s->rexmit_hi : s->snd_una;
            uint32_t high = s->sb[s->sb_n - 1][1];
            int i;
            for (i = 0; i < s->sb_n; i++) {
                if (sd(a, s->sb[i][0]) < 0) break;
                if (sd(a, s->sb[i][1]) < 0) a = s->sb[i][1];
            }
            if (i < s->sb_n && sd(a, high) < 0) {
                uint32_t n = retx_one(s, a, s->sb[i][0]);
                if (n) {
                    s->rexmit_hi = a + n;
                    arm_rto(s);
                    continue;
                }
            }
        }
        if (s->rec == 2)
            for (int i = 0; i < s->sb_n; i++)
                if (sd(s->snd_nxt, s->sb[i][0]) >= 0 && sd(s->snd_nxt, s->sb[i][1]) < 0) s->snd_nxt = s->sb[i][1];
        sent = s->snd_nxt - s->tx_seq;
        if (s->fin_sent && sent) sent--;
        if (sent >= s->tx_len) break;
        uint32_t avail = s->tx_len - sent;
        uint32_t used = s->snd_nxt - s->snd_una;
        uint32_t inwnd = s->snd_wnd > used ? s->snd_wnd - used : 0;
        if (!s->snd_wnd && !used) inwnd = 1;                 /* zero window probe */
        uint32_t n = avail < s->mss ? avail : s->mss;
        if (n > inwnd) n = inwnd;
        if (!n || n > room) break;
        if (n < s->mss) {
            if (n == avail && !s->shut_wr) {
                if (s->cork) break;
                if (!s->nodelay && s->snd_una != s->snd_nxt) break;      /* nagle */
            } else if (n < avail && s->snd_una != s->snd_nxt) break;
        }
        uint32_t seq = s->snd_nxt;
        bool fresh = sd(seq, s->snd_max) >= 0;
        tcp_segment(s, seq, TCP_ACK | (n == avail ? TCP_PSH : 0), n);
        s->snd_nxt += n;
        if (fresh) {
            if (!s->rtt_on) { s->rtt_on = true; s->rtt_seq = s->snd_nxt; s->rtt_t0 = pit_uptime_ms(); }
            s->snd_max = s->snd_nxt;
        } else {
            s->n_retrans++;
            s->rtt_on = false;
        }
        arm_rto(s);
        s->last_tx_ms = pit_uptime_ms();
    }
    sent = s->snd_nxt - s->tx_seq;
    if (s->fin_sent && sent) sent--;
    if (s->shut_wr && !s->fin_sent && sent == s->tx_len) {
        tcp_segment(s, s->snd_nxt, TCP_FIN | TCP_ACK, 0);
        s->snd_nxt++;
        if (sd(s->snd_nxt, s->snd_max) > 0) s->snd_max = s->snd_nxt;
        s->fin_sent = true;
        s->last_tx_ms = pit_uptime_ms();
        arm_rto(s);
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

static uint32_t icbrt(uint64_t x) {
    uint64_t lo = 0, hi = 1 << 20;
    while (lo < hi) {
        uint64_t m = (lo + hi + 1) / 2;
        if (m * m * m <= x) lo = m; else hi = m - 1;
    }
    return (uint32_t)lo;
}

static uint32_t rto_calc(sock_t* s) {
    if (!s->srtt) return 1000;
    uint32_t v = s->srtt + (s->rttvar * 4 > 4 ? s->rttvar * 4 : 4);
    return v < 200 ? 200 : v > 30000 ? 30000 : v;
}

static void rtt_sample(sock_t* s, uint32_t r) {
    if (!r) r = 1;
    if (!s->srtt) { s->srtt = r; s->rttvar = r / 2 ? r / 2 : 1; }
    else {
        uint32_t d = r > s->srtt ? r - s->srtt : s->srtt - r;
        s->rttvar = (3 * s->rttvar + d) / 4;
        s->srtt = (7 * s->srtt + r) / 8;
        if (!s->srtt) s->srtt = 1;
    }
}

static void cc_loss(sock_t* s, uint32_t fl) {
    if (s->cc) {
        s->cb_wmax = s->cwnd / s->mss;
        s->cb_t0 = 0;
        s->ssthresh = fl / 10 * 7;
    } else s->ssthresh = fl / 2;
    if (s->ssthresh < 2 * s->mss) s->ssthresh = 2 * s->mss;
}

static void cc_grow(sock_t* s, uint32_t acked) {
    if (s->cwnd < s->ssthresh) {
        s->cwnd += acked < s->mss ? acked : s->mss;
        return;
    }
    if (!s->cc) {
        s->acked_acc += acked;
        if (s->acked_acc >= s->cwnd) { s->acked_acc -= s->cwnd; s->cwnd += s->mss; }
    } else {
        uint32_t now = pit_uptime_ms();
        uint32_t cur = s->cwnd / s->mss;
        if (!s->cb_t0) {
            s->cb_t0 = now ? now : 1;
            if (s->cb_wmax <= cur) { s->cb_k = 0; s->cb_wmax = cur; }
            else s->cb_k = icbrt((uint64_t)(s->cb_wmax - cur) * 2500000000ull);
            s->cb_cnt = 0;
        }
        int64_t dt = (int64_t)(now - s->cb_t0 + s->srtt) - (int64_t)s->cb_k;
        int64_t tgt = (int64_t)s->cb_wmax + (4 * dt * dt * dt) / 10000000000ll;
        uint32_t cnt = tgt > (int64_t)cur ? cur / (uint32_t)(tgt - cur + 0) : 100 * cur;
        if (cnt < 2) cnt = 2;
        s->cb_cnt += acked / s->mss ? acked / s->mss : 1;
        if (s->cb_cnt >= cnt) { s->cb_cnt = 0; s->cwnd += s->mss; }
    }
    if (s->cwnd > (64u << 20)) s->cwnd = 64u << 20;
}

static void cc_init(sock_t* s) {
    s->cwnd = 10 * s->mss;
    s->ssthresh = 0x40000000;
    s->rec = 0;
    s->quick = 16;
}

static void enter_rec(sock_t* s) {
    cc_loss(s, flight(s));
    s->recover = s->snd_max;
    s->rec = 1;
    s->cwnd = s->sack_ok ? s->ssthresh : s->ssthresh + 3 * s->mss;
    uint32_t end = s->sb_n && sd(s->sb[0][0], s->snd_una) > 0 ? s->sb[0][0] : s->snd_max;
    s->rexmit_hi = s->snd_una + retx_one(s, s->snd_una, end);
    s->timer_on = true;
    s->rto_at = pit_uptime_ms() + s->rto;
}

static void tcp_ack_in(sock_t* s, uint32_t ack, uint32_t wnd, uint32_t dlen, topt_t* o) {
    uint32_t now = pit_uptime_ms();
    if (sd(ack, s->snd_una) < 0) return;
    if (sd(ack, s->snd_max) > 0) { s->ack_now = true; return; }
    if (s->sack_ok)
        for (int i = 0; i < o->nsack; i++) sb_add(s, o->sack[i][0], o->sack[i][1]);
    if (ack == s->snd_una) {
        if (s->state == S_SYN_RCVD) return;
        bool dup = !dlen && s->snd_max != s->snd_una && (wnd == s->snd_wnd || o->nsack);
        s->snd_wnd = wnd;
        if (!dup) return;
        s->dupacks++;
        if (s->rec == 0) {
            if (s->dupacks >= 3 || (s->sack_ok && sack_in(s, s->snd_una, s->snd_max) >= 3u * s->mss)) enter_rec(s);
        } else if (s->rec == 1 && !s->sack_ok) s->cwnd += s->mss;
        return;
    }
    uint32_t acked = ack - s->snd_una;
    uint32_t d = sd(ack, s->tx_seq) > 0 ? ack - s->tx_seq : 0;
    if (d > s->tx_len) d = s->tx_len;
    s->tx_head = (s->tx_head + d) % s->tx_cap;
    s->tx_len -= d;
    s->tx_seq += d;
    s->snd_una = ack;
    if (sd(s->snd_nxt, ack) < 0) s->snd_nxt = ack;
    sb_trim(s);
    s->snd_wnd = wnd;
    s->dupacks = 0;
    s->retries = 0;
    if (o->ts && s->ts_ok && o->tsecr) {
        uint32_t r = now + 1 - o->tsecr;
        if (r < 60000) rtt_sample(s, r);
    } else if (s->rtt_on && sd(ack, s->rtt_seq) >= 0) rtt_sample(s, now - s->rtt_t0);
    if (s->rtt_on && sd(ack, s->rtt_seq) >= 0) s->rtt_on = false;
    s->rto = rto_calc(s);
    if (ack == s->snd_max) s->timer_on = false;
    else { s->timer_on = true; s->rto_at = now + s->rto; }

    if (s->rec == 1) {
        if (sd(ack, s->recover) >= 0) {
            s->rec = 0;
            s->cwnd = s->ssthresh;
            s->acked_acc = 0;
            s->cb_t0 = 0;
        } else {
            if (!s->sack_ok || !s->sb_n) {
                uint32_t n = retx_one(s, ack, s->snd_max);
                if (sd(ack + n, s->rexmit_hi) > 0) s->rexmit_hi = ack + n;
            }
            if (!s->sack_ok) s->cwnd = s->cwnd > acked ? s->cwnd - acked + s->mss : s->mss;
        }
    } else {
        if (s->rec == 2 && sd(ack, s->recover) >= 0) s->rec = 0;
        cc_grow(s, acked);
    }
}

static void ooo_add(sock_t* s, uint32_t a, uint32_t b) {
    uint32_t t[7][2];
    int n = 0;
    for (int i = 0; i < s->ooo_n; i++) {
        uint32_t x = s->ooo[i][0], y = s->ooo[i][1];
        if (sd(y, a) < 0 || sd(x, b) > 0) { t[n + 1][0] = x; t[n + 1][1] = y; n++; }
        else { if (sd(x, a) < 0) a = x; if (sd(y, b) > 0) b = y; }
    }
    t[0][0] = a; t[0][1] = b;
    n++;
    if (n > 6) n = 6;
    memcpy(s->ooo, t, (uint32_t)n * 8);
    s->ooo_n = n;
}

static void tcp_kill(sock_t* s, int err) {
    s->err = err;
    s->state = S_CLOSED;
    s->timer_on = false;
    if (s->parent) { s->parent = NULL; free_sock(s); } else reap_if_done(s);
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
    uint32_t now = pit_uptime_ms();
    topt_t o;

    sock_t* s = find_tcp(src, sport, dst, dport);
    if (!s) { if (!(fl & TCP_RST)) send_rst(src, dst, h, (int)dlen); return; }
    parse_opts(seg + 20, hl - 20, &o);

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
        c->iss = 0x20000000u + now * 64 + (uint32_t)(c - socks) * 4096;
        c->snd_una = c->iss;
        c->snd_nxt = c->snd_max = c->iss + 1;
        c->tx_seq = c->iss + 1;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = be16(h->win);
        c->nodelay = s->nodelay; c->ka_on = s->ka_on; c->cc = s->cc;
        c->ka_idle = s->ka_idle; c->ka_intvl = s->ka_intvl; c->ka_cnt = s->ka_cnt;
        if (is_lo(src) && !s->want_rx && !s->want_tx) tcp_setbuf(c, 1 << 20, 1 << 20);
        else if (s->want_rx || s->want_tx) {
            c->want_rx = s->want_rx; c->want_tx = s->want_tx;
            tcp_setbuf(c, s->want_rx ? s->want_rx : BUF_DEF, s->want_tx ? s->want_tx : BUF_DEF);
        }
        int link = link_mss(src);
        int pm = o.mss ? o.mss : 536;
        if (pm > link) pm = link;
        c->ts_ok = o.ts; c->ts_recent = o.tsval;
        c->sack_ok = o.sackok;
        if (o.ws >= 0) { c->ws_ok = true; c->snd_ws = (uint8_t)o.ws; c->rcv_ws = 7; }
        c->mss = (uint16_t)(pm - (o.ts ? 12 : 0));
        cc_init(c);
        c->state = S_SYN_RCVD;
        c->last_rx_ms = now;
        tcp_segment(c, c->iss, TCP_SYN | TCP_ACK, 0);
        c->rto = 1000;
        c->timer_on = true; c->rto_at = now + c->rto;
        return;
    }

    if (fl & TCP_RST) {
        if (s->state == S_SYN_SENT) { if (!(fl & TCP_ACK)) return; s->err = ECONNREFUSED; }
        else if (s->state != S_TIME_WAIT && s->state != S_CLOSED) s->err = ECONNRESET;
        tcp_kill(s, s->err);
        return;
    }

    if (s->state == S_SYN_SENT) {
        if ((fl & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
            int link = link_mss(s->rip);
            int pm = o.mss ? o.mss : 536;
            if (pm > link) pm = link;
            s->ts_ok = s->ts_ok && o.ts;
            if (s->ts_ok) s->ts_recent = o.tsval;
            s->sack_ok = s->sack_ok && o.sackok;
            if (o.ws < 0) { s->ws_ok = false; s->rcv_ws = 0; s->snd_ws = 0; }
            else s->snd_ws = (uint8_t)o.ws;
            s->mss = (uint16_t)(pm - (s->ts_ok ? 12 : 0));
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = be16(h->win);
            if (!s->retries) rtt_sample(s, now - s->rtt_t0);
            s->rto = rto_calc(s);
            s->timer_on = false;
            s->state = S_ESTABLISHED;
            s->retries = 0;
            s->last_rx_ms = now;
            cc_init(s);
            tcp_segment(s, s->snd_nxt, TCP_ACK, 0);
            tcp_output(s);
        }
        return;
    }

    s->last_rx_ms = now;
    s->ka_n = 0;
    if (s->ts_ok && o.ts) {
        if (s->ts_recent && sd(o.tsval, s->ts_recent) < 0) {       /* paws */
            s->ack_now = true;
            goto out;
        }
        if (sd(seq, s->rcv_nxt) <= 0) s->ts_recent = o.tsval;
    }
    if ((fl & TCP_SYN) && s->state != S_SYN_RCVD) { s->ack_now = true; goto out; }    /* our ack got lost */

    if (fl & TCP_ACK) {
        uint32_t wnd = be16(h->win);
        if (!(fl & TCP_SYN)) wnd <<= s->snd_ws;
        tcp_ack_in(s, ack, wnd, dlen, &o);
        if (s->state == S_SYN_RCVD && ack == s->snd_nxt) {
            s->state = S_ESTABLISHED;
            sock_t* p = s->parent;
            if (p && p->used && p->state == S_LISTEN && p->aq_n < ACCEPT_MAX) {
                p->acceptq[p->aq_n++] = s;
            } else {
                s->parent = NULL;
                tcp_segment(s, s->snd_nxt, TCP_RST, 0);
                free_sock(s);
                return;
            }
        }
        bool fin_acked = s->fin_sent && s->snd_una == s->snd_nxt;
        if (fin_acked) {
            if (s->state == S_FIN_WAIT1) s->state = S_FIN_WAIT2;
            else if (s->state == S_CLOSING) { s->state = S_TIME_WAIT; s->deadline_ms = now + 1000; }
            else if (s->state == S_LAST_ACK) { s->state = S_CLOSED; s->timer_on = false; reap_if_done(s); return; }
        }
    }

    if (dlen && (s->state == S_ESTABLISHED || s->state == S_FIN_WAIT1 || s->state == S_FIN_WAIT2)) {
        uint32_t fr = s->rx_cap - s->rx_count;
        int32_t off = sd(seq, s->rcv_nxt);
        if (off < 0) {
            uint32_t cut = (uint32_t)-off;
            if (cut >= dlen) { s->ack_now = true; dlen = 0; goto nodata; }    /* old stuff */
            data += cut; dlen -= cut; seq += cut; off = 0;
        }
        if ((uint32_t)off >= fr) { s->ack_now = true; dlen = 0; fl &= ~TCP_FIN; goto nodata; }
        if ((uint32_t)off + dlen > fr) { dlen = fr - (uint32_t)off; fl &= ~TCP_FIN; }
        uint32_t pos = (s->rx_head + s->rx_count + (uint32_t)off) % s->rx_cap;
        uint32_t n1 = s->rx_cap - pos < dlen ? s->rx_cap - pos : dlen;
        memcpy(s->rx + pos, data, n1);
        if (dlen > n1) memcpy(s->rx, data + n1, dlen - n1);
        if (off == 0) {
            bool hole = s->ooo_n > 0;
            s->rx_count += dlen;
            s->rcv_nxt += dlen;
            for (bool again = true; again; ) {
                again = false;
                for (int i = 0; i < s->ooo_n; i++) {
                    if (sd(s->ooo[i][0], s->rcv_nxt) > 0) continue;
                    if (sd(s->ooo[i][1], s->rcv_nxt) > 0) {
                        uint32_t adv = s->ooo[i][1] - s->rcv_nxt;
                        s->rx_count += adv;
                        s->rcv_nxt += adv;
                    }
                    memmove(&s->ooo[i], &s->ooo[i + 1], (uint32_t)(s->ooo_n - i - 1) * 8);
                    s->ooo_n--;
                    again = true;
                    break;
                }
            }
            s->ack_segs++;
            if (hole || s->ooo_n || s->ack_segs >= 2) s->ack_now = true;
            else if (s->quick) { s->quick--; s->ack_now = true; }
            else if (!s->ack_at) s->ack_at = now + 40;
        } else {
            ooo_add(s, seq, seq + dlen);
            s->ack_now = true;                           /* dupack + sack */
        }
    }
nodata:
    if ((fl & TCP_FIN) && seq + dlen == s->rcv_nxt && !s->fin_rcvd &&
        (s->state == S_ESTABLISHED || s->state == S_FIN_WAIT1 || s->state == S_FIN_WAIT2)) {
        s->rcv_nxt++;
        s->fin_rcvd = true;
        s->ack_now = true;
        if (s->state == S_ESTABLISHED) s->state = S_CLOSE_WAIT;
        else if (s->state == S_FIN_WAIT1) s->state = (s->snd_una == s->snd_nxt) ? S_TIME_WAIT : S_CLOSING;
        else if (s->state == S_FIN_WAIT2) s->state = S_TIME_WAIT;
        if (s->state == S_TIME_WAIT) s->deadline_ms = now + 1000;
    } else if ((fl & TCP_FIN) && s->fin_rcvd) {
        s->ack_now = true;                               /* retransmitted FIN */
    }
out:
    tcp_output(s);
    if (s->ack_now && s->state != S_CLOSED) tcp_segment(s, s->snd_max, TCP_ACK, 0);
}

/* Retransmission + timers, from netd and blocking waits. */
static void sock_tick(void) {
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || s->type != 1) continue;
        if (s->state == S_TIME_WAIT) { if (now >= s->deadline_ms) { s->state = S_CLOSED; reap_if_done(s); } continue; }
        if (s->state == S_CLOSED || s->state == S_LISTEN) continue;
        if (s->ack_at && sd(now, s->ack_at) >= 0) tcp_segment(s, s->snd_max, TCP_ACK, 0);
        if (s->timer_on && sd(now, s->rto_at) >= 0) {
            bool syn = s->state == S_SYN_SENT || s->state == S_SYN_RCVD;
            bool probe = !syn && !s->snd_wnd;
            if (!probe && ++s->retries > (syn ? 6 : 12)) {
                if (s->state != S_SYN_SENT) tcp_segment(s, s->snd_nxt, TCP_RST, 0);
                tcp_kill(s, ETIMEDOUT);
                continue;
            }
            s->rto = s->rto * 2 > 30000 ? 30000 : s->rto * 2;
            s->rto_at = now + s->rto;
            s->last_tx_ms = now;
            if (s->state == S_SYN_SENT) { tcp_segment(s, s->iss, TCP_SYN, 0); continue; }
            if (s->state == S_SYN_RCVD) { tcp_segment(s, s->iss, TCP_SYN | TCP_ACK, 0); continue; }
            if (!probe) {
                cc_loss(s, s->rec ? s->ssthresh * 2 : flight(s));
                s->cwnd = s->mss;
                s->recover = s->snd_max;
                s->rec = 2;
                s->acked_acc = 0;
                if (s->retries >= 2) s->sb_n = 0;
            }
            s->dupacks = 0;
            s->rtt_on = false;
            bool fin_pending = s->fin_sent;
            s->snd_nxt = s->snd_una;
            s->fin_sent = false;
            if (fin_pending && s->state == S_FIN_WAIT1) s->state = S_ESTABLISHED;
            else if (fin_pending && s->state == S_LAST_ACK) s->state = S_CLOSE_WAIT;
            else if (fin_pending && s->state == S_CLOSING) s->state = S_ESTABLISHED;
            tcp_output(s);
            continue;
        }
        if (s->ka_on && s->snd_una == s->snd_max && s->state != S_SYN_SENT && s->state != S_SYN_RCVD) {
            uint32_t idle = now - s->last_rx_ms;
            if (!s->ka_n ? idle >= s->ka_idle * 1000 : sd(now, s->ka_at) >= 0) {
                if (s->ka_n >= (int)s->ka_cnt) {
                    tcp_segment(s, s->snd_nxt, TCP_RST, 0);
                    tcp_kill(s, ETIMEDOUT);
                    continue;
                }
                tcp_segment(s, s->snd_nxt - 1, TCP_ACK, 0);
                s->ka_n++;
                s->ka_at = now + s->ka_intvl * 1000;
            }
        }
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

static uint32_t smask(sock_t* s) {
    return (uint32_t)sock_readable(s) | (uint32_t)sock_writable(s) << 1 | (uint32_t)sock_hup(s) << 2 |
           (uint32_t)s->state << 4 | (s->err != 0) << 8;
}

static void pump(void) {
    uint32_t f = irq_save();
    net_poll();
    sock_tick();
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t* s = &socks[i];
        if (!s->used || !s->wq.head) continue;
        uint32_t m = smask(s);
        if (m != s->rdy) { s->rdy = m; wq_wake(&s->wq); }
    }
    irq_restore(f);
}

void sock_pump(void) { pump(); }

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

/* sleep until the socket changes, 0 = recheck, -EINTR, -EAGAIN when tmo ms are over */
static int wstep(sock_t* s, wq_w_t** wp, uint32_t* t0, uint32_t tmo) {
    if (proc_interrupted()) return -EINTR;
    uint32_t el = pit_uptime_ms() - *t0;
    if (tmo && el >= tmo) return -EAGAIN;
    if (!*wp) {
        *wp = wq_waiter();
        if (*wp) {
            if (sock_wq_add(s, *wp)) return 0;
            wq_waiter_free(*wp);
            *wp = NULL;
        }
    }
    if (*wp) wq_sleep(*wp, tmo ? tmo - el : 0);
    else { task_yield(); pump(); }
    return 0;
}

static void wfree(wq_w_t** w) { if (*w) wq_waiter_free(*w); }

/* Wait for `cond`; 0, -EAGAIN or -EINTR. Declares a var, once per scope. */
#define WAIT_FOR(cond, nonblock)                                   \
    wq_w_t* w_ __attribute__((cleanup(wfree))) = NULL;             \
    uint32_t t0_ = pit_uptime_ms();                                \
    pump();                                                        \
    while (!(cond)) {                                              \
        if (nonblock) return -EAGAIN;                              \
        int r_ = wstep(s, &w_, &t0_, s->rcvtmo);                   \
        if (r_) return r_;                                         \
    }

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
            tcp_segment(c, c->snd_nxt, TCP_RST, 0);
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
    pump();
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
    kfree(s->rx); kfree(s->tx);                       // 128k a head, listeners never carry data
    s->rx = s->tx = NULL;
    s->rx_cap = s->tx_cap = 0;
    s->backlog = backlog <= 0 ? 1 : backlog > ACCEPT_MAX ? ACCEPT_MAX : backlog;
    return 0;
}

sock_t* sock_accept(sock_t* s, bool nonblock, int* err, uint8_t* ip, uint16_t* port) {
    if (s->state != S_LISTEN) { *err = -EINVAL; return NULL; }
    wq_w_t* w_ __attribute__((cleanup(wfree))) = NULL;
    uint32_t t0_ = pit_uptime_ms();
    pump();
    while (!s->aq_n) {
        if (nonblock) { *err = -EAGAIN; return NULL; }
        if ((*err = wstep(s, &w_, &t0_, 0))) return NULL;
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
        s->snd_nxt = s->snd_max = s->iss + 1;
        s->tx_seq = s->iss + 1;
        s->ws_ok = s->ts_ok = s->sack_ok = true;
        s->rcv_ws = 7;
        if (is_lo(ip) && !s->want_rx && !s->want_tx) tcp_setbuf(s, 1 << 20, 1 << 20);
        s->state = S_SYN_SENT;
        s->err = 0;
        s->retries = 0;
        s->rtt_t0 = pit_uptime_ms();
        s->rto = 1000;
        s->timer_on = true;
        s->rto_at = s->rtt_t0 + 1000;
        tcp_segment(s, s->iss, TCP_SYN, 0);
        if (nonblock) return -EINPROGRESS;
    }
    wq_w_t* w_ __attribute__((cleanup(wfree))) = NULL;
    uint32_t t0_ = pit_uptime_ms();
    pump();
    while (s->state == S_SYN_SENT) {
        int r_ = wstep(s, &w_, &t0_, 0);
        if (r_) return r_;
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
    uint32_t done = 0, st_ = pit_uptime_ms();
    wq_w_t* sw_ __attribute__((cleanup(wfree))) = NULL;
    while (done < len) {
        if (s->err) { int e = s->err; s->err = 0; return done ? (int)done : -e; }
        if (s->shut_wr || (s->state != S_ESTABLISHED && s->state != S_CLOSE_WAIT)) {
            if (done) return (int)done;
            proc_t* me = proc_current();
            if (me) proc_send_signal(me, 13);          /* SIGPIPE */
            return -EPIPE;
        }
        uint32_t room = s->tx_cap - s->tx_len;
        if (!room) {
            if (nonblock) return done ? (int)done : -EAGAIN;
            int r_ = wstep(s, &sw_, &st_, 0);
            if (r_) return done ? (int)done : r_;
            continue;
        }
        uint32_t n = len - done < room ? len - done : room;
        uint32_t pos = (s->tx_head + s->tx_len) % s->tx_cap;
        uint32_t n1 = s->tx_cap - pos < n ? s->tx_cap - pos : n;
        memcpy(s->tx + pos, buf + done, n1);
        if (n > n1) memcpy(s->tx, buf + done + n1, n - n1);
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
    uint32_t n1 = s->rx_cap - s->rx_head < n ? s->rx_cap - s->rx_head : n;
    memcpy(buf, s->rx + s->rx_head, n1);
    if (n > n1) memcpy(buf + n1, s->rx, n - n1);
    if (!peek) {
        s->rx_head = (s->rx_head + n) % s->rx_cap;
        s->rx_count -= n;
        /* Window update once it opened by 2 segments: a sender will not
           fill a window smaller than its MSS and would sit in its persist
           timer (seconds) instead. */
        uint32_t f = irq_save();
        if (s->state != S_CLOSED && rx_window(s) >= s->adv_wnd + 2 * (uint32_t)s->mss)
            tcp_segment(s, s->snd_max, TCP_ACK, 0);
        irq_restore(f);
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
        pump();
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
    return s->tx_cap - s->tx_len > s->tx_cap / 4 || s->err || s->state == S_CLOSED;
}

bool sock_hup(sock_t* s) {
    return s->type == 1 && (s->state == S_CLOSED || (s->fin_rcvd && s->shut_wr));
}

void sock_rcvtmo(sock_t* s, uint32_t ms) { s->rcvtmo = ms; }

/* setsockopt bits that only matter for the icmp cmsgs */
void sock_opt(sock_t* s, int level, int name, int val) {
    if (s->type == 1 && (level == 6 || (level == 1 && (name == 9 || name == 7 || name == 8 || name == 32 || name == 33)))) {
        uint32_t f = irq_save();
        if (level == 1 && name == 9) s->ka_on = val != 0;
        else if (level == 1 && (name == 7 || name == 32)) {
            s->want_tx = buf_clamp((uint32_t)val * 2);
            tcp_setbuf(s, s->rx_cap, s->want_tx);
        } else if (level == 1) {
            s->want_rx = buf_clamp((uint32_t)val * 2);
            tcp_setbuf(s, s->want_rx, s->tx_cap);
        } else if (name == 1) { s->nodelay = val != 0; if (val) tcp_output(s); }
        else if (name == 3) { s->cork = val != 0; if (!val) tcp_output(s); }
        else if (name == 4 && val > 0) s->ka_idle = val;
        else if (name == 5 && val > 0) s->ka_intvl = val;
        else if (name == 6 && val > 0) s->ka_cnt = val;
        else if (name == 2 && val >= 64 && val < s->mss) s->mss = val;
        irq_restore(f);
        return;
    }
    if (level == 0 && name == 12) s->opts = val ? s->opts | 1 : s->opts & ~1;                 /* IP_RECVTTL */
    else if (level == 0 && name == 2) s->ttl = val;                                          /* IP_TTL */
    else if (level == 1 && name == 29) s->opts = val ? s->opts | 2 : s->opts & ~2;           /* SO_TIMESTAMP */
    else if (level == 1 && name == 35) s->opts = val ? s->opts | 4 : s->opts & ~4;           /* SO_TIMESTAMPNS */
    else if (level == 41 && name == 51) s->opts = val ? s->opts | 8 : s->opts & ~8;          /* IPV6_RECVHOPLIMIT */
    else if (level == 41 && (name == 16 || name == 52)) s->ttl = val;
}

int sock_setcc(sock_t* s, const char* name, int len) {
    if (s->type != 1) return -EOPNOTSUPP;
    if (len >= 5 && !memcmp(name, "cubic", 5)) s->cc = 1;
    else if (len >= 4 && !memcmp(name, "reno", 4)) s->cc = 0;
    else return -2;
    return 0;
}

/* getsockopt things that need the tcp state, 1 = handled */
int sock_getopt(sock_t* s, int level, int name, uint8_t* out, uint32_t* len) {
    if (s->type != 1 || (level != 6 && level != 1)) return 0;
    if (level == 1) {
        if (name != 7 && name != 8 && name != 9) return 0;
        uint32_t v = name == 9 ? s->ka_on : name == 7 ? (s->tx_cap ? s->tx_cap : s->want_tx ? s->want_tx : BUF_DEF)
                                                   : (s->rx_cap ? s->rx_cap : s->want_rx ? s->want_rx : BUF_DEF);
        if (*len >= 4) { *(uint32_t*)out = v; *len = 4; }
        return 1;
    }
    if (name == 13) {                                  /* TCP_CONGESTION */
        const char* n = s->cc ? "cubic" : "reno";
        uint32_t l = s->cc ? 6 : 5;
        if (*len < l) l = *len;
        memcpy(out, n, l);
        *len = l;
        return 1;
    }
    if (name == 11) {                                  /* TCP_INFO */
        uint32_t i[26];
        uint32_t now = pit_uptime_ms();
        uint8_t* b = (uint8_t*)i;
        memset(i, 0, sizeof(i));
        static const uint8_t st[] = { 7, 10, 2, 3, 1, 4, 5, 11, 6, 8, 9 };    /* S_* to TCP_* */
        b[0] = s->state < 11 ? st[s->state] : 0;
        b[1] = s->rec == 1 ? 3 : s->rec == 2 ? 4 : 0;
        b[2] = s->retries;
        b[5] = (s->ts_ok ? 1 : 0) | (s->sack_ok ? 2 : 0) | (s->ws_ok ? 4 : 0);
        b[6] = (s->snd_ws & 15) | (s->rcv_ws & 15) << 4;
        i[2] = s->rto * 1000;
        i[3] = 40000;
        i[4] = s->mss;
        i[5] = s->mss;
        i[6] = (s->snd_max - s->snd_una) / (s->mss ? s->mss : 1);
        i[7] = s->sb_n ? sack_in(s, s->snd_una, s->snd_max) / (s->mss ? s->mss : 1) : 0;
        i[9] = s->n_retrans ? 1 : 0;
        i[11] = s->last_data_ms ? now - s->last_data_ms : 0;
        i[12] = s->last_ack_ms ? now - s->last_ack_ms : 0;
        i[14] = s->last_rx_ms ? now - s->last_rx_ms : 0;
        i[15] = 1500;
        i[16] = s->rx_cap;
        i[17] = s->srtt * 1000;
        i[18] = s->rttvar * 1000;
        i[19] = s->ssthresh / (s->mss ? s->mss : 1);
        i[20] = s->cwnd / (s->mss ? s->mss : 1);
        i[21] = s->mss;
        i[22] = 3;
        i[25] = s->n_retrans;
        uint32_t l = *len < sizeof(i) ? *len : sizeof(i);
        memcpy(out, i, l);
        *len = l;
        return 1;
    }
    if (name == 1 || name == 3 || name == 2 || name == 4 || name == 5 || name == 6) {
        uint32_t v = name == 1 ? s->nodelay : name == 3 ? s->cork : name == 2 ? s->mss : name == 4 ? s->ka_idle
                   : name == 5 ? s->ka_intvl : s->ka_cnt;
        if (*len >= 4) { *(uint32_t*)out = v; *len = 4; }
        return 1;
    }
    return 0;
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

wq_ent_t* sock_wq_cb(sock_t* s, void (*cb)(void*), void* arg) {
    bool first = !s->wq.head;
    wq_ent_t* e = wq_add_cb(&s->wq, cb, arg);
    uint32_t m = smask(s);
    if (e && m != s->rdy) { s->rdy = m; if (!first) wq_wake(&s->wq); }
    return e;
}

/* queue a waiter; the first one sets the baseline, later ones wake the others if something changed meanwhile */
wq_ent_t* sock_wq_add(sock_t* s, wq_w_t* w) {
    bool first = !s->wq.head;
    wq_ent_t* e = wq_add(&s->wq, w);
    uint32_t m = smask(s);
    if (e && m != s->rdy) { s->rdy = m; if (!first) wq_wake(&s->wq); }
    return e;
}
