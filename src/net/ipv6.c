#include "net/net.h"
#include "net/sock.h"
#include "core/string.h"
#include "core/heap.h"
#include "core/task.h"
#include "core/io.h"
#include "boot/pit.h"

/* IPv6: ethernet 0x86DD, ICMPv6 echo + NDP, SLAAC from RAs, a default route.
   No DAD, no MLD, no fragments (they get dropped), no pmtu discovery. */

#define A6_MAX   4
#define NB_SLOTS 16
#define PFX_MAX  2
#define LOOP6    32

typedef struct { uint8_t a[16]; uint8_t plen, scope; } a6_t;

typedef struct {
    a6_t addr[A6_MAX];
    int  na;
    uint8_t pfx[PFX_MAX][16];
    uint8_t pfx_len[PFX_MAX];
    int  np;
    uint8_t rtr[16];
    bool rtr_ok;
    uint32_t rtr_exp;
    uint16_t mtu;
    int  rs_n;
    uint32_t rs_at;
    bool got_ra;
} if6_t;

static if6_t v6[6];
static int n6;

static struct { uint8_t ip[16]; uint8_t mac[6]; int ifi; bool v; } nb[NB_SLOTS];
static int nb_next;

static uint8_t* g_loop[LOOP6];
static int g_loop_len[LOOP6], lh, lt;

static const uint8_t lo6[16] = { [15] = 1 };
static const uint8_t allnodes[16] = { 0xFF, 2, [15] = 1 };

static inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t be32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static bool is_ll(const uint8_t* a) { return a[0] == 0xFE && (a[1] & 0xC0) == 0x80; }
static bool is_mc(const uint8_t* a) { return a[0] == 0xFF; }
static bool is_zero(const uint8_t* a) { for (int i = 0; i < 16; i++) if (a[i]) return false; return true; }
static bool same(const uint8_t* a, const uint8_t* b) { return !memcmp(a, b, 16); }

static bool pfx_match(const uint8_t* a, const uint8_t* p, int len) {
    int n = len / 8;
    if (memcmp(a, p, n)) return false;
    if (len % 8) { uint8_t m = (uint8_t)(0xFF << (8 - len % 8)); if ((a[n] ^ p[n]) & m) return false; }
    return true;
}

static uint16_t sum_part(uint32_t s, const void* d, int len) {
    const uint8_t* p = (const uint8_t*)d;
    while (len > 1) { s += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)s;
}

/* 0 when the data (with its checksum field) is good */
uint16_t ip6_sum(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* d, int len) {
    uint8_t ph[8] = { 0, 0, (uint8_t)(len >> 8), (uint8_t)len, 0, 0, 0, proto };
    uint32_t s = sum_part(0, src, 16);
    s = sum_part(s, dst, 16);
    s = sum_part(s, ph, 8);
    s = sum_part(s, d, len);
    return (uint16_t)~s;
}

static void ll_from_mac(uint8_t* a, const uint8_t* mac) {
    memset(a, 0, 16);
    a[0] = 0xFE; a[1] = 0x80;
    a[8] = mac[0] ^ 2; a[9] = mac[1]; a[10] = mac[2]; a[11] = 0xFF; a[12] = 0xFE;
    a[13] = mac[3]; a[14] = mac[4]; a[15] = mac[5];
}

static void snode(uint8_t* out, const uint8_t* a) {
    memset(out, 0, 16);
    out[0] = 0xFF; out[1] = 2; out[11] = 1; out[12] = 0xFF;
    out[13] = a[13]; out[14] = a[14]; out[15] = a[15];
}

static void mc_mac(uint8_t* mac, const uint8_t* a) {
    mac[0] = mac[1] = 0x33;
    memcpy(mac + 2, a + 12, 4);
}

/* ours? *ifi = card, -1 for ::1 */
static bool mine(const uint8_t* a, int* ifi) {
    if (same(a, lo6)) { *ifi = -1; return true; }
    for (int i = 0; i < n6; i++)
        for (int k = 0; k < v6[i].na; k++)
            if (same(v6[i].addr[k].a, a)) { *ifi = i; return true; }
    return false;
}

