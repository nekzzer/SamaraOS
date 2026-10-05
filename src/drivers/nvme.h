#ifndef SAMARA_NVME_H
#define SAMARA_NVME_H
#include "core/types.h"

#define NVME_MAX 2                       /* namespaces: nvme0n1, nvme0n2 */

int      nvme_init(void);                /* number of namespaces */
bool     nvme_present(int i);
uint32_t nvme_sectors(int i);            /* 512 byte units */
int      nvme_read(int i, uint32_t lba, int count, void* buf);
int      nvme_write(int i, uint32_t lba, int count, const void* buf);
const char* nvme_model(void);

#endif
