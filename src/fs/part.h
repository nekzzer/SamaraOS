#ifndef SAMARA_PART_H
#define SAMARA_PART_H
#include "core/types.h"

int  part_scan(int disk);        /* mbr/gpt, adds sda1.. through ata_part_add */
void part_scan_all(void);

#endif
