#ifndef SAMARA_SYNTH_H
#define SAMARA_SYNTH_H
#include "core/types.h"

/* Generates a few short 8-bit PCM WAV files into the ramfs:
   - /home/user/welcome.wav  : the Korobeiniki melody (~4 s)
   - /home/user/tone.wav     : 1 s 440 Hz sine
   Safe to call exactly once after fs_init() and heap_init(). */
void synth_install_demo_wavs(void);

#endif
