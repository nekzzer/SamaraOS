#ifndef SAMARA_RTL8139_H
#define SAMARA_RTL8139_H
#include "types.h"

/* Polling-based RTL8139 driver. Init scans PCI for 10EC:8139. */

int  rtl8139_init(void);
bool rtl8139_present(void);
const uint8_t* rtl8139_mac(void);
const char*    rtl8139_status(void);

/* Send a raw Ethernet frame. Returns 0 on accept (queued), -1 on error. */
int  rtl8139_send(const void* data, int len);

/* Try to receive one packet. Returns length (or 0 if nothing available).
   Copies up to `max` bytes into `buf`. */
int  rtl8139_recv(void* buf, int max);

#endif
