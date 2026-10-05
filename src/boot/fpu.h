#ifndef SAMARA_FPU_H
#define SAMARA_FPU_H
#include "core/types.h"

/* Detect x87, clear EM, set MP/NE, FNINIT, install #NM/#MF handlers. */
void fpu_init(void);

/* True if a hardware FPU was detected. */
void fpu_cpu_init(void);     /* per cpu part, for the aps */
int fpu_present(void);
int fpu_sse(void);           /* SSE/SSE2 enabled for programs (CR4.OSFXSR) */

#endif
