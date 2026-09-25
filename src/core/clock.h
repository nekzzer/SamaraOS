#ifndef SAMARA_CLOCK_H
#define SAMARA_CLOCK_H
#include "core/types.h"

/* Wall clock: CMOS RTC sampled once at boot, advanced by the PIT. */
void     clock_init(void);
uint32_t clock_epoch(void);           /* seconds since 1970-01-01 UTC */
void     clock_now(uint32_t* sec, uint32_t* nsec);

#endif
