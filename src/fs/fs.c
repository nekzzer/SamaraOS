#include "fs/fs.h"
#include "core/heap.h"
#include "core/string.h"
#include "core/clock.h"
#include "drivers/ata.h"

static fs_node_t* root_;

static fs_node_t* node_new(const char* name, fs_type_t type, fs_node_t* parent) {
    fs_node_t* n = (fs_node_t*)kmalloc(sizeof(fs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_NAME_MAX - 1);
    n->type = type;
    n->parent = parent;
    n->mode = type == FS_DIR ? 0755 : 0644;
    n->mtime = fs_now();
    return n;
}

static void link_child(fs_node_t* dir, fs_node_t* c) {
    c->next = dir->child;
    dir->child = c;
}

void fs_init(void) {
    root_ = node_new("/", FS_DIR, NULL);

    fs_node_t* home = node_new("home", FS_DIR, root_);
    fs_node_t* etc  = node_new("etc",  FS_DIR, root_);
    fs_node_t* tmp  = node_new("tmp",  FS_DIR, root_);
    link_child(root_, tmp);
    link_child(root_, etc);
    link_child(root_, home);

    fs_node_t* dev = node_new("dev", FS_DIR, root_);
    link_child(root_, dev);
    static const struct { const char* name; uint8_t dev; } devs[] = {
        { "null", FS_DEV_NULL }, { "zero", FS_DEV_ZERO }, { "tty", FS_DEV_TTY },
        { "console", FS_DEV_TTY }, { "random", FS_DEV_RANDOM }, { "urandom", FS_DEV_RANDOM },
        { "fb0", FS_DEV_FB }, { "input", FS_DEV_INPUT }, { "ptmx", FS_DEV_PTMX },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        fs_node_t* d = node_new(devs[i].name, FS_FILE, dev);
        d->dev = devs[i].dev;
        d->mode = 0666;
        link_child(dev, d);
    }

    link_child(dev, node_new("pts", FS_DIR, dev));      /* pty slaves appear here */

    fs_node_t* user = node_new("user", FS_DIR, home);
    link_child(home, user);

    fs_node_t* readme = node_new("README.txt", FS_FILE, user);
    const char* msg =
        "Welcome to SamaraOS!\n"
        "Try: help, neofetch, ls, cat README.txt, nano notes.txt\n";
    readme->size = strlen(msg);
    readme->cap  = readme->size + 1;
    readme->data = (char*)kmalloc(readme->cap);
    memcpy(readme->data, msg, readme->size + 1);
    link_child(user, readme);

    fs_node_t* hostname = node_new("hostname", FS_FILE, etc);
    const char* hn = "samara\n";
    hostname->size = strlen(hn);
    hostname->cap  = hostname->size + 1;
    hostname->data = (char*)kmalloc(hostname->cap);
    memcpy(hostname->data, hn, hostname->size + 1);
    link_child(etc, hostname);
}

fs_node_t* fs_root(void) { return root_; }

uint32_t fs_now(void) { return clock_epoch(); }

void (*fs_dirty_hook)(int mount_id);

void fs_touch(fs_node_t* n) {
    for (; n; n = n->parent)
        if (n->mount_id) { if (fs_dirty_hook) fs_dirty_hook(n->mount_id); return; }
}

static fs_node_t* find_child(fs_node_t* dir, const char* name) {
    if (!dir || dir->type != FS_DIR) return NULL;
    if (!strcmp(name, "."))  return dir;
    if (!strcmp(name, "..")) return dir->parent ? dir->parent : dir;
    for (fs_node_t* c = dir->child; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

fs_node_t* fs_resolve(fs_node_t* cwd, const char* path) {
    if (!path || !*path) return cwd;
    fs_node_t* cur = (path[0] == '/') ? root_ : cwd;
    char part[FS_NAME_MAX];
    int pi = 0;
    for (const char* p = (path[0] == '/') ? path + 1 : path; ; p++) {
        if (*p == '/' || *p == 0) {
            if (pi > 0) {
                part[pi] = 0;
                cur = find_child(cur, part);
                if (!cur) return NULL;
                pi = 0;
            }
            if (*p == 0) break;
        } else if (pi < FS_NAME_MAX - 1) {
            part[pi++] = *p;
        }
    }
    return cur;
}

static void split_dir_base(const char* path, char* dir, char* base) {
    int slash = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') slash = i;
    if (slash < 0) { dir[0] = 0; strncpy(base, path, FS_NAME_MAX - 1); base[FS_NAME_MAX-1]=0; return; }
    if (slash == 0) { dir[0] = '/'; dir[1] = 0; }
    else { for (int i = 0; i < slash; i++) dir[i] = path[i]; dir[slash] = 0; }
    strncpy(base, path + slash + 1, FS_NAME_MAX - 1);
    base[FS_NAME_MAX - 1] = 0;
}

fs_node_t* fs_create(fs_node_t* cwd, const char* path, fs_type_t type) {
    char dir[256], base[FS_NAME_MAX];
    split_dir_base(path, dir, base);
    fs_node_t* parent = dir[0] ? fs_resolve(cwd, dir) : cwd;
    if (!parent || parent->type != FS_DIR) return NULL;
    if (find_child(parent, base)) return NULL;
    fs_node_t* n = node_new(base, type, parent);
    if (!n) return NULL;
    link_child(parent, n);
    fs_touch(parent);
    return n;
}

int fs_unlink(fs_node_t* cwd, const char* path) {
    fs_node_t* n = fs_resolve(cwd, path);
    if (!n || n == root_) return -1;
    if (n->type == FS_DIR && n->child) return -1;
    fs_detach(n);
    if (n->refs > 0) { n->unlinked = true; return 0; }   /* still open */
    fs_data_free(n);
    kfree(n);
    return 0;
}

void fs_detach(fs_node_t* n) {
    fs_node_t* p = n->parent;
    if (!p) return;
    fs_touch(p);
    fs_node_t** link = &p->child;
    while (*link && *link != n) link = &(*link)->next;
    if (*link == n) *link = n->next;
    n->next = NULL;
    n->parent = NULL;
    p->mtime = fs_now();
}

void fs_attach(fs_node_t* dir, fs_node_t* n) {
    n->parent = dir;
    link_child(dir, n);
    dir->mtime = fs_now();
    fs_touch(dir);
}

void fs_release(fs_node_t* n) {
    if (!n || --n->refs > 0) return;
    if (n->unlinked) {
        fs_data_free(n);
        kfree(n);
    }
}

fs_node_t* fs_child(fs_node_t* dir, const char* name) { return find_child(dir, name); }

void fs_data_free(fs_node_t* n) {
    if (n->data && n->cap) kfree(n->data);
    n->data = NULL;
    n->cap = 0;
}

void fs_set_static(fs_node_t* n, const char* data, size_t len) {
    fs_data_free(n);
    n->data = (char*)data;
    n->size = len;
    n->cap = 0;
    n->mtime = fs_now();
    fs_touch(n);
}

int fs_write(fs_node_t* f, const char* data, size_t len) {
    if (!f || f->type != FS_FILE) return -1;
    if (f->cap < len + 1) {
        fs_data_free(f);
        f->cap = len + 64;
        f->data = (char*)kmalloc_big(f->cap);
        if (!f->data) { f->cap = 0; f->size = 0; return -1; }
    }
    memcpy(f->data, data, len);
    f->data[len] = 0;
    f->size = len;
    f->mtime = fs_now();
    fs_touch(f);
    return (int)len;
}

int fs_append(fs_node_t* f, const char* data, size_t len) {
    if (!f || f->type != FS_FILE) return -1;
    size_t need = f->size + len + 1;
    if (f->cap < need) {
        char* nb = (char*)kmalloc_big(need + 64);
        if (!nb) return -1;
        if (f->data) memcpy(nb, f->data, f->size);
        fs_data_free(f);
        f->data = nb; f->cap = need + 64;
    }
    memcpy(f->data + f->size, data, len);
    f->size += len;
    f->data[f->size] = 0;
    f->mtime = fs_now();
    fs_touch(f);
    return (int)len;
}

void fs_path(fs_node_t* n, char* out, size_t cap) {
    if (!n) { if (cap) out[0] = 0; return; }
    if (n == root_) { strncpy(out, "/", cap); out[cap-1]=0; return; }
    char tmp[256] = {0};
    fs_node_t* parts[64];
    int np = 0;
    for (fs_node_t* c = n; c && c != root_ && np < 64; c = c->parent) parts[np++] = c;
    int pos = 0;
    for (int i = np - 1; i >= 0; i--) {
        tmp[pos++] = '/';
        for (const char* s = parts[i]->name; *s && pos < (int)sizeof(tmp) - 1; s++) tmp[pos++] = *s;
    }
    tmp[pos] = 0;
    strncpy(out, tmp, cap); out[cap-1] = 0;
}

void fs_add_disk_nodes(void) {
    fs_node_t* dev = fs_resolve(root_, "/dev");
    if (!dev) return;
    for (int i = 0; i < DISK_MAX; i++) {
        if (!ata_drive_present(i) || find_child(dev, ata_drive_name(i))) continue;
        fs_node_t* d = node_new(ata_drive_name(i), FS_FILE, dev);
        if (!d) return;
        d->dev = (uint8_t)(FS_DEV_DISK + i);
        d->mode = 0660;
        link_child(dev, d);
    }
}
