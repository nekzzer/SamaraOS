#ifndef _LIBC_TIME_H
#define _LIBC_TIME_H

#include "../../core/types.h"

typedef int32_t  time_t;
typedef int32_t  clock_t;

#define CLOCKS_PER_SEC 1000

struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

time_t time(time_t* t);
clock_t clock(void);
struct tm* localtime(const time_t* t);
struct tm* gmtime(const time_t* t);
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm);

#endif
