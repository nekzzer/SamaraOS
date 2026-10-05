#ifndef SAMARA_EXT2_H
#define SAMARA_EXT2_H
#include "core/types.h"
#include "fs/fs.h"

/* ext2 volumes loaded into the ramfs tree, written back in the background
   like FAT (fatfs.h). ext3/ext4 mount read-only. */
bool ext2_probe(int disk);
void ext2_label(int disk, char* out);    /* 17 bytes */
int  ext2_mount(int disk, fs_node_t* at);   /* 0 rw, 1 read-only, <0 -errno */
int  ext2_sync_all(void);
bool ext2_statfs(fs_node_t* n, uint32_t* bs, uint64_t* tot, uint64_t* fr, uint32_t* ino);   /* ino: total, free */
int  ext2_umount(fs_node_t* at);
void ext2_dirty(int id);
void ext2_throttle(void);
void ext2_make_room(size_t need);         /* sync + drop clean files, for a writer that ran dry */                 /* file arena nearly full: wait for a sync */

#endif
