#include "boot/multiboot.h"
#include "core/string.h"

/* multiboot2 info -> the old mb1 struct, so kmain/gfx don't care who booted us.
   everything is copied to static buffers: the grub copy sits in free ram and the heap will eat it */

#define KOFF 0xffffffff80000000ull

uint8_t mb2_rsdp[36];
int mb2_have_rsdp;

static multiboot_info_t fake;
static struct { uint32_t size; uint64_t base, len; uint32_t type; } __attribute__((packed)) mm[64];
static struct { uint32_t start, end, string, res; } mods[8];
static char mnames[8][64];
static char cmd[512];

multiboot_info_t* mb2_convert(uint32_t addr) {
    uint8_t* p = (uint8_t*)(uint64_t)addr + 0xffff800000000000ull;   // dmap, same as P2V
    uint8_t* e = p + *(uint32_t*)p;
    int nm = 0, nmod = 0;
    p += 8;
    while (p < e) {
        uint32_t type = *(uint32_t*)p, size = *(uint32_t*)(p + 4);
        if (type == 0) break;
        if (type == 1) {
            strncpy(cmd, (char*)p + 8, sizeof(cmd) - 1);
            fake.cmdline = (uint32_t)((uint64_t)cmd - KOFF);
            fake.flags |= 4;
        } else if (type == 3 && nmod < 8) {
            mods[nmod].start = *(uint32_t*)(p + 8);
            mods[nmod].end = *(uint32_t*)(p + 12);
            strncpy(mnames[nmod], (char*)p + 16, 63);
            mods[nmod].string = (uint32_t)((uint64_t)mnames[nmod] - KOFF);
            nmod++;
        } else if (type == 6) {
            uint32_t esz = *(uint32_t*)(p + 8);
            for (uint8_t* q = p + 16; q + esz <= p + size && nm < 64; q += esz) {
                mm[nm].size = 20;
                mm[nm].base = *(uint64_t*)q;
                mm[nm].len = *(uint64_t*)(q + 8);
                mm[nm].type = *(uint32_t*)(q + 16);
                nm++;
            }
        } else if (type == 8) {
            fake.framebuffer_addr = *(uint64_t*)(p + 8);
            fake.framebuffer_pitch = *(uint32_t*)(p + 16);
            fake.framebuffer_width = *(uint32_t*)(p + 20);
            fake.framebuffer_height = *(uint32_t*)(p + 24);
            fake.framebuffer_bpp = p[28];
            fake.framebuffer_type = p[29];
            if (p[29] == 1) {
                fake.fb_red_field_position = p[32]; fake.fb_red_mask_size = p[33];
                fake.fb_green_field_position = p[34]; fake.fb_green_mask_size = p[35];
                fake.fb_blue_field_position = p[36]; fake.fb_blue_mask_size = p[37];
            }
            fake.flags |= MBI_FLAG_FRAMEBUFFER;
        } else if (type == 14 || type == 15) {
            if (!mb2_have_rsdp || type == 15) {
                memcpy(mb2_rsdp, p + 8, type == 15 ? 36 : 20);
                mb2_have_rsdp = 1;
            }
        }
        p += (size + 7) & ~7u;
    }
    if (nm) {
        fake.mmap_addr = (uint32_t)((uint64_t)mm - KOFF);
        fake.mmap_length = nm * 24;
        fake.flags |= 64;
    }
    if (nmod) {
        fake.mods_count = nmod;
        fake.mods_addr = (uint32_t)((uint64_t)mods - KOFF);
        fake.flags |= 8;
    }
    return &fake;
}
