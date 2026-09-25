/* SoundBlaster 16 driver — minimal but real PCM playback.
   Tested target: QEMU `-device sb16`. Defaults: base 0x220, IRQ 5, DMA1/DMA5.
   This driver uses single-cycle 8-bit DMA on channel 1 (legacy DSP cmd 0x14),
   which works for SB / SB Pro / SB16 alike. Buffer must live below 16 MiB and
   not cross a 64 KiB boundary, so we allocate a 64 KiB-aligned bounce buffer
   in .bss and memcpy caller PCM into it before kicking DMA. */

#include "drivers/sb16.h"
#include "core/io.h"
#include "core/string.h"
#include "boot/pit.h"

#define SB_BASE         0x220
#define SB_MIXER_ADDR   (SB_BASE + 0x04)
#define SB_MIXER_DATA   (SB_BASE + 0x05)
#define SB_RESET        (SB_BASE + 0x06)
#define SB_READ_DATA    (SB_BASE + 0x0A)
#define SB_WRITE        (SB_BASE + 0x0C)
#define SB_READ_STATUS  (SB_BASE + 0x0E)
#define SB_INT_ACK_8    (SB_BASE + 0x0E)
#define SB_INT_ACK_16   (SB_BASE + 0x0F)

/* 64 KiB-aligned 32 KiB bounce buffer. Lives entirely inside one ISA DMA page
   (it is at most 32 KiB so it can't cross the 64 KiB boundary once aligned). */
__attribute__((aligned(65536)))
static uint8_t sb_buf[32768];

static bool   g_present = false;
static const char* g_status = "not initialised";
static uint32_t g_busy_until_ms = 0;
static uint8_t  g_dsp_ver_hi = 0, g_dsp_ver_lo = 0;

/* ---- low-level DSP I/O ---- */

static void io_delay_us(int us) {
    /* PIT is 100 Hz (10 ms tick), too coarse. Use ports 0x80 as ~1us each */
    for (int i = 0; i < us; i++) (void)inb(0x80);
}

static void dsp_write(uint8_t v) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(SB_WRITE) & 0x80) == 0) { outb(SB_WRITE, v); return; }
    }
    /* gave up — driver likely stuck */
}

static int dsp_read_timeout(int us, uint8_t* out) {
    for (int i = 0; i < us; i++) {
        if (inb(SB_READ_STATUS) & 0x80) { *out = inb(SB_READ_DATA); return 0; }
        (void)inb(0x80);
    }
    return -1;
}

static bool dsp_reset(void) {
    outb(SB_RESET, 1);
    io_delay_us(3);
    outb(SB_RESET, 0);
    /* DSP should return 0xAA on read-data within ~100us */
    for (int i = 0; i < 1000; i++) {
        if (inb(SB_READ_STATUS) & 0x80) {
            return inb(SB_READ_DATA) == 0xAA;
        }
        io_delay_us(2);
    }
    return false;
}

static void mixer_write(uint8_t reg, uint8_t val) {
    outb(SB_MIXER_ADDR, reg);
    outb(SB_MIXER_DATA, val);
}

/* ---- ISA DMA controller 1 (channels 0..3, 8-bit) ---- */

/* Channel 1 page register address */
#define DMA1_PAGE_CH1   0x83

static void dma1_setup_ch1(uint32_t phys, uint16_t bytes) {
    uint8_t  mode = 0x49;            /* single-cycle read, no autoinit, ch 1
                                        bits: [7:6]=01 single, [5]=0 inc, [4]=0 no-auto
                                              [3:2]=10 read, [1:0]=01 ch1 */
    uint16_t offs = phys & 0xFFFF;
    uint8_t  page = (phys >> 16) & 0xFF;
    uint16_t cnt  = bytes - 1;

    outb(0x0A, 0x05);            /* mask channel 1 */
    outb(0x0C, 0x00);            /* clear byte-pointer flip-flop */
    outb(0x0B, mode);            /* mode register */
    outb(0x02, offs & 0xFF);     /* base address low */
    outb(0x02, (offs >> 8) & 0xFF);
    outb(DMA1_PAGE_CH1, page);   /* page */
    outb(0x03, cnt & 0xFF);      /* count low */
    outb(0x03, (cnt >> 8) & 0xFF);
    outb(0x0A, 0x01);            /* unmask channel 1 */
}

/* ---- public API ---- */

bool sb16_present(void) { return g_present; }
const char* sb16_status(void) { return g_status; }

bool sb16_init(void) {
    if (!dsp_reset()) {
        g_status = "DSP reset failed (no SB at 0x220)";
        g_present = false;
        return false;
    }
    /* Identify DSP version (cmd 0xE1) — useful for status text */
    dsp_write(0xE1);
    if (dsp_read_timeout(20000, &g_dsp_ver_hi) == 0)
        dsp_read_timeout(20000, &g_dsp_ver_lo);

    /* Route IRQ to 5, DMA8=1, DMA16=5 (matches QEMU defaults). The mixer
       registers are SB16-only; on older clones the writes are no-ops. */
    mixer_write(0x80, 0x02);   /* IRQ select = IRQ 5 */
    mixer_write(0x81, 0x22);   /* DMA1 + DMA5 */
    /* Master volume to max */
    mixer_write(0x22, 0xFF);
    mixer_write(0x04, 0xFF);   /* voice */
    mixer_write(0x26, 0xFF);   /* FM */

    g_present = true;
    g_status  = "ok";
    return true;
}

bool sb16_busy(void) {
    return g_present && (int32_t)(pit_uptime_ms() - g_busy_until_ms) < 0;
}

void sb16_stop(void) {
    if (!g_present) return;
    dsp_write(0xD0);          /* pause 8-bit DMA */
    dsp_write(0xDA);          /* exit auto-init 8-bit DMA */
    dsp_write(0xD3);          /* speaker off */
    g_busy_until_ms = pit_uptime_ms();
}

int sb16_play_pcm_u8_mono(const uint8_t* pcm, uint32_t len, uint32_t hz) {
    if (!g_present) return -1;
    if (!pcm || !len) return -2;
    bool sb16_dsp = (g_dsp_ver_hi >= 4);
    uint32_t max_hz = sb16_dsp ? 44100u : 23000u;     /* DSP 4.x supports 44.1 kHz natively */
    if (hz < 4000)    hz = 4000;
    if (hz > max_hz)  hz = max_hz;
    if (len > sizeof sb_buf) len = sizeof sb_buf;

    /* Copy caller's PCM into the DMA-safe bounce buffer */
    memcpy(sb_buf, pcm, len);

    uint32_t phys = (uint32_t)sb_buf;
    dma1_setup_ch1(phys, (uint16_t)len);

    /* DSP: speaker on, set sample rate */
    dsp_write(0xD1);                                  /* turn speaker on */
    if (sb16_dsp) {
        dsp_write(0x41);                              /* set output sample rate */
        dsp_write((uint8_t)((hz >> 8) & 0xFF));
        dsp_write((uint8_t)(hz & 0xFF));
    } else {
        uint8_t tc = (uint8_t)(256 - (1000000u / hz));
        dsp_write(0x40);                              /* legacy time-constant */
        dsp_write(tc);
    }

    uint16_t cnt = (uint16_t)(len - 1);
    dsp_write(0x14);                                  /* single-cycle 8-bit DMA */
    dsp_write((uint8_t)(cnt & 0xFF));
    dsp_write((uint8_t)((cnt >> 8) & 0xFF));

    uint32_t dur_ms = (uint32_t)((uint64_t)len * 1000u / hz);
    g_busy_until_ms = pit_uptime_ms() + dur_ms + 50;
    return 0;
}