static bool mine_snode(const uint8_t* a) {
    uint8_t t[16];
    for (int i = 0; i < n6; i++)
        for (int k = 0; k < v6[i].na; k++) { snode(t, v6[i].addr[k].a); if (same(t, a)) return true; }
    return false;
}

static void nb_store(const uint8_t* ip, const uint8_t* mac, int ifi) {
    int s = -1;
    for (int i = 0; i < NB_SLOTS; i++) if (nb[i].v && same(nb[i].ip, ip)) s = i;
    if (s < 0) { s = nb_next; nb_next = (nb_next + 1) % NB_SLOTS; }
    memcpy(nb[s].ip, ip, 16);
    memcpy(nb[s].mac, mac, 6);
    nb[s].ifi = ifi;
    nb[s].v = true;
}

static bool nb_find(const uint8_t* ip, uint8_t* mac) {
    for (int i = 0; i < NB_SLOTS; i++)
        if (nb[i].v && same(nb[i].ip, ip)) { memcpy(mac, nb[i].mac, 6); return true; }
    return false;
}

/* link-local of card i, or whatever it has first */
static const uint8_t* ll_of(int i) { return v6[i].addr[0].a; }

static const uint8_t* mac_of(int i) {
    const char* n; const uint8_t* m; uint32_t x, y, z, r, t;
    net_ifinfo(i, &n, &m, &x, &y, &z, &r, &t);
    return m;
}

static void send_raw(int ifi, const uint8_t* dmac, const uint8_t* src, const uint8_t* dst,
                     uint8_t proto, uint8_t hl, const void* pl, int len) {
    static uint8_t f[1600];                  /* leaf, irqs are off here. stack is only 8k */
    if (len + 40 > 1560) return;
    f[0] = 0x60; f[1] = f[2] = f[3] = 0;
    f[4] = (uint8_t)(len >> 8); f[5] = (uint8_t)len;
    f[6] = proto; f[7] = hl;
    memcpy(f + 8, src, 16);
    memcpy(f + 24, dst, 16);
    memcpy(f + 40, pl, len);
    net_eth_send(ifi, dmac, 0x86DD, f, 40 + len);
}

/* icmpv6 message with checksum filled in, for ND (hop limit 255) */
static void send_icmp(int ifi, const uint8_t* dmac, const uint8_t* src, const uint8_t* dst, uint8_t* m, int len) {
    m[2] = m[3] = 0;
    uint16_t c = ip6_sum(src, dst, 58, m, len);
    m[2] = (uint8_t)(c >> 8); m[3] = (uint8_t)c;
    send_raw(ifi, dmac, src, dst, 58, 255, m, len);
}

static void send_ns(int ifi, const uint8_t* target) {
    uint8_t m[32], d[16], dm[6];
    memset(m, 0, sizeof(m));
    m[0] = 135;
    memcpy(m + 8, target, 16);
    m[24] = 1; m[25] = 1;
    memcpy(m + 26, mac_of(ifi), 6);
    snode(d, target);
    mc_mac(dm, d);
    send_icmp(ifi, dm, ll_of(ifi), d, m, 32);
}

static void send_rs(int ifi) {
    uint8_t m[16], dm[6] = { 0x33, 0x33, 0, 0, 0, 2 };
    uint8_t d[16] = { 0xFF, 2, [15] = 2 };
    memset(m, 0, sizeof(m));
    m[0] = 133;
    m[8] = 1; m[9] = 1;
    memcpy(m + 10, mac_of(ifi), 6);
    send_icmp(ifi, dm, ll_of(ifi), d, m, 16);
}

static void send_na(int ifi, const uint8_t* dmac, const uint8_t* src, const uint8_t* dst, const uint8_t* target) {
    uint8_t m[32];
    memset(m, 0, sizeof(m));
    m[0] = 136;
    m[4] = is_zero(dst) ? 0x20 : 0x60;
    memcpy(m + 8, target, 16);
    m[24] = 2; m[25] = 1;
    memcpy(m + 26, mac_of(ifi), 6);
    send_icmp(ifi, dmac, src, dst, m, 32);
}

