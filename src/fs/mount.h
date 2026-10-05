#ifndef SAMARA_MOUNT_H
#define SAMARA_MOUNT_H
#include "core/types.h"
#include "fs/fs.h"

/* mount table: what /proc/mounts shows, ro flags, tmpfs limits.
   ext2 and fat register themselves in their mount functions. */

#define MNT_TMP_ID 16                 /* tmpfs mount ids 16..23 */

void mnt_init(void);                  /* after fs_init: proc, dev ... lines */
void mnt_add(const char* type, const char* dev, fs_node_t* at, int disk);
void mnt_set_ro(fs_node_t* at);
int  mnt_mount(fs_node_t* src, fs_node_t* dst, const char* type, uint64_t flags, const char* data);
int  mnt_umount(fs_node_t* n);        /* n: the mount point or a /dev node */
int  mnt_text(char* out, int cap);
bool mnt_ro(fs_node_t* n);
bool mnt_ro_id(int id);
int  mnt_grow(fs_node_t* n, uint64_t newsize);    /* before a file gets bigger: -EROFS / -ENOSPC */
int  mnt_newnode(fs_node_t* dir);                 /* before a new name in dir */
void mnt_tmpfs_boot(const char* path);
int  mnt_statfs(fs_node_t* n, uint64_t* b);       /* the 15 words of struct statfs */
void fs_free_tree(fs_node_t* n);                  /* the children of n, not n */
bool fs_tree_busy(fs_node_t* n);                  /* open files below */

#endif
