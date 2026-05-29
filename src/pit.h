#ifndef SAMARA_PIT_H
#define SAMARA_PIT_H
#include "types.h"

void pit_init(uint32_t hz);
uint32_t pit_ticks(void);
uint32_t pit_uptime_ms(void);

#endif
