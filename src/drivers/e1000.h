#ifndef SAMARA_E1000_H
#define SAMARA_E1000_H
#include "core/types.h"

/* Intel e1000 (qemu default, virtualbox, vmware). polling like rtl8139 */
int  e1000_init(void);
const uint8_t* e1000_mac(void);
int  e1000_send(const void* data, int len);
int  e1000_recv(void* buf, int max);

#endif
