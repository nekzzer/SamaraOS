/* Installs WAV blobs that were linked into the kernel via `objcopy -I binary`
   from the project's embed/ directory. The list of entries is auto-generated
   into src/embed_list.h by the Makefile so the user just drops files. */

#include "apps/embed.h"
#include "fs/fs.h"
#include "core/string.h"
#include "core/types.h"

/* Step 1: declare extern symbols for every embedded file. The generated
   header expands EMBED(name, sym) to one declaration per file. */
#define EMBED(name, sym) \
    extern uint8_t _binary_##sym##_start[]; \
    extern uint8_t _binary_##sym##_end[];
#include "apps/embed_list.h"
#undef EMBED

typedef struct { const char* name; uint8_t* s; uint8_t* e; } embed_t;

/* Step 2: build a sentinel-terminated list of items. */
#define EMBED(name, sym) { name, _binary_##sym##_start, _binary_##sym##_end },
static embed_t items[] = {
    #include "apps/embed_list.h"
    { 0, 0, 0 }
};
#undef EMBED

void embed_install(void) {
    fs_node_t* dir = fs_resolve(fs_root(), "/home/user");
    if (!dir) return;
    for (int i = 0; items[i].name; i++) {
        fs_node_t* f = fs_create(dir, items[i].name, FS_FILE);
        if (!f || f->type != FS_FILE) continue;
        uint32_t size = (uint32_t)(items[i].e - items[i].s);
        fs_write(f, (const char*)items[i].s, size);
    }
}