static bool addr_add(int i, const uint8_t* a, int plen, int scope) {
    if6_t* f = &v6[i];
    for (int k = 0; k < f->na; k++) if (same(f->addr[k].a, a)) return true;
    if (f->na >= A6_MAX) return false;
    memcpy(f->addr[f->na].a, a, 16);
    f->addr[f->na].plen = (uint8_t)plen;
    f->addr[f->na].scope = (uint8_t)scope;
    f->na++;
    return true;
}

static void ra_input(int ifi, const uint8_t* smac, const uint8_t* src, const uint8_t* m, int n) {
    if6_t* f = &v6[ifi];
    if (n < 16) return;
    uint16_t life = be16(*(const uint16_t*)(m + 6));
    const uint8_t* o = m + 16;
    const uint8_t* end = m + n;
    const uint8_t* rmac = smac;
    while (o + 8 <= end && o[1]) {
        int l = o[1] * 8;
        if (o + l > end) break;
        if (o[0] == 1 && l >= 8) rmac = o + 2;
        else if (o[0] == 3 && l >= 32) {
            int plen = o[2];
            const uint8_t* p = o + 16;
            uint32_t valid = be32(*(const uint32_t*)(o + 4));
            if (!is_ll(p) && valid) {
                if (o[3] & 0x80) {
                    int k;
                    for (k = 0; k < f->np; k++) if (same(f->pfx[k], p) && f->pfx_len[k] == plen) break;
                    if (k == f->np && f->np < PFX_MAX) { memcpy(f->pfx[k], p, 16); f->pfx_len[k] = (uint8_t)plen; f->np++; }
                }
                if ((o[3] & 0x40) && plen == 64) {
                    uint8_t a[16];
                    memcpy(a, p, 8);
                    memcpy(a + 8, ll_of(ifi) + 8, 8);
                    addr_add(ifi, a, 64, 0);
                }
            }
        } else if (o[0] == 5 && l >= 8) {
            uint32_t mtu = be32(*(const uint32_t*)(o + 4));
            if (mtu >= 1280 && mtu <= 1500) f->mtu = (uint16_t)mtu;
        }
        o += l;
    }
    if (life) {
        memcpy(f->rtr, src, 16);
        f->rtr_ok = true;
        f->rtr_exp = pit_uptime_ms() + (uint32_t)life * 1000;
        nb_store(src, rmac, ifi);
    } else if (same(f->rtr, src)) f->rtr_ok = false;
    f->got_ra = true;
}

/* route lookup: card, next hop, source address. 0 or -101 */
static int route6(const uint8_t* dst, int* ifi, uint8_t* nh, uint8_t* src) {
    int i = -1;
    int k;
    if (mine(dst, &k)) {
        *ifi = -1;
        memcpy(nh, dst, 16);
        memcpy(src, dst, 16);
        return 0;
    }
    if (!n6) return -101;
    if (is_mc(dst) || is_ll(dst)) {
        i = 0;
        memcpy(nh, dst, 16);
    } else {
        for (int c = 0; c < n6 && i < 0; c++) {
            for (k = 0; k < v6[c].np; k++) if (pfx_match(dst, v6[c].pfx[k], v6[c].pfx_len[k])) i = c;
            for (k = 1; k < v6[c].na; k++) if (pfx_match(dst, v6[c].addr[k].a, v6[c].addr[k].plen)) i = c;
        }
        if (i >= 0) memcpy(nh, dst, 16);
        else {
            for (int c = 0; c < n6; c++)
                if (v6[c].rtr_ok && (int32_t)(v6[c].rtr_exp - pit_uptime_ms()) > 0) { i = c; break; }
            if (i < 0) return -101;
            memcpy(nh, v6[i].rtr, 16);
        }
    }
    *ifi = i;
    const uint8_t* s = ll_of(i);
    if (!is_ll(dst) && !is_mc(dst))
        for (k = 1; k < v6[i].na; k++) { s = v6[i].addr[k].a; if (pfx_match(dst, s, v6[i].addr[k].plen)) break; }
    memcpy(src, s, 16);
    return 0;
}

