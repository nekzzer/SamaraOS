#ifndef SAMARA_SOCK_H
#define SAMARA_SOCK_H
#include "core/types.h"

/* BSD-style sockets for user processes: AF_INET stream (TCP) and datagram
   (UDP) sockets on top of net.c, including listen/accept and loopback.
   Addresses and ports are host byte order inside the kernel. */

typedef struct sock sock_t;

void    sock_pump(void);
void    sock_init(void);                      /* starts the netd polling task */

/* Packet input from net.c (payload = TCP/UDP header onwards). */
void    sock_input_tcp(uint32_t src, uint32_t dst, const uint8_t* seg, int len);
void    sock_input_udp(uint32_t src, uint32_t dst, const uint8_t* dgram, int len);
void    sock_input_tcp6(const uint8_t* src, const uint8_t* dst, const uint8_t* seg, int len);
void    sock_input_udp6(const uint8_t* src, const uint8_t* dst, const uint8_t* dgram, int len);
void    sock_input_pkt(int ifi, const uint8_t* fr, int len, int pt);
extern int npkt_socks;
int     sock_pkt_ring(sock_t* s, uint32_t* rq);
void    sock_pkt_flush(void);
int     sock_pkt_ver(sock_t* s, int v);
int     sock_pkt_mmap(sock_t* s, uint64_t pd, uint64_t addr, uint64_t len);
int     sock_pkt_bind(sock_t* s, int ifidx, int proto);
int     sock_pkt_send(sock_t* s, const uint8_t* buf, uint32_t len, int ifidx, int proto, const uint8_t* mac);
void    sock_input_icmp6(const uint8_t* src, const uint8_t* dst, const uint8_t* msg, int len, int hl);
void    sock_input_icmp(uint32_t src, uint32_t dst, const uint8_t* pkt, int ihl, int total);
void    sock_rcvtmo(sock_t* s, uint32_t ms);
void    sock_opt(sock_t* s, int level, int name, int val);
int     sock_setcc(sock_t* s, const char* name, int len);
int     sock_getopt(sock_t* s, int level, int name, uint8_t* out, uint32_t* len);
int     sock_cmsg(sock_t* s, uint8_t* out, int cap);

/* All return >= 0 or -errno. `nonblock` comes from the file's O_NONBLOCK. */
/* addresses below are 16 bytes, v4 as ::ffff:a.b.c.d */
sock_t* sock_create(int af, int type, int proto, int* err);   /* type 1 stream, 2 dgram, 3 raw */
int     sock_af(sock_t* s);
int     sock_v6only(sock_t* s, int set, int val);   /* set < 0 = just read */
void    sock_ref(sock_t* s);
void    sock_close(sock_t* s);
int     sock_bind(sock_t* s, const uint8_t* ip, uint16_t port);
int     sock_listen(sock_t* s, int backlog);
sock_t* sock_accept(sock_t* s, bool nonblock, int* err, uint8_t* ip, uint16_t* port);
int     sock_connect(sock_t* s, const uint8_t* ip, uint16_t port, bool nonblock);
int     sock_send(sock_t* s, const uint8_t* buf, uint32_t len, bool nonblock,
                  const uint8_t* to_ip, const uint16_t* to_port);
int     sock_recv(sock_t* s, uint8_t* buf, uint32_t len, bool nonblock, bool peek,
                  uint8_t* from_ip, uint16_t* from_port);
int     sock_shutdown(sock_t* s, int how);
void    sock_name(sock_t* s, bool peer, uint8_t* ip, uint16_t* port);
int     sock_type(sock_t* s);
int     sock_take_error(sock_t* s);           /* SO_ERROR */
bool    sock_readable(sock_t* s);
bool    sock_writable(sock_t* s);
bool    sock_hup(sock_t* s);
struct wq_w; struct wq_ent;
struct wq_ent* sock_wq_cb(sock_t* s, void (*cb)(void*), void* arg);
struct wq_ent* sock_wq_add(sock_t* s, struct wq_w* w);   /* wakes on any readiness change */

#endif
