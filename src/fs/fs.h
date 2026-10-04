#ifndef SAMARA_FS_H
#define SAMARA_FS_H
#include "core/types.h"

#define FS_NAME_MAX 128

typedef enum { FS_FILE = 1, FS_DIR = 2, FS_LINK = 3 } fs_type_t;   /* LINK: data = target */

typedef struct fs_node {
    char name[FS_NAME_MAX];
    fs_type_t type;
    struct fs_node* parent;
    struct fs_node* child;     /* dir: first child */
    struct fs_node* next;      /* sibling */
    char* data;                /* file: contents; cap == 0 && data: borrowed,
                                  read-only (boot archive) - copied on write */
    size_t size;
    size_t cap;
    uint16_t mode;             /* permission bits (0755 dirs, 0644 files) */
    uint8_t  dev;              /* FS_DEV_* for /dev nodes, 0 = plain */
    bool     unlinked;         /* removed while open: freed on last release */
    int      refs;             /* open file descriptions pointing here */
    uint32_t mtime;
    uint8_t  mount_id;         /* nonzero on the root of a mounted volume */
    uint8_t  lazy_vol;
    uint32_t lazy;             /* ext2 ino whose bytes aren't read yet: data NULL, size is right */
} fs_node_t;

enum { FS_DEV_NONE = 0, FS_DEV_NULL, FS_DEV_ZERO, FS_DEV_TTY, FS_DEV_RANDOM, FS_DEV_FB, FS_DEV_INPUT,
       FS_DEV_PTMX, FS_DEV_SOCK,       /* SOCK: a bound AF_UNIX path, /tmp/.X11-unix/X0 */
       FS_DEV_EVKBD, FS_DEV_EVMOUSE,
       FS_DEV_DRM, FS_DEV_DRMR };   /* /dev/dri/card0, renderD128 */   /* /dev/input-kbd, /dev/input-mouse: evdev for xorg */
#define FS_DEV_DISK 16                 /* FS_DEV_DISK + ata index: /dev/hda.. /dev/sda.. */
#define FS_DEV_PTS  64                 /* FS_DEV_PTS + n: /dev/pts/n (pty slaves) */
#define FS_DEV_IS_DISK(d) ((d) >= FS_DEV_DISK && (d) < FS_DEV_PTS)
#define FS_DEV_IS_PTS(d)  ((d) >= FS_DEV_PTS && (d) < FS_DEV_PTS + 16)

void        fs_add_disk_nodes(void);      /* after disks are probed */

void        fs_init(void);
fs_node_t*  fs_root(void);
fs_node_t*  fs_resolve(fs_node_t* cwd, const char* path);   /* NULL if missing; follows symlinks */
fs_node_t*  fs_resolve_nf(fs_node_t* cwd, const char* path);  /* the last one is not followed */
/* same, but a lazy file stays on disk: stat, access, readlink don't need the bytes */
fs_node_t*  fs_peek(fs_node_t* cwd, const char* path, bool follow);
extern int (*fs_lazy_hook)(fs_node_t* n);
void        fs_need(fs_node_t* n);            /* read a lazy file in */
bool        fs_recent(fs_node_t* n);          /* looked up just now: exec & co read ->data right after */
void        fs_need_tree(fs_node_t* n);       /* a whole subtree (moving it to another volume) */
fs_node_t*  fs_owner(fs_node_t* n);           /* the mount root above n */
void        fs_write_begin(void);             /* a syscall that changes files/tree */
void        fs_write_end(void);
void        fs_sync_begin(void);              /* a volume sync: waits writers out */
void        fs_sync_end(void);
void        fs_wait_room(uint32_t need);
void        fs_need_room(uint32_t need);     /* sync + drop clean files, from anywhere that may sleep */     /* writer out of file memory: wait for a sync */
fs_node_t*  fs_symlink(fs_node_t* dir, const char* name, const char* target);
fs_node_t*  fs_create(fs_node_t* cwd, const char* path, fs_type_t type);
int         fs_unlink(fs_node_t* cwd, const char* path);
int         fs_write(fs_node_t* file, const char* data, size_t len);  /* replaces */
void        fs_data_free(fs_node_t* n);       /* drop contents (owned ones are freed) */
/* Contents in place without copying (must outlive the node, e.g. a boot
   module); the first write makes a private copy. */
void        fs_set_static(fs_node_t* n, const char* data, size_t len);
int         fs_append(fs_node_t* file, const char* data, size_t len);
void        fs_path(fs_node_t* node, char* out, size_t cap);
void        fs_release(fs_node_t* node);          /* drop an open reference */
fs_node_t*  fs_child(fs_node_t* dir, const char* name);
void        fs_detach(fs_node_t* node);           /* unlink from parent, keep node */
void        fs_attach(fs_node_t* dir, fs_node_t* node);
uint32_t    fs_now(void);
/* Note a change at/under `n` so the owning mounted volume gets synced. */
void        fs_touch(fs_node_t* n);
extern void (*fs_dirty_hook)(int mount_id);
extern void (*fs_free_hook)(fs_node_t* n);    /* an unlinked node is about to be freed */

#endif
