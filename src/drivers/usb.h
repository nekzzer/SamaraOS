#ifndef SAMARA_USB_H
#define SAMARA_USB_H
#include "core/types.h"

/* usb: xhci (qemu-xhci) and uhci host controllers, hid devices, mass storage.
   everything polled from the usbd kernel task. */

void        usb_init(void);
const char* usb_status(void);

/* interfaces out of a configuration descriptor */
typedef struct {
    uint8_t num, cls, sub, proto;
    uint16_t rdlen;                 /* hid report descriptor length */
    int nep;
    struct { uint8_t addr, attr, interval; uint16_t mps; } ep[4];
} usb_if_t;
int usb_ifaces(const uint8_t* cfg, int len, usb_if_t* out, int max);

/* hid: boot keyboard or whatever the report descriptor says (mice, tablets).
   reports go to the input layer as evdev events */
typedef struct hid hid_t;
hid_t* hid_new(const char* name, uint16_t vid, uint16_t pid, int boot_kbd, const uint8_t* rd, int rlen);
void   hid_report(hid_t* h, const uint8_t* b, int len);
void   hid_tick(hid_t* h);          /* key repeat for the DE, call often */
void   hid_free(hid_t* h);

/* bot mass storage on xhci, see usbms.c. disks are 0..USB_DISKS-1 */
#define USB_DISKS 4
bool     usbms_present(int i);
uint32_t usbms_sectors(int i);
int      usbms_read(int i, uint32_t lba, int count, void* buf);
int      usbms_write(int i, uint32_t lba, int count, const void* buf);

#endif
