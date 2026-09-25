#ifndef SAMARA_SB16_H
#define SAMARA_SB16_H
#include "core/types.h"

/* SoundBlaster 16 / SB Pro / SB compat driver — 8-bit mono PCM via ISA DMA.
   For first cut: single-cycle DMA on channel 1, max ~32 KB per shot. Caller
   tracks playback duration; sb16_busy() can be polled to wait for completion. */

bool sb16_init(void);
bool sb16_present(void);
const char* sb16_status(void);

/* Plays len bytes of 8-bit unsigned PCM at hz Hz, mono. Returns 0 on success.
   Buffer is copied into the driver's DMA-safe bounce buffer, so caller's
   memory can be freed/reused after this returns. Max len = SB16_MAX_CHUNK. */
#define SB16_MAX_CHUNK (32u * 1024u)
int  sb16_play_pcm_u8_mono(const uint8_t* pcm, uint32_t len, uint32_t hz);

void sb16_stop(void);
bool sb16_busy(void);          /* true while last chunk is still playing */

#endif
