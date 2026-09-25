#include "apps/wav.h"
#include "drivers/sb16.h"
#include "core/string.h"
#include "core/io.h"
#include "boot/pit.h"
#include "drivers/keyboard.h"
#include "drivers/vga.h"

static uint32_t rd_u32_le(const uint8_t* p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16_le(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

int wav_parse(const uint8_t* buf, uint32_t len, wav_info_t* out) {
    if (!buf || len < 44 || !out) return -1;
    if (memcmp(buf, "RIFF", 4) != 0)   return -2;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return -3;

    const uint8_t* p   = buf + 12;
    const uint8_t* end = buf + len;
    bool got_fmt = false;
    memset(out, 0, sizeof *out);

    while (p + 8 <= end) {
        const uint8_t* id = p;
        uint32_t sz = rd_u32_le(p + 4);
        const uint8_t* body = p + 8;
        if (body + sz > end) return -4;

        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            uint16_t fmt = rd_u16_le(body + 0);
            uint16_t real_fmt = fmt;
            /* WAVE_FORMAT_EXTENSIBLE wraps the real format code in a GUID at
               body+24. First 16-bit field of that GUID is the actual format. */
            if (fmt == 0xFFFE && sz >= 40) {
                real_fmt = rd_u16_le(body + 24);
            }
            if (real_fmt != 1) return -5;     /* PCM only (no float, ADPCM, etc) */
            out->channels        = rd_u16_le(body + 2);
            out->sample_rate     = rd_u32_le(body + 4);
            out->bits_per_sample = rd_u16_le(body + 14);
            got_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!got_fmt) return -6;
            out->pcm      = body;
            out->pcm_size = sz;
            return 0;
        }
        /* Chunks are word-aligned; pad if size is odd */
        p = body + sz + (sz & 1);
    }
    return -7;
}

/* Convert a window of source PCM (8-bit unsigned or 16-bit signed) into the
   8-bit unsigned format SB16 wants. Returns the number of source frames
   consumed and writes 'frames' bytes into dst. Mono and stereo handled. */
static uint32_t resample_chunk(const wav_info_t* w,
                                uint32_t src_offset_frames,
                                uint8_t* dst,
                                uint32_t dst_max_bytes,
                                uint32_t* frames_out) {
    uint32_t bps   = w->bits_per_sample / 8;
    uint32_t fsize = bps * w->channels;
    uint32_t total_frames = w->pcm_size / fsize;
    if (src_offset_frames >= total_frames) { *frames_out = 0; return 0; }

    uint32_t remain = total_frames - src_offset_frames;
    uint32_t n = (remain < dst_max_bytes) ? remain : dst_max_bytes;

    const uint8_t* src = w->pcm + src_offset_frames * fsize;
    if (w->bits_per_sample == 8) {
        if (w->channels == 1) {
            memcpy(dst, src, n);
        } else {
            /* downmix 8u stereo → 8u mono (average) */
            for (uint32_t i = 0; i < n; i++) {
                uint16_t s = (uint16_t)src[i*2] + (uint16_t)src[i*2+1];
                dst[i] = (uint8_t)(s >> 1);
            }
        }
    } else {
        /* 16-bit signed → 8-bit unsigned */
        if (w->channels == 1) {
            const int16_t* s16 = (const int16_t*)src;
            for (uint32_t i = 0; i < n; i++) {
                int v = s16[i];
                dst[i] = (uint8_t)((v >> 8) + 128);
            }
        } else {
            const int16_t* s16 = (const int16_t*)src;
            for (uint32_t i = 0; i < n; i++) {
                int v = ((int)s16[i*2] + (int)s16[i*2+1]) / 2;
                dst[i] = (uint8_t)((v >> 8) + 128);
            }
        }
    }
    *frames_out = n;
    return n;
}

int wav_play(const uint8_t* buf, uint32_t len) {
    wav_info_t w;
    int r = wav_parse(buf, len, &w);
    if (r != 0) return r;
    if (!sb16_present()) return -10;

    /* SB16 driver chunk limit. Smaller actual chunks reduce hiccups. */
    enum { CHUNK = SB16_MAX_CHUNK / 2 };
    static uint8_t conv[CHUNK];

    uint32_t bps   = w.bits_per_sample / 8;
    uint32_t fsize = bps * w.channels;
    if (fsize == 0) return -11;
    uint32_t total_frames = w.pcm_size / fsize;

    uint32_t off = 0;
    while (off < total_frames) {
        uint32_t taken = 0;
        resample_chunk(&w, off, conv, CHUNK, &taken);
        if (taken == 0) break;
        if (sb16_play_pcm_u8_mono(conv, taken, w.sample_rate) != 0) return -12;
        off += taken;
        /* wait for chunk to finish, allowing Ctrl-C / Esc to abort */
        while (sb16_busy()) {
            char c = kbd_trygetc();
            if (c == 3 || c == 0x1B) { sb16_stop(); return 0; }
            __asm__ volatile ("hlt");
        }
    }
    return 0;
}