bool ip6_src_for(const uint8_t* dst, uint8_t* src) {
    int i; uint8_t nh[16];
    return route6(dst, &i, nh, src) == 0;
}

/* the same dance as arp: ask, then poll while yielding */
static bool nd_resolve(int ifi, const uint8_t* nh, uint8_t* mac) {
    if (is_mc(nh)) { mc_mac(mac, nh); return true; }
    if (nb_find(nh, mac)) return true;
    uint32_t t0 = pit_uptime_ms();
    for (int n = 0; n < 4; n++) {
        send_ns(ifi, nh);
        uint32_t dl = pit_uptime_ms() + 250;
        while ((int32_t)(pit_uptime_ms() - dl) < 0) {
            net_poll();
            if (nb_find(nh, mac)) return true;
            task_yield();
        }
        if (pit_uptime_ms() - t0 > 1000) break;
    }
    return false;
}

static void queue_loop(const uint8_t* pkt, int len) {
    int next = (lh + 1) % LOOP6;
    if (next == lt) return;
    if (!g_loop[lh]) g_loop[lh] = (uint8_t*)kmalloc(1600);
    if (!g_loop[lh]) return;
    memcpy(g_loop[lh], pkt, len);
    g_loop_len[lh] = len;
    lh = next;
}

static int send_hl(const uint8_t* src, const uint8_t* dst, uint8_t proto, uint8_t hl, const void* pl, int len) {
    uint8_t s[16], nh[16], mac[6];
    int ifi;
    int r = route6(dst, &ifi, nh, s);
    if (r < 0) return r;
    if (src && !is_zero(src)) memcpy(s, src, 16);
    if (ifi < 0) {
        static uint8_t f[1600];
        if (len + 40 > 1560) return -90;
        f[0] = 0x60; f[1] = f[2] = f[3] = 0;
        f[4] = (uint8_t)(len >> 8); f[5] = (uint8_t)len;
        f[6] = proto; f[7] = hl;
        memcpy(f + 8, s, 16);
        memcpy(f + 24, dst, 16);
        memcpy(f + 40, pl, len);
        queue_loop(f, 40 + len);
        return 0;
    }
    if (len + 40 > (v6[ifi].mtu ? v6[ifi].mtu : 1500)) return -90;
    if (!nd_resolve(ifi, nh, mac)) return -113;                /* EHOSTUNREACH */
    send_raw(ifi, mac, s, dst, proto, hl, pl, len);
    return 0;
}

int ip6_send(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* pl, int len) {
    return send_hl(src, dst, proto, 64, pl, len);
}

static void icmp_input(int ifi, const uint8_t* smac, const uint8_t* src, const uint8_t* dst,
                       uint8_t hl, const uint8_t* m, int n) {
    if (n < 8) return;
    if (ip6_sum(src, dst, 58, m, n)) return;
    int k;
    switch (m[0]) {
    case 128: {
        static uint8_t buf[1500];
        uint8_t s[16];
        if (n > 1500 - 40) return;
        memcpy(buf, m, n);
        buf[0] = 129; buf[2] = buf[3] = 0;
        if (is_mc(dst) || !mine(dst, &k)) { int i; uint8_t nh[16]; if (route6(src, &i, nh, s) < 0) return; }
        else memcpy(s, dst, 16);
        uint16_t c = ip6_sum(s, src, 58, buf, n);
        buf[2] = (uint8_t)(c >> 8); buf[3] = (uint8_t)c;
        send_hl(s, src, 58, 64, buf, n);
        break;
    }
    case 135:
        if (hl != 255 || n < 24 || ifi < 0) return;
        if (!mine(m + 8, &k) || k != ifi) return;
        if (n >= 32 && m[24] == 1 && m[25] == 1 && !is_zero(src)) { nb_store(src, m + 26, ifi); smac = m + 26; }
        if (is_zero(src)) { uint8_t mm[6] = { 0x33, 0x33, 0, 0, 0, 1 }; send_na(ifi, mm, m + 8, allnodes, m + 8); }
        else send_na(ifi, smac, m + 8, src, m + 8);
        return;
    case 136: {
        if (hl != 255 || n < 24 || ifi < 0) return;
        const uint8_t* mac = smac;
        if (n >= 32 && m[24] == 2 && m[25] == 1) mac = m + 26;
        if (!mine(m + 8, &k)) nb_store(m + 8, mac, ifi);
        return;
    }
    case 134:
        if (hl != 255 || !is_ll(src) || ifi < 0) return;
        ra_input(ifi, smac, src, m, n);
        return;
    }
    if (m[0] < 130) sock_input_icmp6(src, dst, m, n);
}

