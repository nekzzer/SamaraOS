/* Boot-time PCM tune generator. Creates a couple of short WAV files in the
   ramfs so the user can do `playwav welcome.wav` right out of the box without
   any external assets. The PCM is 8-bit unsigned mono at 11025 Hz — small,
   loud enough through SB16, and within the single-cycle DMA limit when
   chunked by wav.c. */

#include "apps/synth.h"
#include "fs/fs.h"
#include "core/heap.h"
#include "core/string.h"

typedef struct { uint16_t freq; uint16_t ms; } note_t;

#define R(d) { 0, (d) }
#define E5 659
#define D5 587
#define C5 523
#define B4 494
#define A4 440
#define G4 392
#define F4 349
#define E4 330
#define D4 294
#define C4 262

static const note_t welcome_notes[] = {
    {E5, 250}, {B4, 130}, {C5, 130}, {D5, 250}, {C5, 130}, {B4, 130},
    {A4, 250}, {A4, 130}, {C5, 130}, {E5, 250}, {D5, 130}, {C5, 130},
    {B4, 400}, {C5, 130}, {D5, 250}, {E5, 250},
    {C5, 250}, {A4, 250}, {A4, 500}, R(200),
};

/* Triangle wave is gentler on the ear than a square. 8-bit unsigned, 0..255
   centred on 128. */
static uint8_t triangle_sample(uint32_t phase, uint32_t period) {
    if (period < 2) return 128;
    uint32_t half = period / 2;
    uint32_t p = phase % period;
    int v;
    if (p < half) v = (int)((p * 200) / half) - 100;        /* -100..+99 */
    else          v = 100 - (int)(((p - half) * 200) / half);
    return (uint8_t)(128 + v);
}

/* Render notes[n] at hz into pcm[max], return bytes written. */
static uint32_t render_notes(const note_t* notes, int n, uint32_t hz,
                              uint8_t* pcm, uint32_t max) {
    uint32_t pos = 0;
    for (int i = 0; i < n && pos < max; i++) {
        uint32_t samples = (uint32_t)((uint64_t)notes[i].ms * hz / 1000);
        if (notes[i].freq == 0) {
            for (uint32_t j = 0; j < samples && pos < max; j++) pcm[pos++] = 128;
            continue;
        }
        uint32_t period = hz / notes[i].freq;
        uint32_t att = hz / 200;        /* 5 ms attack */
        uint32_t rel = hz / 50;         /* 20 ms release */
        for (uint32_t j = 0; j < samples && pos < max; j++) {
            int s = (int)triangle_sample(j, period) - 128;
            /* envelope: linear attack & release, sustain in the middle */
            int g = 256;
            if (j < att) g = (int)(256 * j / att);
            else if (samples > rel && j >= samples - rel)
                g = (int)(256 * (samples - j) / rel);
            int v = (s * g) >> 8;
            pcm[pos++] = (uint8_t)(128 + v);
        }
    }
    return pos;
}

/* Write a RIFF/WAVE/PCM header for 'data_size' bytes of 8-bit mono at hz. */
static void write_wav_header(uint8_t* hdr, uint32_t data_size, uint32_t hz) {
    uint32_t riff_size = 4 + (8 + 16) + (8 + data_size);
    memcpy(hdr + 0,  "RIFF", 4);
    hdr[4] =  riff_size        & 0xFF;
    hdr[5] = (riff_size >> 8)  & 0xFF;
    hdr[6] = (riff_size >> 16) & 0xFF;
    hdr[7] = (riff_size >> 24) & 0xFF;
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;       /* fmt size = 16 */
    hdr[20] = 1;  hdr[21] = 0;                                  /* PCM */
    hdr[22] = 1;  hdr[23] = 0;                                  /* mono */
    hdr[24] =  hz        & 0xFF;
    hdr[25] = (hz >> 8)  & 0xFF;
    hdr[26] = (hz >> 16) & 0xFF;
    hdr[27] = (hz >> 24) & 0xFF;
    /* byte rate = sample_rate (mono, 1 byte/sample) */
    hdr[28] =  hz        & 0xFF;
    hdr[29] = (hz >> 8)  & 0xFF;
    hdr[30] = (hz >> 16) & 0xFF;
    hdr[31] = (hz >> 24) & 0xFF;
    hdr[32] = 1;  hdr[33] = 0;                                  /* block align */
    hdr[34] = 8;  hdr[35] = 0;                                  /* bits/sample */
    memcpy(hdr + 36, "data", 4);
    hdr[40] =  data_size        & 0xFF;
    hdr[41] = (data_size >> 8)  & 0xFF;
    hdr[42] = (data_size >> 16) & 0xFF;
    hdr[43] = (data_size >> 24) & 0xFF;
}

/* Create a ramfs file at /home/user/<name> with 'size' bytes of 'data'. */
static void install_file(const char* name, const uint8_t* data, uint32_t size) {
    fs_node_t* home = fs_resolve(fs_root(), "/home/user");
    if (!home) return;
    fs_node_t* f = fs_create(home, name, FS_FILE);
    if (!f) return;
    fs_write(f, (const char*)data, size);
}

void synth_install_demo_wavs(void) {
    const uint32_t hz = 11025;
    const int n = (int)(sizeof welcome_notes / sizeof welcome_notes[0]);

    /* ---- welcome.wav ---- */
    {
        /* worst-case total ms */
        uint32_t total_ms = 0;
        for (int i = 0; i < n; i++) total_ms += welcome_notes[i].ms;
        uint32_t pcm_max = (uint32_t)((uint64_t)total_ms * hz / 1000) + 16;
        uint8_t* buf = (uint8_t*)kmalloc(44 + pcm_max);
        if (!buf) return;
        uint32_t written = render_notes(welcome_notes, n, hz, buf + 44, pcm_max);
        write_wav_header(buf, written, hz);
        install_file("welcome.wav", buf, 44 + written);
        kfree(buf);
    }

    /* ---- tone.wav : 1 second of 440 Hz triangle ---- */
    {
        uint32_t samples = hz;
        uint8_t* buf = (uint8_t*)kmalloc(44 + samples);
        if (!buf) return;
        uint32_t period = hz / 440;
        for (uint32_t i = 0; i < samples; i++) {
            int s = (int)triangle_sample(i, period) - 128;
            int g = 256;
            if (i < hz/20) g = (int)(256 * i / (hz/20));
            else if (i > samples - hz/20) g = (int)(256 * (samples - i) / (hz/20));
            int v = (s * g) >> 8;
            buf[44 + i] = (uint8_t)(128 + v);
        }
        write_wav_header(buf, samples, hz);
        install_file("tone.wav", buf, 44 + samples);
        kfree(buf);
    }
}
