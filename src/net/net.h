#ifndef SAMARA_NET_H
#define SAMARA_NET_H
#include "core/types.h"

/* Hard-coded QEMU SLIRP defaults so wget/ping work out of the box.
   Override via net_set_addr() if needed. */
#define NET_IP_A 10
#define NET_IP_B 0
#define NET_IP_C 2
#define NET_IP_D 15
#define NET_GW_D 2

#define IP4(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

int  net_init(void);                       /* brings up NIC + ARP + sets IP */
bool net_ready(void);
const char* net_status(void);
const uint8_t* net_mac(void);
uint32_t       net_ip(void);
uint32_t       net_gw(void);
/* all the cards: eth0, eth1, ... */
int  net_ifcount(void);
bool net_ifinfo(int i, const char** name, const uint8_t** mac, uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* rx, uint32_t* tx);

/* Pump RX: dispatch ARP, ICMP, TCP. Call this in any loop that waits. */
void net_poll(void);

/* ICMP echo. Returns round-trip ms (0..timeout), -1 on timeout. */
int  net_ping(uint32_t ip, uint32_t timeout_ms);

/* HTTP GET. Opens TCP to ip:port, sends "GET path HTTP/1.0\r\nHost: host\r\n\r\n",
   reads response into out (up to outcap). Returns bytes read, -1 on error.
   `host` may be NULL (then "Host" header is skipped). */
int  net_http_get(uint32_t ip, uint16_t port, const char* host, const char* path,
                  uint8_t* out, int outcap);

/* For the socket layer (net/sock.c). Addresses in host byte order. */
extern uint8_t net_ttl;                    /* 0 = default 64, set around net_send_ip */
void     net_send_ip(uint32_t dst, uint8_t proto, const void* payload, int len);
uint8_t* net_buf_get(int big);               /* pooled 1600 / 16k+ byte buffers */
void     net_buf_put(uint8_t* p, int big);
uint32_t net_rxwnd(uint32_t dst);          /* how much the nic can take in one burst */
uint32_t net_src_for(uint32_t dst);        /* our address as seen by `dst` */

#define A6_MAX 4
/* ipv6.c, addresses are 16 bytes network order */
typedef struct { uint8_t dst[16], gw[16]; int dlen, ifi, metric; uint32_t flags; } ip6_route_t;
void ip6_init(void);
void ip6_poll(void);
void ip6_input(int ifi, const uint8_t* pkt, int len, const uint8_t* smac);
int  ip6_send(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* pl, int len);
bool ip6_src_for(const uint8_t* dst, uint8_t* src);   /* false = no route */
uint16_t ip6_sum(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* d, int len);
int  ip6_str(char* out, const uint8_t* a);
int  ip6_addr_add(int ifi, const uint8_t* a, int plen);
int  ip6_addr_del(int ifi, const uint8_t* a);
int  ip6_addrs(int ifi, uint8_t addrs[][16], uint8_t* plen, uint8_t* scope, int max);   /* ifi -1 = lo */
int  ip6_routes(ip6_route_t* r, int max);
/* net.c */
int  net_raw_send(int ifi, const void* fr, int len);
int  net_eth_send(int ifi, const uint8_t* dmac, uint16_t type, const void* pl, int len);

#endif
