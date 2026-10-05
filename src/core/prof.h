#ifndef SAMARA_PROF_H
#define SAMARA_PROF_H
#include "core/types.h"

extern int prof_on, prof_gen;
extern uint64_t pf_anon, pf_cow, pf_file, pf_kern, tlb_ipis, tlb_rounds, uwin_bytes, bkl_wait, bkl_takes, bkl_slow;

static inline uint64_t prof_tsc(void) {
    uint32_t a, d;
    __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

void prof_sample(uint64_t rip, bool user, bool idle, const char* name);
void prof_sys(int nr, uint64_t cyc);
void prof_cmd(const char* s, uint32_t n);
int prof_dump(char* buf, int cap);

#endif
