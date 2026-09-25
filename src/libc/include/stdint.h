#ifndef _LIBC_STDINT_H
#define _LIBC_STDINT_H

#include "../../core/types.h"

typedef int32_t  intptr_t;
typedef uint32_t uintptr_t;

#define INT8_MAX   0x7f
#define INT16_MAX  0x7fff
#define INT32_MAX  0x7fffffff
#define INT64_MAX  0x7fffffffffffffffLL
#define INT8_MIN   (-INT8_MAX-1)
#define INT16_MIN  (-INT16_MAX-1)
#define INT32_MIN  (-INT32_MAX-1)
#define INT64_MIN  (-INT64_MAX-1)
#define UINT8_MAX  0xffU
#define UINT16_MAX 0xffffU
#define UINT32_MAX 0xffffffffU
#define UINT64_MAX 0xffffffffffffffffULL

#define INT32_C(c) c
#define UINT32_C(c) c##U

#endif
