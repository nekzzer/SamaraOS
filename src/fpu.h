#ifndef SAMARA_FPU_H
#define SAMARA_FPU_H
#include "types.h"

/* Detect x87, clear EM, set MP/NE, FNINIT, install #NM/#MF handlers. */
void fpu_init(void);

/* True if a hardware FPU was detected. */
int fpu_present(void);

#endif
