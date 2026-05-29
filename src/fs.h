#ifndef SAMARA_FS_H
#define SAMARA_FS_H
#include "types.h"

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
} fs_node_t;

void        fs_init(void);
fs_node_t*  fs_root(void);
fs_node_t*  fs_resolve(fs_node_t* cwd, const char* path);   /* NULL if missing */
fs_node_t*  fs_create(fs_node_t* cwd, const char* path, fs_type_t type);
int         fs_unlink(fs_node_t* cwd, const char* path);
int         fs_write(fs_node_t* file, const char* data, size_t len);  /* replaces */
int         fs_append(fs_node_t* file, const char* data, size_t len);
void        fs_path(fs_node_t* node, char* out, size_t cap);

#endif
