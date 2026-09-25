#ifndef SAMARA_FAT_H
#define SAMARA_FAT_H
#include "core/types.h"

/* Minimal read-only FAT12/FAT16 reader.
   Mount an ATA drive (index 0..3), then enumerate the root directory or read
   files by 8.3 name. Designed for QEMU `-drive file=fat:dir,format=raw`. */

bool fat_mount(int ata_idx);
bool fat_mounted(void);
const char* fat_status(void);

/* Iterate root directory entries. Calls cb for each regular file. */
void fat_list(void (*cb)(const char* name83, uint32_t size, void* user), void* user);

/* Find file by 8.3 short name (case-insensitive). Allocates a buffer from the
   kernel heap and returns it via *out_buf / *out_size. Caller must kfree.
   Returns 0 on success, negative on error. */
int  fat_read_file(const char* name83, uint8_t** out_buf, uint32_t* out_size);

#endif
