#include "net/net.h"
#include "drivers/rtl8139.h"
#include "drivers/virtio.h"
#include "drivers/e1000.h"
#include "fs/fs.h"
#include "core/string.h"
#include "core/heap.h"
#include "core/io.h"
#include "boot/pit.h"
#include "core/task.h"
#include "net/sock.h"
#include "core/smp.h"

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
#define IP_PROTO_UDP  17

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
static uint8_t   g_gw_mac[6];
static bool      g_gw_known = false;

/* Network cards. eth0 is 10.0.2.15/24 (QEMU's default SLIRP net), ethN
   gets 10.0.(2+N).15 with the gateway at .2, which is what
   "-netdev user,net=10.0.(2+N).0/24" hands out. Packets leave through
   the card whose subnet has the address, else through eth0. */
#define NIF_MAX 6
enum { NIC_RTL, NIC_VIRTIO, NIC_E1000 };
typedef struct { int kind, dev; uint8_t mac[6]; uint32_t ip, mask, gw, rx, tx; char name[8]; } nif_t;
static nif_t nifs[NIF_MAX];
static int   n_nifs;

static int nic_send(nif_t* f, const void* d, int n) {
    f->tx++;
    if (f->kind == NIC_E1000) return e1000_send(d, n);
    return f->kind == NIC_RTL ? rtl8139_send(d, n) : vnet_send(f->dev, d, n);
}
static int nic_recv(nif_t* f, void* d, int max) {
    int n = f->kind == NIC_E1000 ? e1000_recv(d, max) : f->kind == NIC_RTL ? rtl8139_recv(d, max) : vnet_recv(f->dev, d, max);
    if (n > 0) f->rx++;
    return n;
}

/* the card for dst */
static nif_t* route(uint32_t dst) {
    for (int i = 0; i < n_nifs; i++) if ((dst & nifs[i].mask) == (nifs[i].ip & nifs[i].mask)) return &nifs[i];
    return n_nifs ? &nifs[0] : NULL;
}
static nif_t* nif_by_ip(uint32_t ip) {
    for (int i = 0; i < n_nifs; i++) if (nifs[i].ip == ip) return &nifs[i];
    return NULL;
}

int net_ifcount(void) { return n_nifs; }
bool net_ifinfo(int i, const char** name, const uint8_t** mac, uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* rx, uint32_t* tx) {
    if (i < 0 || i >= n_nifs) return false;
    nif_t* f = &nifs[i];
    *name = f->name; *mac = f->mac; *ip = f->ip; *mask = f->mask; *gw = f->gw; *rx = f->rx; *tx = f->tx;
    return true;
}

static void set_status(const char* s) {
    int i; for (i = 0; i < 63 && s[i]; i++) g_status[i] = s[i];
    g_status[i] = 0;
}

const char* net_status(void) { return g_status; }
bool        net_ready(void)  { return g_ready; }
const uint8_t* net_mac(void) { static const uint8_t z[6]; return n_nifs ? nifs[0].mac : z; }
uint32_t    net_ip(void)     { return n_nifs ? nifs[0].ip : 0; }
uint32_t    net_gw(void)     { return n_nifs ? nifs[0].gw : 0; }

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

/* === ARP cache: a few entries (gateway, SLIRP DNS, neighbours). === */
#define ARP_SLOTS 8
static struct { uint32_t ip; uint8_t mac[6]; bool v; } g_arp[ARP_SLOTS];
static int g_arp_next;

static bool arp_lookup(uint32_t ip, uint8_t* mac) {
    for (int i = 0; i < ARP_SLOTS; i++)
        if (g_arp[i].v && g_arp[i].ip == ip) { memcpy(mac, g_arp[i].mac, 6); return true; }
    return false;
}

static void arp_store(uint32_t ip, const uint8_t* mac) {
    int slot = -1;
    for (int i = 0; i < ARP_SLOTS; i++) if (g_arp[i].v && g_arp[i].ip == ip) slot = i;
    if (slot < 0) { slot = g_arp_next; g_arp_next = (g_arp_next + 1) % ARP_SLOTS; }
    g_arp[slot].ip = ip;
    memcpy(g_arp[slot].mac, mac, 6);
    g_arp[slot].v = true;
}

