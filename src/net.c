#include "net.h"
#include "rtl8139.h"
#include "string.h"
#include "heap.h"
#include "io.h"
#include "pit.h"

/* === Small IP/ARP/ICMP/TCP-client stack over RTL8139. ===
   No fragmentation, no retransmits, single TCP connection at a time.
   Fixed local IP, no DHCP, no DNS. */

#define ETH_HDR     14
#define MTU         1500
#define PKT_BUF     1600

#define ET_IPV4     0x0800
#define ET_ARP      0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6

#define ARP_REQ     1
#define ARP_REPLY   2

#define ICMP_ECHO_REQ   8
#define ICMP_ECHO_REPLY 0

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

static bool      g_ready = false;
static char      g_status[64] = "net: down";
static uint32_t  g_ip = IP4(NET_IP_A, NET_IP_B, NET_IP_C, NET_IP_D);
static uint32_t  g_gw = IP4(NET_IP_A, NET_IP_B, NET_IP_C, NET_GW_D);
static uint32_t  g_mask = 0xFFFFFF00u;
static uint8_t   g_gw_mac[6];
static bool      g_gw_known = false;

static void set_status(const char* s) {
    int i; for (i = 0; i < 63 && s[i]; i++) g_status[i] = s[i];
    g_status[i] = 0;
}

const char* net_status(void) { return g_status; }
bool        net_ready(void)  { return g_ready; }
const uint8_t* net_mac(void) { return rtl8139_mac(); }
uint32_t    net_ip(void)     { return g_ip; }
uint32_t    net_gw(void)     { return g_gw; }

/* === byte order helpers === */
static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint32_t htonl(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000u) | (v << 24);
}
#define ntohs(x) htons(x)
#define ntohl(x) htonl(x)

