#ifndef SAMARA_IMGDEC_H
#define SAMARA_IMGDEC_H
#include "core/types.h"

/* PNG / baseline JPEG / GIF -> 0xAARRGGBB pixels (kmalloc_big, kfree them). NULL if unknown or broken. */
uint32_t* img_decode(const uint8_t* d, int n, int* w, int* h);

#endif
