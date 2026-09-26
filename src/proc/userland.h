#ifndef SAMARA_USERLAND_H
#define SAMARA_USERLAND_H

/* Populate the ramfs with the embedded busybox (/bin/busybox), one stub per
   applet (/bin/ls, /usr/bin/awk, ...) and a minimal /etc. Returns the number
   of applets installed, or -1. */
int userland_install(void);
/* Unpack the boot modules (QEMU -initrd tar archives) into the ramfs, zero-copy.
   Returns the number of files. */
int userland_install_modules(void);

#endif