static void send_eth(nif_t* f, uint16_t type, const uint8_t* dst_mac, const void* payload, int len) {
    uint8_t frame[PKT_BUF];
    if (len + ETH_HDR > PKT_BUF || !g_ready || !f) return;
    eth_hdr_t* eh = (eth_hdr_t*)frame;
    for (int i = 0; i < 6; i++) eh->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eh->src[i] = f->mac[i];
    eh->type = htons(type);
    memcpy(frame + ETH_HDR, payload, len);
    nic_send(f, frame, ETH_HDR + len);
}

int net_eth_send(int ifi, const uint8_t* dmac, uint16_t type, const void* pl, int len) {
    if (ifi < 0 || ifi >= n_nifs) return -1;
    send_eth(&nifs[ifi], type, dmac, pl, len);
    return 0;
}

static void send_arp_request(nif_t* f, uint32_t target_ip) {
    arp_hdr_t a;
    a.htype = htons(1);
    a.ptype = htons(ET_IPV4);
    a.hlen = 6; a.plen = 4;
    a.op = htons(ARP_REQ);
    for (int i = 0; i < 6; i++) a.sha[i] = f->mac[i];
    a.spa = htonl(f->ip);
    for (int i = 0; i < 6; i++) a.tha[i] = 0;
    a.tpa = htonl(target_ip);
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    send_eth(f, ET_ARP, bcast, &a, sizeof(a));
}

static void send_arp_reply(nif_t* f, const uint8_t* dst_mac, uint32_t dst_ip) {
    arp_hdr_t a;
    a.htype = htons(1);
    a.ptype = htons(ET_IPV4);
    a.hlen = 6; a.plen = 4;
    a.op = htons(ARP_REPLY);
    for (int i = 0; i < 6; i++) a.sha[i] = f->mac[i];
    a.spa = htonl(f->ip);
    for (int i = 0; i < 6; i++) a.tha[i] = dst_mac[i];
    a.tpa = htonl(dst_ip);
    send_eth(f, ET_ARP, dst_mac, &a, sizeof(a));
}

