#ifndef SAMARA_MULTIBOOT_H
#define SAMARA_MULTIBOOT_H
#include "core/types.h"

#define MB1_BOOTED_MAGIC  0x2BADB002
#define MBI_FLAG_FRAMEBUFFER 0x1000

typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;        /* 0=indexed, 1=rgb, 2=ega-text */
    uint8_t  fb_red_field_position;
    uint8_t  fb_red_mask_size;
    uint8_t  fb_green_field_position;
    uint8_t  fb_green_mask_size;
    uint8_t  fb_blue_field_position;
    uint8_t  fb_blue_mask_size;
} __attribute__((packed)) multiboot_info_t;

#endif
