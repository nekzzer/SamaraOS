#ifndef _LIBC_STDBOOL_H
#define _LIBC_STDBOOL_H

/* Match the kernel-side macros from src/types.h so DOOM's
   doomtype.h sees the guard and skips its enum redefinition. */
#include "../../types.h"

#define __bool_true_false_are_defined 1
#define bool _Bool

#endif
