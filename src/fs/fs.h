#ifndef SAMARA_FS_H
#define SAMARA_FS_H
#include "core/types.h"

#define FS_NAME_MAX 32

typedef enum { FS_FILE = 1, FS_DIR = 2 } fs_type_t;

typedef struct fs_node {
    char name[FS_NAME_MAX];
    fs_type_t type;
    struct fs_node* parent;
    struct fs_node* child;     /* dir: first child */
    struct fs_node* next;      /* sibling */
    char* data;                /* file: contents */
    size_t size;
    size_t cap;
    uint16_t mode;             /* permission bits (0755 dirs, 0644 files) */
    uint8_t  dev;              /* FS_DEV_* for /dev nodes, 0 = plain */
    bool     unlinked;         /* removed while open: freed on last release */
    int      refs;             /* open file descriptions pointing here */
    uint32_t mtime;
    uint8_t  mount_id;         /* nonzero on the root of a mounted volume */
} fs_node_t;

enum { FS_DEV_NONE = 0, FS_DEV_NULL, FS_DEV_ZERO, FS_DEV_TTY, FS_DEV_RANDOM, FS_DEV_FB, FS_DEV_INPUT };
#define FS_DEV_DISK 16                 /* FS_DEV_DISK + ata index: /dev/hda.. /dev/sda.. */

void        fs_add_disk_nodes(void);      /* after disks are probed */

void        fs_init(void);
fs_node_t*  fs_root(void);
fs_node_t*  fs_resolve(fs_node_t* cwd, const char* path);   /* NULL if missing */
fs_node_t*  fs_create(fs_node_t* cwd, const char* path, fs_type_t type);
int         fs_unlink(fs_node_t* cwd, const char* path);
int         fs_write(fs_node_t* file, const char* data, size_t len);  /* replaces */
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

#endif
