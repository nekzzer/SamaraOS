#ifndef SAMARA_PCACHE_H
#define SAMARA_PCACHE_H
#include "core/types.h"
#include "fs/fs.h"

/* file pages shared between address spaces. the file bytes still live in
   node->data (ramfs style), this is the copy that mmap/exec map. */
#define PCM_W      1      /* writable: private = cow, shared = rw */
#define PCM_SHARED 2

int      pc_map(uint64_t pd, uint64_t va, fs_node_t* n, uint64_t off, uint64_t len, int mode);
void     pc_changed(fs_node_t* n);       /* file bytes changed under us */
void     pc_free(fs_node_t* n);          /* node goes away */
void     pc_sync(fs_node_t* n);          /* mmap writes -> node->data */
void     pc_sync_all(void);
uint64_t pc_reclaim(uint64_t want);      /* drop unmapped pages, returns pages freed */
uint64_t pc_pages(void);
uint64_t pc_idle_pages(void);            /* cached and nobody maps them */

#endif
