#ifndef SAMARA_GDT_H
#define SAMARA_GDT_H
#include "core/types.h"

void gdt_init(void);

/* Update the ring0 stack pointer that the CPU will load on a ring3->ring0
   transition. Call when the active kernel stack changes (e.g. task switch). */
void tss_set_esp0(uint32_t esp0);

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UCODE 0x1B          /* index 3, RPL 3 */
#define GDT_UDATA 0x23          /* index 4, RPL 3 */
#define GDT_TLS_INDEX 6
#define GDT_UTLS  0x33          /* index 6, RPL 3 */

/* Rewrite the user TLS descriptor (base of %gs for the running process). */
void gdt_set_tls(uint32_t base);

#endif
