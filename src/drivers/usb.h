#ifndef SAMARA_USB_H
#define SAMARA_USB_H
#include "core/types.h"

/* UHCI host controller + HID boot mouse. Polled from a kernel task. */

void        usb_init(void);
const char* usb_status(void);

#endif
