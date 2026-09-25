#ifndef SAMARA_SOCK_H
#define SAMARA_SOCK_H
#include "core/types.h"

/* BSD-style sockets for user processes: AF_INET stream (TCP) and datagram
   (UDP) sockets on top of net.c, including listen/accept and loopback.
   Addresses and ports are host byte order inside the kernel. */

typedef struct sock sock_t;

void    sock_init(void);                      /* starts the netd polling task */

/* Packet input from net.c (payload = TCP/UDP header onwards). */
void    sock_input_tcp(uint32_t src, uint32_t dst, const uint8_t* seg, int len);
void    sock_input_udp(uint32_t src, uint32_t dst, const uint8_t* dgram, int len);

/* All return >= 0 or -errno. `nonblock` comes from the file's O_NONBLOCK. */
sock_t* sock_create(int type, int* err);      /* 1 = SOCK_STREAM, 2 = SOCK_DGRAM */
void    sock_ref(sock_t* s);
void    sock_close(sock_t* s);
int     sock_bind(sock_t* s, uint32_t ip, uint16_t port);
int     sock_listen(sock_t* s, int backlog);
sock_t* sock_accept(sock_t* s, bool nonblock, int* err, uint32_t* ip, uint16_t* port);
int     sock_connect(sock_t* s, uint32_t ip, uint16_t port, bool nonblock);
int     sock_send(sock_t* s, const uint8_t* buf, uint32_t len, bool nonblock,
                  const uint32_t* to_ip, const uint16_t* to_port);
int     sock_recv(sock_t* s, uint8_t* buf, uint32_t len, bool nonblock, bool peek,
                  uint32_t* from_ip, uint16_t* from_port);
int     sock_shutdown(sock_t* s, int how);
void    sock_name(sock_t* s, bool peer, uint32_t* ip, uint16_t* port);
int     sock_type(sock_t* s);
int     sock_take_error(sock_t* s);           /* SO_ERROR */
bool    sock_readable(sock_t* s);
bool    sock_writable(sock_t* s);
bool    sock_hup(sock_t* s);

#endif
