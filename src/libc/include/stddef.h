#ifndef _LIBC_STDDEF_H
#define _LIBC_STDDEF_H

#include "../../core/types.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define offsetof(t, m) __builtin_offsetof(t, m)

typedef int ptrdiff_t;
typedef unsigned int wchar_t;

#endif
