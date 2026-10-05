#ifndef SAMARA_SWAP_H
#define SAMARA_SWAP_H
#include "core/types.h"

/* linux compatible swap (SWAPSPACE2) on a whole disk. a slot is a 4k page,
   slot ids are (area << 28) | page, page 0 is the header */
#define SWAP_AREAS 4

int      swap_on(int disk, int prio);                  /* 0 or -errno */
int      swap_off(int disk);
int      swap_active(void);
int      swap_alloc(void);                             /* -1 when full */
void     swap_dup(uint64_t slot);
void     swap_put(uint64_t slot);
int      swap_write(uint64_t slot, int n, const void* buf);    /* n slots in a row */
int      swap_read(uint64_t slot, void* buf);
uint64_t swap_total(void);                             /* pages */
uint64_t swap_free(void);
int      swap_info(int i, char* name, uint64_t* size, uint64_t* used, int* prio);   /* kb */
int      swap_area_of(int disk);

/* mem.c */
void     mem_init(void);
uint64_t mem_low(void);
uint64_t mem_reclaim(void);
bool     mem_oom(void);
void     mem_check(void);
extern uint32_t mem_oom_kills, mem_swapouts;

#endif