void ip6_input(int ifi, const uint8_t* p, int len, const uint8_t* smac) {
    if (len < 40 || (p[0] >> 4) != 6) return;
    int pl = (p[4] << 8) | p[5];
    if (40 + pl > len) return;
    const uint8_t *src = p + 8, *dst = p + 24;
    int k;
    if (!mine(dst, &k) && !same(dst, allnodes) && !mine_snode(dst)) return;
    int nh = p[6], off = 40, end = 40 + pl;
    for (;;) {
        if (nh == 0 || nh == 43 || nh == 60 || nh == 135 || nh == 139 || nh == 140) {
            if (off + 8 > end) return;
            int l = (p[off + 1] + 1) * 8;
            nh = p[off];
            off += l;
        } else if (nh == 51) {
            if (off + 8 > end) return;
            int l = (p[off + 1] + 2) * 4;
            nh = p[off];
            off += l;
        } else if (nh == 44) {
            if (off + 8 > end) return;
            if ((p[off + 2] << 8 | p[off + 3]) & 0xFFF9) return;   /* real fragment: sorry */
            nh = p[off];
            off += 8;
        } else break;
        if (off > end) return;
    }
    const uint8_t* d = p + off;
    int n = end - off;
    if (nh == 58) icmp_input(ifi, smac, src, dst, p[7], d, n);
    else if (nh == 6) sock_input_tcp6(src, dst, d, n);
    else if (nh == 17) sock_input_udp6(src, dst, d, n);
}

void ip6_poll(void) {
    for (int i = 0; i < 16 && lt != lh; i++) {
        int s = lt;
        lt = (lt + 1) % LOOP6;
        ip6_input(-1, g_loop[s], g_loop_len[s], NULL);
    }
    uint32_t now = pit_uptime_ms();
    for (int i = 0; i < n6; i++) {
        if6_t* f = &v6[i];
        if (f->got_ra || f->rs_n >= 3 || (int32_t)(now - f->rs_at) < 0) continue;
        send_rs(i);
        f->rs_n++;
        f->rs_at = now + 2000 * f->rs_n;
    }
}

static char* hex4(char* p, uint32_t v) {
    char t[8];
    utoa(v, t, 16);
    for (char* q = t; *q; q++) *p++ = *q;
    return p;
}

int ip6_str(char* out, const uint8_t* a) {
    int bs = -1, bl = 0;
    for (int i = 0; i < 8; i++) {
        int j = i;
        while (j < 8 && !a[j * 2] && !a[j * 2 + 1]) j++;
        if (j - i > bl && j - i > 1) { bs = i; bl = j - i; }
    }
    char* p = out;
    for (int i = 0; i < 8; i++) {
        if (i == bs) { *p++ = ':'; if (i == 0) *p++ = ':'; i += bl - 1; continue; }
        p = hex4(p, a[i * 2] << 8 | a[i * 2 + 1]);
        if (i < 7) *p++ = ':';
    }
    *p = 0;
    return (int)(p - out);
}