static uint16_t cksum_partial(uint32_t sum, const void* data, int len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 1) { sum += ((uint16_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += ((uint16_t)p[0]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}
static uint16_t cksum(const void* data, int len) {
    return ~cksum_partial(0, data, len);
}

/* === ethernet framing === */
typedef struct __attribute__((packed)) {
    uint8_t  dst[6], src[6];
    uint16_t type;       /* big endian */
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} arp_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  vihl;     /* version<<4|ihl */
    uint8_t  tos;
    uint16_t total;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t check;
    uint32_t src, dst;
} ip4_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, code;
    uint16_t check;
    uint16_t id, seq;
} icmp_echo_t;

typedef struct __attribute__((packed)) {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  data_off;     /* (data offset in 32-bit words) << 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t check;
    uint16_t urg;
} tcp_hdr_t;

/* === ARP cache: one entry (gateway). === */
static uint8_t g_arp_cache_mac[6];
static uint32_t g_arp_cache_ip = 0;
static bool     g_arp_cache_v = false;

static void send_eth(uint16_t type, const uint8_t* dst_mac, const void* payload, int len) {
    uint8_t frame[PKT_BUF];
    if (len + ETH_HDR > PKT_BUF) return;
    eth_hdr_t* eh = (eth_hdr_t*)frame;
    for (int i = 0; i < 6; i++) eh->dst[i] = dst_mac[i];
    const uint8_t* src = rtl8139_mac();
    for (int i = 0; i < 6; i++) eh->src[i] = src[i];
    eh->type = htons(type);
    memcpy(frame + ETH_HDR, payload, len);
    rtl8139_send(frame, ETH_HDR + len);
}

static void send_arp_request(uint32_t target_ip) {
    arp_hdr_t a;
    a.htype = htons(1);
    a.ptype = htons(ET_IPV4);
    a.hlen = 6; a.plen = 4;
    a.op = htons(ARP_REQ);
    const uint8_t* mac = rtl8139_mac();
    for (int i = 0; i < 6; i++) a.sha[i] = mac[i];
    a.spa = htonl(g_ip);
    for (int i = 0; i < 6; i++) a.tha[i] = 0;
    a.tpa = htonl(target_ip);
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    send_eth(ET_ARP, bcast, &a, sizeof(a));
}

static void send_arp_reply(const uint8_t* dst_mac, uint32_t dst_ip) {
    arp_hdr_t a;
    a.htype = htons(1);
    a.ptype = htons(ET_IPV4);
    a.hlen = 6; a.plen = 4;
    a.op = htons(ARP_REPLY);
    const uint8_t* mac = rtl8139_mac();
    for (int i = 0; i < 6; i++) a.sha[i] = mac[i];
    a.spa = htonl(g_ip);
    for (int i = 0; i < 6; i++) a.tha[i] = dst_mac[i];
    a.tpa = htonl(dst_ip);
    send_eth(ET_ARP, dst_mac, &a, sizeof(a));
}

/* Resolve next-hop MAC. If `ip` is off-net, resolves gateway. */
static bool resolve_mac(uint32_t ip, uint8_t* out_mac, uint32_t timeout_ms) {
    uint32_t want = ((ip & g_mask) == (g_ip & g_mask)) ? ip : g_gw;
    if (g_arp_cache_v && g_arp_cache_ip == want) {
        for (int i = 0; i < 6; i++) out_mac[i] = g_arp_cache_mac[i];
        return true;
    }
    uint32_t t0 = pit_uptime_ms();
    for (int attempt = 0; attempt < 4; attempt++) {
        send_arp_request(want);
        uint32_t deadline = pit_uptime_ms() + 250;
        while (pit_uptime_ms() < deadline) {
            net_poll();
            if (g_arp_cache_v && g_arp_cache_ip == want) {
                for (int i = 0; i < 6; i++) out_mac[i] = g_arp_cache_mac[i];
                return true;
            }
        }
        if (pit_uptime_ms() - t0 > timeout_ms) break;
    }
    return false;
}

/* === ICMP echo state === */
static volatile bool     g_ping_got = false;
static volatile uint16_t g_ping_id = 0;
static volatile uint16_t g_ping_seq = 0;

/* === TCP state machine (single connection) === */
typedef enum {
    TCPS_CLOSED, TCPS_SYN_SENT, TCPS_ESTABLISHED, TCPS_FIN_WAIT, TCPS_TIME_WAIT
} tcp_state_t;

static struct {
    tcp_state_t state;
    uint32_t    peer_ip;
    uint16_t    peer_port;
    uint16_t    local_port;
    uint32_t    snd_nxt;        /* next sequence we'll send */
    uint32_t    rcv_nxt;        /* what we expect from peer */
    uint8_t     peer_mac[6];

    /* receive buffer */
    uint8_t     rxbuf[16384];
    int         rxlen;
    bool        peer_closed;
} tcp;

static void send_ip(uint32_t dst, uint8_t proto, const void* payload, int len) {
    uint8_t frame[PKT_BUF];
    int total = sizeof(ip4_hdr_t) + len;
    if (total > MTU) return;

    ip4_hdr_t* ih = (ip4_hdr_t*)frame;
    ih->vihl = 0x45;
    ih->tos = 0;
    ih->total = htons((uint16_t)total);
    static uint16_t ipid = 1;
    ih->id = htons(ipid++);
    ih->flags_frag = 0;
    ih->ttl = 64;
    ih->proto = proto;
    ih->check = 0;
    ih->src = htonl(g_ip);
    ih->dst = htonl(dst);
    ih->check = htons(cksum(ih, sizeof(*ih)));

    memcpy(frame + sizeof(ip4_hdr_t), payload, len);

    uint8_t mac[6];
    if (!resolve_mac(dst, mac, 1000)) return;
    send_eth(ET_IPV4, mac, frame, total);
}

static void send_tcp_segment(uint8_t flags, const void* data, int data_len) {
    uint8_t buf[PKT_BUF];
    tcp_hdr_t* th = (tcp_hdr_t*)buf;
    th->sport = htons(tcp.local_port);
    th->dport = htons(tcp.peer_port);
    th->seq = htonl(tcp.snd_nxt);
    th->ack = htonl(tcp.rcv_nxt);
    th->data_off = (5 << 4);
    th->flags = flags;
    th->window = htons(8192);
    th->check = 0;
    th->urg = 0;
    if (data_len) memcpy(buf + sizeof(tcp_hdr_t), data, data_len);

    /* pseudo header for checksum */
    int tcp_len = sizeof(tcp_hdr_t) + data_len;
    uint8_t pseudo[12];
    uint32_t src = htonl(g_ip), dst = htonl(tcp.peer_ip);
    memcpy(pseudo + 0, &src, 4);
    memcpy(pseudo + 4, &dst, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10] = (tcp_len >> 8) & 0xFF;
    pseudo[11] = tcp_len & 0xFF;

    uint32_t sum = 0;
    sum = cksum_partial(sum, pseudo, 12);
    /* segment may have odd length; cksum_partial handles that. */
    sum = cksum_partial(sum, buf, tcp_len);
    th->check = htons((uint16_t)~sum);

    /* advance snd_nxt by data + SYN/FIN consumption */
    uint32_t consume = data_len;
    if (flags & TCP_SYN) consume++;
    if (flags & TCP_FIN) consume++;
    tcp.snd_nxt += consume;

    send_ip(tcp.peer_ip, IP_PROTO_TCP, buf, tcp_len);
}

/* === dispatch === */

static void on_arp(const uint8_t* pkt, int len) {
    if (len < (int)sizeof(arp_hdr_t)) return;
    const arp_hdr_t* a = (const arp_hdr_t*)pkt;
    if (ntohs(a->ptype) != ET_IPV4 || a->plen != 4) return;
    uint16_t op = ntohs(a->op);
    uint32_t spa = ntohl(a->spa);
    uint32_t tpa = ntohl(a->tpa);
    if (op == ARP_REQ && tpa == g_ip) {
        send_arp_reply(a->sha, spa);
    } else if (op == ARP_REPLY) {
        g_arp_cache_v = true;
        g_arp_cache_ip = spa;
        for (int i = 0; i < 6; i++) g_arp_cache_mac[i] = a->sha[i];
    }
}

static void on_icmp(const ip4_hdr_t* ih, const uint8_t* pkt, int len) {
    if (len < (int)sizeof(icmp_echo_t)) return;
    const icmp_echo_t* e = (const icmp_echo_t*)pkt;
    if (e->type == ICMP_ECHO_REQ) {
        /* reply */
        uint8_t buf[PKT_BUF];
        if (len > PKT_BUF) return;
        memcpy(buf, pkt, len);
        icmp_echo_t* er = (icmp_echo_t*)buf;
        er->type = ICMP_ECHO_REPLY;
        er->check = 0;
        er->check = htons(cksum(buf, len));
        send_ip(ntohl(ih->src), IP_PROTO_ICMP, buf, len);
    } else if (e->type == ICMP_ECHO_REPLY) {
        if (ntohs(e->id) == g_ping_id && ntohs(e->seq) == g_ping_seq) {
            g_ping_got = true;
        }
    }
}

static void on_tcp(const ip4_hdr_t* ih, const uint8_t* pkt, int len) {
    if (len < (int)sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t* th = (const tcp_hdr_t*)pkt;
    uint16_t sport = ntohs(th->sport);
    uint16_t dport = ntohs(th->dport);
    if (dport != tcp.local_port) return;
    if (sport != tcp.peer_port) return;
    if (ntohl(ih->src) != tcp.peer_ip) return;

    int hl = (th->data_off >> 4) * 4;
    int data_len = len - hl;
    const uint8_t* data = pkt + hl;
    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);
    uint8_t  flags = th->flags;

    if (flags & TCP_RST) {
        tcp.state = TCPS_CLOSED;
        return;
    }

    if (tcp.state == TCPS_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == tcp.snd_nxt) {
            tcp.rcv_nxt = seq + 1;
            tcp.state = TCPS_ESTABLISHED;
            send_tcp_segment(TCP_ACK, 0, 0);
        }
        return;
    }

    if (tcp.state == TCPS_ESTABLISHED || tcp.state == TCPS_FIN_WAIT) {
        if (data_len > 0 && seq == tcp.rcv_nxt) {
            int can = (int)sizeof(tcp.rxbuf) - tcp.rxlen;
            if (can > data_len) can = data_len;
            if (can > 0) {
                memcpy(tcp.rxbuf + tcp.rxlen, data, can);
                tcp.rxlen += can;
            }
            tcp.rcv_nxt += data_len;
            send_tcp_segment(TCP_ACK, 0, 0);
        } else if (data_len > 0) {
            /* out-of-order: send dup-ack */
            send_tcp_segment(TCP_ACK, 0, 0);
        }
        if (flags & TCP_FIN) {
            tcp.rcv_nxt++;
            tcp.peer_closed = true;
            send_tcp_segment(TCP_ACK, 0, 0);
            if (tcp.state == TCPS_FIN_WAIT) {
                tcp.state = TCPS_TIME_WAIT;
            }
        }
    }
}

static void on_ipv4(const uint8_t* pkt, int len) {
    if (len < (int)sizeof(ip4_hdr_t)) return;
    const ip4_hdr_t* ih = (const ip4_hdr_t*)pkt;
    int ihl = (ih->vihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    uint32_t dst = ntohl(ih->dst);
    if (dst != g_ip && dst != 0xFFFFFFFFu) return;
    int total = ntohs(ih->total);
    if (total > len) total = len;
    const uint8_t* payload = pkt + ihl;
    int payload_len = total - ihl;
    switch (ih->proto) {
        case IP_PROTO_ICMP: on_icmp(ih, payload, payload_len); break;
        case IP_PROTO_TCP:  on_tcp (ih, payload, payload_len); break;
        default: break;
    }
}

void net_poll(void) {
    if (!g_ready) return;
    uint8_t buf[PKT_BUF];
    for (int i = 0; i < 32; i++) {
        int n = rtl8139_recv(buf, sizeof(buf));
        if (n <= 0) return;
        if (n < ETH_HDR) continue;
        const eth_hdr_t* eh = (const eth_hdr_t*)buf;
        uint16_t type = ntohs(eh->type);
        if (type == ET_ARP) on_arp(buf + ETH_HDR, n - ETH_HDR);
        else if (type == ET_IPV4) on_ipv4(buf + ETH_HDR, n - ETH_HDR);
    }
}

int net_init(void) {
    if (g_ready) return 0;
    if (rtl8139_init() != 0) {
        set_status(rtl8139_status());
        return -1;
    }
    g_ready = true;
    set_status("net: up (10.0.2.15/24 gw 10.0.2.2)");

    /* warm ARP cache for gateway so first packet doesn't stall */
    if (resolve_mac(g_gw, g_gw_mac, 1500)) g_gw_known = true;
    return 0;
}

/* === ping === */
int net_ping(uint32_t ip, uint32_t timeout_ms) {
    if (!g_ready) return -1;
    static uint16_t seqn = 1;
    g_ping_id  = 0x4242;
    g_ping_seq = seqn++;
    g_ping_got = false;

    uint8_t payload[8 + 32];
    icmp_echo_t* e = (icmp_echo_t*)payload;
    e->type = ICMP_ECHO_REQ;
    e->code = 0;
    e->check = 0;
    e->id = htons(g_ping_id);
    e->seq = htons(g_ping_seq);
    for (int i = 0; i < 32; i++) payload[8 + i] = 'A' + (i & 31);
    int plen = 8 + 32;
    e->check = htons(cksum(payload, plen));

    uint32_t t0 = pit_uptime_ms();
    send_ip(ip, IP_PROTO_ICMP, payload, plen);
    uint32_t deadline = t0 + timeout_ms;
    while (pit_uptime_ms() < deadline) {
        net_poll();
        if (g_ping_got) return (int)(pit_uptime_ms() - t0);
    }
    return -1;
}

/* === tcp public-ish wrappers === */
static int tcp_open(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    tcp.state = TCPS_CLOSED;
    tcp.peer_ip = ip;
    tcp.peer_port = port;
    static uint16_t lp = 32768;
    tcp.local_port = lp++;
    tcp.rxlen = 0;
    tcp.peer_closed = false;
    /* pseudo-random ISN */
    tcp.snd_nxt = 0x10000000u + pit_uptime_ms();
    tcp.rcv_nxt = 0;

    if (!resolve_mac(ip, tcp.peer_mac, 1500)) return -1;

    tcp.state = TCPS_SYN_SENT;
    send_tcp_segment(TCP_SYN, 0, 0);

    uint32_t deadline = pit_uptime_ms() + timeout_ms;
    while (pit_uptime_ms() < deadline) {
        net_poll();
        if (tcp.state == TCPS_ESTABLISHED) return 0;
        if (tcp.state == TCPS_CLOSED) return -2;
    }
    return -3;
}

static int tcp_send_all(const void* data, int len) {
    if (tcp.state != TCPS_ESTABLISHED) return -1;
    const uint8_t* p = (const uint8_t*)data;
    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1400) chunk = 1400;
        send_tcp_segment(TCP_ACK | TCP_PSH, p + sent, chunk);
        sent += chunk;
    }
    return sent;
}

