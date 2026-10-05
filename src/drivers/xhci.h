#ifndef SAMARA_XHCI_H
#define SAMARA_XHCI_H
#include "core/types.h"

typedef struct xdev xdev_t;

int  xhci_up(void);                 /* 1 if a controller was found and started */
void xhci_poll(void);               /* ports, events, hid reports. from usbd */
/* bulk transfer on endpoint slot ep (1..4) of d: bytes moved or -cc, len <= 64k */
int  xhci_bulk(xdev_t* d, int ep, bool in, void* buf, int len);
int  xhci_unstall(xdev_t* d, int ep);
/* from xhci to usbms.c */
void usbms_attach(xdev_t* d, int in_ep, int out_ep);
void usbms_detach(xdev_t* d);

#endif