void ip6_init(void) {
    n6 = net_ifcount();
    if (n6 > 6) n6 = 6;
    for (int i = 0; i < n6; i++) {
        uint8_t a[16];
        ll_from_mac(a, mac_of(i));
        v6[i].na = 0;
        addr_add(i, a, 64, 0x20);
        v6[i].rs_at = pit_uptime_ms() + 2000;
        send_rs(i);
        v6[i].rs_n = 1;
    }
    /* slirp answers right away, real routers may take a bit. not waiting long */
    uint32_t t0 = pit_uptime_ms();
    bool all = false;
    while (!all && pit_uptime_ms() - t0 < 400) {
        net_poll();
        all = true;
        for (int i = 0; i < n6; i++) if (!v6[i].got_ra) all = false;
        task_yield();
    }
    for (int i = 0; i < n6; i++)
        for (int k = 1; k < v6[i].na; k++) {
            char s[48];
            const char* m = "samara: ip6 ";
            ip6_str(s, v6[i].addr[k].a);
            for (; *m; m++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *m); }
            for (char* q = s; *q; q++) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *q); }
            outb(0x3F8, '\r'); outb(0x3F8, '\n');
        }
}

int ip6_addr_add(int ifi, const uint8_t* a, int plen) {
    if (ifi < 0 || ifi >= n6) return -19;
    if (is_ll(a) || is_mc(a) || is_zero(a)) return -22;
    return addr_add(ifi, a, plen, 0) ? 0 : -28;
}

int ip6_addr_del(int ifi, const uint8_t* a) {
    if (ifi < 0 || ifi >= n6) return -19;
    if6_t* f = &v6[ifi];
    for (int k = 1; k < f->na; k++)
        if (same(f->addr[k].a, a)) { f->addr[k] = f->addr[--f->na]; return 0; }
    return -99;
}

/* ifi -1 = lo */
int ip6_addrs(int ifi, uint8_t addrs[][16], uint8_t* plen, uint8_t* scope, int max) {
    if (ifi < 0) {
        if (max < 1) return 0;
        memcpy(addrs[0], lo6, 16); plen[0] = 128; scope[0] = 0x10;
        return 1;
    }
    if (ifi >= n6) return 0;
    int n = v6[ifi].na < max ? v6[ifi].na : max;
    for (int k = 0; k < n; k++) {
        memcpy(addrs[k], v6[ifi].addr[k].a, 16);
        plen[k] = v6[ifi].addr[k].plen;
        scope[k] = v6[ifi].addr[k].scope;
    }
    return n;
}

int ip6_routes(ip6_route_t* r, int max) {
    int n = 0;
    uint32_t now = pit_uptime_ms();
    if (n < max) { memset(&r[n], 0, sizeof(*r)); memcpy(r[n].dst, lo6, 16); r[n].dlen = 128; r[n].ifi = -1; r[n].metric = 0; r[n].flags = 0x8001; n++; }
    for (int i = 0; i < n6; i++) {
        if6_t* f = &v6[i];
        if (n < max) { memset(&r[n], 0, sizeof(*r)); memcpy(r[n].dst, f->addr[0].a, 8); r[n].dlen = 64; r[n].ifi = i; r[n].metric = 256; r[n].flags = 1; n++; }
        for (int k = 0; k < f->np && n < max; k++) {
            memset(&r[n], 0, sizeof(*r)); memcpy(r[n].dst, f->pfx[k], 16); r[n].dlen = f->pfx_len[k];
            r[n].ifi = i; r[n].metric = 256; r[n].flags = 1; n++;
        }
        for (int k = 1; k < f->na && n < max; k++) {
            uint8_t t[16]; int j;
            memcpy(t, f->addr[k].a, 16);
            for (j = f->addr[k].plen; j < 128; j++) t[j / 8] &= (uint8_t)~(0x80 >> (j % 8));
            for (j = 0; j < f->np; j++) if (same(f->pfx[j], t) && f->pfx_len[j] == f->addr[k].plen) break;
            if (j < f->np) continue;                      /* RA prefix, listed already */
            memset(&r[n], 0, sizeof(*r)); memcpy(r[n].dst, t, 16); r[n].dlen = f->addr[k].plen;
            r[n].ifi = i; r[n].metric = 256; r[n].flags = 1; n++;
        }
        if (f->rtr_ok && (int32_t)(f->rtr_exp - now) > 0 && n < max) {
            memset(&r[n], 0, sizeof(*r)); memcpy(r[n].gw, f->rtr, 16);
            r[n].ifi = i; r[n].metric = 1024; r[n].flags = 3; n++;
        }
    }
    return n;
}