static int tcp_recv_until_close(uint8_t* out, int max, uint32_t timeout_ms) {
    int total = 0;
    uint32_t last_progress = pit_uptime_ms();
    while (pit_uptime_ms() - last_progress < timeout_ms) {
        net_poll();
        if (tcp.rxlen > 0) {
            int n = tcp.rxlen;
            if (total + n > max) n = max - total;
            if (n > 0) {
                memcpy(out + total, tcp.rxbuf, n);
                total += n;
            }
            /* shift remaining left (usually rxlen <= 16K so cheap) */
            if (tcp.rxlen > n) {
                memmove(tcp.rxbuf, tcp.rxbuf + n, tcp.rxlen - n);
            }
            tcp.rxlen -= n;
            last_progress = pit_uptime_ms();
        }
        if (tcp.peer_closed && tcp.rxlen == 0) break;
        if (total >= max) break;
    }
    return total;
}

static void tcp_close_active(void) {
    if (tcp.state == TCPS_ESTABLISHED) {
        send_tcp_segment(TCP_ACK | TCP_FIN, 0, 0);
        tcp.state = TCPS_FIN_WAIT;
        uint32_t deadline = pit_uptime_ms() + 1000;
        while (pit_uptime_ms() < deadline && tcp.state != TCPS_CLOSED &&
               tcp.state != TCPS_TIME_WAIT) {
            net_poll();
        }
    }
    tcp.state = TCPS_CLOSED;
}

