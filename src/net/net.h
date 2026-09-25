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
void     net_send_ip(uint32_t dst, uint8_t proto, const void* payload, int len);
uint32_t net_src_for(uint32_t dst);        /* our address as seen by `dst` */

#endif
