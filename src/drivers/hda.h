#ifndef SAMARA_HDA_H
#define SAMARA_HDA_H
#include "core/types.h"

/* intel hda, one output stream, polled (no irq). the ring is HDA_BUF bytes,
   snd.c puts alsa on top of it */

#define HDA_BUF 65536

bool     hda_init(void);
bool     hda_present(void);
const char* hda_status(void);
uint8_t* hda_ring(void);
void     hda_setup(uint32_t rate, uint32_t buf, uint32_t per);   /* resets the stream, zeroes the ring */
void     hda_run(bool on);
uint32_t hda_lpib(void);
int      hda_vol_max(void);
void     hda_vol_get(int* l, int* r, int* sw);
void     hda_vol_set(int l, int r, int sw);

#endif