int net_http_get(uint32_t ip, uint16_t port, const char* host, const char* path,
                 uint8_t* out, int outcap) {
    if (!g_ready) return -1;
    if (tcp_open(ip, port, 4000) != 0) return -2;

    char req[512];
    int n = 0;
    const char* g = "GET ";
    while (*g && n < (int)sizeof(req)) req[n++] = *g++;
    const char* p = path ? path : "/";
    while (*p && n < (int)sizeof(req)) req[n++] = *p++;
    const char* httpv = " HTTP/1.0\r\n";
    for (const char* q = httpv; *q && n < (int)sizeof(req); q++) req[n++] = *q;
    if (host) {
        const char* hh = "Host: ";
        for (const char* q = hh; *q && n < (int)sizeof(req); q++) req[n++] = *q;
        for (const char* q = host; *q && n < (int)sizeof(req); q++) req[n++] = *q;
        const char* cr = "\r\n";
        for (const char* q = cr; *q && n < (int)sizeof(req); q++) req[n++] = *q;
    }
    const char* ua = "User-Agent: samaraos/1\r\nConnection: close\r\n\r\n";
    for (const char* q = ua; *q && n < (int)sizeof(req); q++) req[n++] = *q;

    if (tcp_send_all(req, n) < 0) { tcp_close_active(); return -3; }
    int got = tcp_recv_until_close(out, outcap, 6000);
    tcp_close_active();
    return got;
}
