#ifndef SAMARA_BOOTMOD_H
#define SAMARA_BOOTMOD_H
#include "core/types.h"

/* Multiboot modules (QEMU -initrd "a.tar,b.tar"), moved to the top of RAM at
   boot so the heap and frame pool never overlap them. Physical addresses;
   reach the bytes through P2V(). Each one is a ustar archive unpacked into
   the ramfs without copying (see proc/userland.c). */
typedef struct {
    uint32_t start, end;
    char     name[64];
} bootmod_t;

int boot_modules(const bootmod_t** out);

#endif
