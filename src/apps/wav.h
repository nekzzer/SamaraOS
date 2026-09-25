#ifndef SAMARA_WAV_H
#define SAMARA_WAV_H
#include "core/types.h"

/* Minimal RIFF/WAVE parser. Supports PCM (format 1), mono, 8-bit unsigned
   or 16-bit signed. 16-bit input is downmixed to 8-bit on the fly when sent
   to the SB16 driver (which uses 8-bit DMA on channel 1). */

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t* pcm;         /* into source buffer, no copy */
    uint32_t pcm_size;          /* bytes of PCM data */
} wav_info_t;

/* Parses 'buf' (size 'len'). Returns 0 if it looks like a usable WAV. */
int  wav_parse(const uint8_t* buf, uint32_t len, wav_info_t* out);

/* High-level: load a WAV from memory and play through SB16. Blocks until
   playback is queued; for chunks > SB16_MAX_CHUNK it streams chunk-by-chunk
   and waits between. Returns 0 on success. */
int  wav_play(const uint8_t* buf, uint32_t len);

#endif
