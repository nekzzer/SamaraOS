#ifndef SAMARA_LAT_H
#define SAMARA_LAT_H
#include "core/types.h"

/* desktop latency samples, read from /proc/samara/lat */
enum { LAT_WM_FRAME, LAT_WM_GAP, LAT_IN2PRES, LAT_IN2CONT, LAT_WL_GAP, LAT_WL_IN, LAT_N };

uint64_t lat_now(void);                    /* us */
uint32_t lat_us(uint64_t cyc);             /* tsc ticks -> us */
void lat_add(int m, uint32_t us);
void lat_input(void);                      /* from the input irqs / feeders */
void lat_uw_present(void);                 /* a user window pushed pixels */
void lat_presented(void);                  /* wm put a frame on screen */
void lat_cmd(const char* s, uint32_t n);
int  lat_dump(char* buf, int cap);

#endif
