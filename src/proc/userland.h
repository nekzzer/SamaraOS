#ifndef SAMARA_USERLAND_H
#define SAMARA_USERLAND_H

/* Populate the ramfs with the embedded busybox (/bin/busybox), one stub per
   applet (/bin/ls, /usr/bin/awk, ...) and a minimal /etc. Returns the number
   of applets installed, or -1. */
int userland_install(void);

#endif
