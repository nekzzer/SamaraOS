#ifndef SAMARA_FATFS_H
#define SAMARA_FATFS_H
#include "core/types.h"
#include "fs/fs.h"

/* Persistent FAT16/FAT32 volumes (with long file names) mounted into the
   ramfs tree.

   Mounting loads the whole volume into ramfs nodes under the mount point;
   from then on every program works on it like any other directory. Any
   change below the mount point marks the volume dirty, and a background
   task writes the tree back (about a second after the last change, on
   sync(2) and on umount). Write-back lays the tree out afresh, but only
   clusters/sectors whose contents actually changed hit the disk. */

#define FATFS_MAX_MOUNTS 4

void fatfs_init(void);                                  /* starts the sync task */
int  fatfs_mount(int disk, fs_node_t* at);              /* 0 or -errno */
int  fatfs_umount(fs_node_t* at);
int  fatfs_sync_all(void);
bool fatfs_probe(int disk);                             /* looks like FAT16/32 */
/* "/dev/sda /mnt vfat rw 0 0\n" lines for /proc/mounts; returns length. */
int  fatfs_mounts_text(char* out, int cap);
/* Mount id owning `n` (0 = plain ramfs) and that volume's usage. */
int  fatfs_owner(fs_node_t* n);
bool fatfs_statfs(int id, uint32_t* csize, uint32_t* total, uint32_t* free_clus);

#endif