/* Resolve next-hop MAC. If `ip` is off-net, resolves gateway. */
static bool resolve_mac(uint32_t ip, uint8_t* out_mac, uint32_t timeout_ms) {
    nif_t* f = route(ip);
    if (!f) return false;
    uint32_t want = ((ip & f->mask) == (f->ip & f->mask)) ? ip : f->gw;
    if (arp_lookup(want, out_mac)) return true;
    if (!g_ready) return false;
    uint32_t t0 = pit_uptime_ms();
    for (int attempt = 0; attempt < 4; attempt++) {
        send_arp_request(f, want);
        uint32_t deadline = pit_uptime_ms() + 250;
        while ((int32_t)(pit_uptime_ms() - deadline) < 0) {
            net_poll();
            if (arp_lookup(want, out_mac)) return true;
            /* Yield instead of spinning: may run inside a syscall with IRQs
               masked, where only a yield/hlt lets the clock advance. */
            task_yield();
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

/* === loopback: packets to 127/8 or to ourselves are queued and fed back
   into on_ipv4 by net_poll (never recursively from the send path). === */
#define LOOP_SLOTS 1024
static uint8_t* g_loop[LOOP_SLOTS];
static int      g_loop_len[LOOP_SLOTS];
static int      g_loop_head, g_loop_tail;
static uint32_t g_loop_bytes;
static uint8_t  g_loop_big[LOOP_SLOTS];

static bool is_local(uint32_t ip) { return (ip >> 24) == 127 || nif_by_ip(ip) != NULL; }

static void on_ipv4(const uint8_t* pkt, int len);

uint8_t net_ttl;

/* kmalloc of 16k a packet was slow with all the other stuff in the heap: keep some around */
#define BIGB 16448
static uint8_t* bpool[2][64];
static int bpn[2];
static spin_t bpl;

uint8_t* net_buf_get(int big) {
    uint64_t f = spin_lock(&bpl);
    uint8_t* p = bpn[big] ? bpool[big][--bpn[big]] : NULL;
    spin_unlock(&bpl, f);
    return p ? p : (uint8_t*)kmalloc(big ? BIGB : PKT_BUF);
}

void net_buf_put(uint8_t* p, int big) {
    uint64_t f = spin_lock(&bpl);
    if (bpn[big] < 64) { bpool[big][bpn[big]++] = p; p = NULL; }
    spin_unlock(&bpl, f);
    if (p) kfree(p);
}

/* loopback has no mtu to speak of: one malloc per packet, handed to on_ipv4 as is */
static void loop_pkt(uint32_t dst, uint8_t proto, const void* payload, int len) {
    int total = 20 + len;
    uint32_t irq = irq_save();
    int next = (g_loop_head + 1) % LOOP_SLOTS;
    if (total > BIGB || next == g_loop_tail || g_loop_bytes > (8u << 20)) { irq_restore(irq); return; }
    int big = total > PKT_BUF;
    uint8_t* p = net_buf_get(big);
    if (!p) { irq_restore(irq); return; }
    ip4_hdr_t* ih = (ip4_hdr_t*)p;
    static uint16_t lid = 1;
    ih->vihl = 0x45;
    ih->tos = 0;
    ih->total = htons((uint16_t)total);
    ih->id = htons(lid++);
    ih->flags_frag = 0;
    ih->ttl = net_ttl ? net_ttl : 64;
    ih->proto = proto;
    ih->check = 0;
    ih->src = htonl(dst);
    ih->dst = htonl(dst);
    ih->check = htons(cksum(ih, sizeof(*ih)));
    memcpy(p + 20, payload, len);
    g_loop[g_loop_head] = p;
    g_loop_big[g_loop_head] = big;
    g_loop_len[g_loop_head] = total;
    g_loop_bytes += total;
    g_loop_head = next;
    irq_restore(irq);
}

static void send_ip(uint32_t dst, uint8_t proto, const void* payload, int len) {
    uint8_t frame[PKT_BUF];
    int total = sizeof(ip4_hdr_t) + len;
    if (is_local(dst)) { loop_pkt(dst, proto, payload, len); return; }
    if (total > MTU) return;
    uint32_t irq = irq_save();
    nif_t* out = route(dst);
    uint32_t src = (dst >> 24) == 127 ? dst : nif_by_ip(dst) ? dst : out ? out->ip : 0;

    ip4_hdr_t* ih = (ip4_hdr_t*)frame;
    ih->vihl = 0x45;
    ih->tos = 0;
    ih->total = htons((uint16_t)total);
    static uint16_t ipid = 1;
    ih->id = htons(ipid++);
    ih->flags_frag = 0;
    ih->ttl = net_ttl ? net_ttl : 64;
    ih->proto = proto;
    ih->check = 0;
    ih->src = htonl(src);
    ih->dst = htonl(dst);
    ih->check = htons(cksum(ih, sizeof(*ih)));

    memcpy(frame + sizeof(ip4_hdr_t), payload, len);

    uint8_t mac[6];
    if (resolve_mac(dst, mac, 1000)) send_eth(out, ET_IPV4, mac, frame, total);
    irq_restore(irq);
}

void net_send_ip(uint32_t dst, uint8_t proto, const void* payload, int len) {
    send_ip(dst, proto, payload, len);
}

uint32_t net_rxwnd(uint32_t dst) {
    if ((dst >> 24) == 127) return 1 << 20;
    nif_t* f = dst ? route(dst) : &nifs[0];
    return f && f->kind == NIC_E1000 ? 192 * 1024 : 48 * 1024;
}

uint32_t net_src_for(uint32_t dst) {
    if ((dst >> 24) == 127) return dst;
    nif_t* f = route(dst);
    return f ? f->ip : 0;
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
    uint32_t src = htonl(net_src_for(tcp.peer_ip)), dst = htonl(tcp.peer_ip);
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

static void on_arp(nif_t* f, const uint8_t* pkt, int len) {
    if (len < (int)sizeof(arp_hdr_t)) return;
    const arp_hdr_t* a = (const arp_hdr_t*)pkt;
    if (ntohs(a->ptype) != ET_IPV4 || a->plen != 4) return;
    uint16_t op = ntohs(a->op);
    uint32_t spa = ntohl(a->spa);
    uint32_t tpa = ntohl(a->tpa);
    if (op == ARP_REQ && tpa == f->ip) {
        send_arp_reply(f, a->sha, spa);
    } else if (op == ARP_REPLY) {
        arp_store(spa, a->sha);
    }
    if (op == ARP_REQ && tpa == f->ip) arp_store(spa, a->sha);
}

static void on_icmp(const ip4_hdr_t* ih, const uint8_t* pkt, int len) {
    if (len < (int)sizeof(icmp_echo_t)) return;
    sock_input_icmp(ntohl(ih->src), ntohl(ih->dst), (const uint8_t*)ih, (int)(pkt - (const uint8_t*)ih), (int)(pkt - (const uint8_t*)ih) + len);
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
    /* Not the kernel's own wget/browser connection: user sockets. */
    if (tcp.state == TCPS_CLOSED || dport != tcp.local_port || sport != tcp.peer_port ||
        ntohl(ih->src) != tcp.peer_ip) {
        sock_input_tcp(ntohl(ih->src), ntohl(ih->dst), pkt, len);
        return;
    }

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

/* dhcp client state, only alive while net_init asks */
static struct { nif_t* f; uint32_t xid, ip, mask, gw, dns, srv; int state; } dh;

static void dhcp_input(const uint8_t* u, int len) {
    const uint8_t* b = u + 8;
    if (len < 8 + 240 || b[0] != 2 || *(uint32_t*)(b + 4) != dh.xid) return;
    int type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, srv = 0;
    const uint8_t* o = b + 240;
    const uint8_t* end = u + len;
    while (o + 2 <= end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 + o[1] > end) break;
        uint32_t v = o[1] >= 4 ? ntohl(*(uint32_t*)(o + 2)) : 0;
        if (o[0] == 53) type = o[2];
        else if (o[0] == 1) mask = v;
        else if (o[0] == 3) gw = v;
        else if (o[0] == 6) dns = v;
        else if (o[0] == 54) srv = v;
        o += 2 + o[1];
    }
    uint32_t yi = ntohl(*(uint32_t*)(b + 16));
    if (type == 2 && dh.state == 1) { dh.ip = yi; dh.srv = srv; dh.state = 2; }
    else if (type == 5 && dh.state == 3) { dh.ip = yi; dh.mask = mask; dh.gw = gw; dh.dns = dns; dh.state = 4; }
    else if (type == 6) dh.state = -1;     /* NAK */
}

static void on_ipv4(const uint8_t* pkt, int len) {
    if (len < (int)sizeof(ip4_hdr_t)) return;
    const ip4_hdr_t* ih = (const ip4_hdr_t*)pkt;
    int ihl = (ih->vihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    /* udp to :68 while asking, before the dst check: we have no ip yet */
    if (dh.f && ih->proto == IP_PROTO_UDP && len >= ihl + 8 && pkt[ihl + 2] == 0 && pkt[ihl + 3] == 68) {
        int t = ntohs(ih->total);
        dhcp_input(pkt + ihl, (t > len ? len : t) - ihl);
        return;
    }
    uint32_t dst = ntohl(ih->dst);
    if (!nif_by_ip(dst) && dst != 0xFFFFFFFFu && (dst >> 24) != 127) return;
    int total = ntohs(ih->total);
    if (total > len) total = len;
    const uint8_t* payload = pkt + ihl;
    int payload_len = total - ihl;
    switch (ih->proto) {
        case IP_PROTO_ICMP: on_icmp(ih, payload, payload_len); break;
        case IP_PROTO_TCP:  on_tcp (ih, payload, payload_len); break;
        case IP_PROTO_UDP:  sock_input_udp(ntohl(ih->src), dst, payload, payload_len); break;
        default: break;
    }
}

void net_poll(void) {
    uint32_t irq = irq_save();
    uint8_t buf[PKT_BUF];                  /* on the stack: net_poll can nest via ARP */
    bool got = false;
    for (int i = 0; i < 256 && g_loop_tail != g_loop_head; i++) {
        int slot = g_loop_tail;
        got = true;
        g_loop_tail = (g_loop_tail + 1) % LOOP_SLOTS;
        uint8_t* p = g_loop[slot];
        int pl = g_loop_len[slot];
        g_loop[slot] = NULL;
        g_loop_bytes -= pl;
        on_ipv4(p, pl);
        net_buf_put(p, g_loop_big[slot]);
    }
    for (int k = 0; g_ready && k < n_nifs; k++)
        for (int i = 0; i < 32; i++) {
            int n = nic_recv(&nifs[k], buf, sizeof(buf));
            if (n <= 0) break;
            got = true;
            if (n < ETH_HDR) continue;
            const eth_hdr_t* eh = (const eth_hdr_t*)buf;
            uint16_t type = ntohs(eh->type);
            if (type == ET_ARP) on_arp(&nifs[k], buf + ETH_HDR, n - ETH_HDR);
            else if (type == ET_IPV4) on_ipv4(buf + ETH_HDR, n - ETH_HDR);
            else if (type == 0x86DD) ip6_input(k, buf + ETH_HDR, n - ETH_HDR, eh->src);
        }
    ip6_poll();
    if (got) io_wake();
    irq_restore(irq);
}

static void dhcp_send(nif_t* f, int type) {
    uint8_t pkt[20 + 8 + 300];
    memset(pkt, 0, sizeof(pkt));
    uint8_t* u = pkt + 20;
    uint8_t* b = u + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;
    *(uint32_t*)(b + 4) = dh.xid;
    b[10] = 0x80;                          /* broadcast reply pls, we have no ip */
    memcpy(b + 28, f->mac, 6);
    uint8_t* o = b + 236;
    *o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    if (type == 3) {
        *o++ = 50; *o++ = 4; *(uint32_t*)o = htonl(dh.ip); o += 4;
        *o++ = 54; *o++ = 4; *(uint32_t*)o = htonl(dh.srv); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 12; *o++ = 6; memcpy(o, "samara", 6); o += 6;
    *o = 255;
    *(uint16_t*)u = htons(68);
    *(uint16_t*)(u + 2) = htons(67);
    *(uint16_t*)(u + 4) = htons(8 + 300);  /* udp csum 0 = none, fine for v4 */
    ip4_hdr_t* ih = (ip4_hdr_t*)pkt;
    ih->vihl = 0x45;
    ih->total = htons(sizeof(pkt));
    ih->ttl = 64;
    ih->proto = IP_PROTO_UDP;
    ih->dst = 0xFFFFFFFFu;
    ih->check = htons(cksum(ih, 20));
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    send_eth(f, ET_IPV4, bcast, pkt, sizeof(pkt));
}

static bool dhcp_wait(int st, uint32_t ms) {
    uint32_t t0 = pit_uptime_ms();
    while (pit_uptime_ms() - t0 < ms) {
        net_poll();
        if (dh.state == st || dh.state < 0) break;
        task_yield();
    }
    return dh.state == st;
}

static bool dhcp(nif_t* f) {
    dh.f = f;
    dh.xid = (pit_uptime_ms() * 2654435761u) ^ f->mac[5] ^ (f->mac[4] << 8);
    // e1000 on qemu+kvm missed every try here: kvm replays lost pit ticks after
    // boot and our 1s turned into 3ms, while qemu's e1000 eats rx for a real 1s
    // after RCTL. Makefile has lost_tick_policy=discard now, extra tries anyway
    for (int i = 0; i < 4; i++) {
        dh.state = 1;
        dhcp_send(f, 1);
        if (!dhcp_wait(2, 1000)) continue;
        dh.state = 3;
        dhcp_send(f, 3);
        if (dhcp_wait(4, 1000)) break;
    }
    dh.f = NULL;
    return dh.state == 4;
}

static char* ipstr(char* p, uint32_t ip) {
    for (int i = 3; i >= 0; i--) {
        utoa((ip >> (i * 8)) & 0xFF, p, 10);
        while (*p) p++;
        if (i) *p++ = '.';
    }
    *p = 0;
    return p;
}

int net_init(void) {
    if (g_ready) return 0;
    if (rtl8139_init() == 0) {
        nifs[n_nifs].kind = NIC_RTL;
        memcpy(nifs[n_nifs].mac, rtl8139_mac(), 6);
        n_nifs++;
    }
    if (e1000_init() == 0) {
        nifs[n_nifs].kind = NIC_E1000;
        memcpy(nifs[n_nifs].mac, e1000_mac(), 6);
        n_nifs++;
    }
    int nv = vnet_init();
    for (int i = 0; i < nv && n_nifs < NIF_MAX; i++) {
        nifs[n_nifs].kind = NIC_VIRTIO;
        nifs[n_nifs].dev = i;
        memcpy(nifs[n_nifs].mac, vnet_mac(i), 6);
        n_nifs++;
    }
    if (!n_nifs) { set_status("net: no network card"); return -1; }
    for (int i = 0; i < n_nifs; i++) {
        nif_t* f = &nifs[i];
        f->name[0] = 'e'; f->name[1] = 't'; f->name[2] = 'h'; f->name[3] = (char)('0' + i); f->name[4] = 0;
        f->ip = IP4(10, 0, 2 + i, 15);
        f->gw = IP4(10, 0, 2 + i, 2);
        f->mask = 0xFFFFFF00u;
    }
    g_ready = true;

    /* dhcp on every card, keep the slirp guess if nobody answers */
    uint32_t dns = 0;
    bool got = false;
    for (int i = 0; i < n_nifs; i++) {
        nif_t* f = &nifs[i];
        if (!dhcp(f)) continue;
        f->ip = dh.ip;
        f->mask = dh.mask ? dh.mask : 0xFFFFFF00u;
        f->gw = dh.gw;
        if (!i) got = true;
        if (!dns) dns = dh.dns;
    }
    char s[64] = "net: up (", *p = s + 9;
    p = ipstr(p, nifs[0].ip);
    memcpy(p, " gw ", 4); p += 4;
    p = ipstr(p, nifs[0].gw);
    memcpy(p, got ? " dhcp)" : " static)", got ? 7 : 9);
    set_status(s);
    if (dns) {
        char rc[64] = "nameserver ";
        p = ipstr(rc + 11, dns);
        memcpy(p, "\nnameserver 1.1.1.1\n", 21);
        /* netd can beat userland_install, which writes the default one. wait for it */
        fs_node_t* n = NULL;
        for (int i = 0; i < 300 && !(n = fs_resolve(fs_root(), "/etc/resolv.conf")); i++) task_sleep_ms(10);
        if (n) fs_write(n, rc, strlen(rc));
    }

    /* warm ARP cache for gateways so first packet doesn't stall */
    for (int i = 1; i < n_nifs; i++) resolve_mac(nifs[i].gw, g_gw_mac, 500);
    if (resolve_mac(nifs[0].gw, g_gw_mac, 1500)) g_gw_known = true;
    for (const char* m = g_gw_known ? "samara: gw arp ok\r\n" : "samara: gw arp FAIL\r\n"; *m; m++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m); }
    uint8_t dns_mac[6];
    resolve_mac(dns ? dns : IP4(NET_IP_A, NET_IP_B, NET_IP_C, 3), dns_mac, 500);
    ip6_init();
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

    /* Synthesise a Host header from the IP literal if the caller didn't give
       us one — vhost-based servers refuse requests without it. */
    char host_buf[32];
    if (!host || !*host) {
        int p = 0;
        for (int oct = 0; oct < 4; oct++) {
            int v = (ip >> (24 - oct*8)) & 0xFF;
            if (v >= 100) host_buf[p++] = (char)('0' + v / 100);
            if (v >= 10)  host_buf[p++] = (char)('0' + (v / 10) % 10);
            host_buf[p++] = (char)('0' + v % 10);
            if (oct < 3) host_buf[p++] = '.';
        }
        if (port != 80) {
            host_buf[p++] = ':';
            char tmp[6]; int t = 0;
            int v = port;
            if (v == 0) tmp[t++] = '0';
            while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            for (int i = t - 1; i >= 0; i--) host_buf[p++] = tmp[i];
        }
        host_buf[p] = 0;
        host = host_buf;
    }

    char req[640];
    int n = 0;
    const char* g = "GET ";
    while (*g && n < (int)sizeof(req)) req[n++] = *g++;
    const char* p = path ? path : "/";
    while (*p && n < (int)sizeof(req)) req[n++] = *p++;
    const char* httpv = " HTTP/1.0\r\n";
    for (const char* q = httpv; *q && n < (int)sizeof(req); q++) req[n++] = *q;
    const char* hh = "Host: ";
    for (const char* q = hh; *q && n < (int)sizeof(req); q++) req[n++] = *q;
    for (const char* q = host; *q && n < (int)sizeof(req); q++) req[n++] = *q;
    const char* cr = "\r\n";
    for (const char* q = cr; *q && n < (int)sizeof(req); q++) req[n++] = *q;
    const char* ua =
        "User-Agent: samaraos/1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n";
    for (const char* q = ua; *q && n < (int)sizeof(req); q++) req[n++] = *q;

    if (tcp_send_all(req, n) < 0) { tcp_close_active(); return -3; }
    /* Idle timeout is now 10s — some hosts take a moment to flush. */
    int got = tcp_recv_until_close(out, outcap, 10000);
    tcp_close_active();
    return got;
}
