#ifndef SAMARA_CLOCK_H
#define SAMARA_CLOCK_H
#include "core/types.h"

/* Wall clock: CMOS RTC sampled once at boot, advanced by the PIT. */
void     clock_init(void);
uint32_t clock_epoch(void);           /* seconds since 1970-01-01 UTC */
void     clock_now_us(uint32_t* sec, uint32_t* usec);
void     clock_now(uint32_t* sec, uint32_t* nsec);
void     clock_set(uint32_t sec, uint32_t nsec);       /* also writes the rtc */
void     clock_rtc_get(int* tm);                       /* struct rtc_time: 9 ints */
int      clock_rtc_set(const int* tm);
void     clock_rtc_uie(int on);
uint32_t clock_rtc_ups(void);

#endif
