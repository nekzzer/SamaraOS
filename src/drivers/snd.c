#include "drivers/snd.h"
#include "drivers/hda.h"
#include "core/heap.h"
#include "core/vmm.h"
#include "core/io.h"
#include "fs/fs.h"
#include "core/string.h"
#include "core/task.h"
#include "core/smp.h"
#include "core/wq.h"
#include "boot/pit.h"
#include "proc/proc.h"

#define EPERM_ 1
#define ENOMEM 12
#define EINTR  4
#define EAGAIN 11
#define EBUSY  16
#define EINVAL 22
#define ENOTTY 25
#define EPIPE  32
#define ENOSYS 38
#define EBADFD 77

enum { S_OPEN, S_SETUP, S_PREP, S_RUN, S_XRUN, S_DRAIN, S_PAUSE };

struct snd_fd { int pcm; };

typedef struct { uint32_t bits[8]; } mask_t;
typedef struct { uint32_t min, max; uint32_t fl; } iv_t;   // fl: 1 openmin, 2 openmax, 4 integer, 8 empty
typedef struct {
    uint32_t flags;
    mask_t masks[3], mres[5];
    iv_t iv[12], ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
    uint64_t fifo;
    uint8_t res[64];
} hwp_t;

typedef struct {
    int32_t tstamp_mode;
    uint32_t period_step, sleep_min;
    uint64_t avail_min, xfer_align, start, stop, silence_th, silence_size, boundary;
    uint32_t proto, tstamp_type;
    uint8_t res[56];
} swp_t;

typedef struct { int64_t s, ns; } ts_t;

typedef struct {
    int32_t state, pad;
    ts_t trig, tstamp;
    uint64_t appl, hw;
    int64_t delay;
    uint64_t avail, avail_max, overrange;
    int32_t susp;
    uint32_t atd;
    ts_t atstamp, dtstamp;
    uint32_t acc;
    uint8_t res[20];
} status_t;

typedef struct { uint32_t flags; uint32_t pad; uint8_t s[64]; uint8_t c[64]; } sync_t;

typedef struct { int64_t result; void* buf; uint64_t frames; } xferi_t;

typedef struct { uint32_t numid; int32_t iface; uint32_t dev, sub; uint8_t name[44]; uint32_t idx; } eid_t;
typedef struct { uint32_t off, space, used, count; eid_t* ids; uint8_t res[50]; } elist_t;
typedef struct {
    eid_t id; int32_t type; uint32_t access, count; int32_t owner;
    int64_t min, max, step;
    uint8_t res[272 - 64 - 16 - 24];
} einfo_t;
typedef struct { eid_t id; uint32_t indirect; uint32_t pad; int64_t v[128]; uint8_t res[128]; } eval_t;

static bool have;
static spin_t lk;
static wq_t wq;
static int open_pcm;       // somebody has the playback node

// stream state
static int state;
static uint32_t rate, bufb, perb, bfr, pfr;     // bytes and frames
static uint64_t appl, hw, boundary;
static bool mmapd;
static uint64_t avail_min, start_th, stop_th;
static uint32_t last_lpib;
static uint32_t drain_at;
static bool kern;

static void hw_update(void) {
    if (state != S_RUN && state != S_DRAIN) return;
    uint32_t l = hda_lpib();
    if (l >= bufb) l = 0;
    uint32_t d = (l + bufb - last_lpib) % bufb;
    last_lpib = l;
    hw += d / 4;
    if (hw < appl) return;
    if (state == S_RUN) { hda_run(false); state = S_XRUN; return; }
    if (!drain_at) drain_at = pit_uptime_ms() + 30;       // fifo + qemu still hold some
    if ((int32_t)(pit_uptime_ms() - drain_at) >= 0) { hda_run(false); state = S_SETUP; }
}

// mediaplayer tones: square wave pushed into the ring by the snd task, alsa owns the stream when it is open
static void do_prepare(void);
static void do_start(void);
static uint32_t tone_f, tone_ph, tone_quiet;
static bool tone_run;

static void tone_put(uint32_t n) {
    int16_t* r = (int16_t*)hda_ring();
    uint32_t step = tone_f ? (uint32_t)(((uint64_t)tone_f << 32) / rate) : 0;
    while (n--) {
        int16_t v = !tone_f ? 0 : (tone_ph & 0x80000000u) ? -5000 : 5000;
        tone_ph += step;
        uint64_t p = appl % bfr;
        r[p * 2] = r[p * 2 + 1] = v;
        appl++;
    }
}

static void tone_tick(void) {
    if (!tone_run) return;
    if (state == S_XRUN) { do_prepare(); tone_put(bfr / 2); do_start(); }
    while (appl - hw < bfr * 3 / 4) tone_put(128);
    if (tone_f) tone_quiet = 0;
    else if (!tone_quiet) tone_quiet = pit_uptime_ms() | 1;
    else if ((int32_t)(pit_uptime_ms() - tone_quiet) > 400) { hda_run(false); state = S_OPEN; tone_run = false; }
}

bool snd_tone(uint32_t f) {
    if (!have) return false;
    uint64_t fl = spin_lock(&lk);
    if (open_pcm || (state != S_OPEN && !tone_run)) { spin_unlock(&lk, fl); return false; }
    tone_f = f;
    if (f && !tone_run) {
        tone_run = true;
        rate = 48000;
        pfr = 512; bfr = 2048; perb = 2048; bufb = 8192;
        do_prepare();
        tone_put(bfr / 2);
        do_start();
    }
    spin_unlock(&lk, fl);
    return true;
}

static void snd_task(void) {
    for (;;) {
        task_sleep_ms(state == S_RUN || state == S_DRAIN ? 4 : 40);
        uint64_t f = spin_lock(&lk);
        hw_update();
        tone_tick();
        spin_unlock(&lk, f);
        wq_wake(&wq);
    }
}

bool snd_present(void) { return have; }

struct snd_fd* snd_open(int pcm) {
    static struct snd_fd fds[2];
    if (!have) return NULL;
    if (pcm) {
        if (open_pcm) return NULL;
        uint64_t f = spin_lock(&lk);
        if (tone_run) { hda_run(false); tone_run = false; }
        spin_unlock(&lk, f);
        open_pcm = 1;
        mmapd = false;
        state = S_OPEN;
        rate = 0;
    }
    fds[pcm].pcm = pcm;
    return &fds[pcm];
}

void snd_close(struct snd_fd* s) {
    if (!s->pcm) return;
    uint64_t f = spin_lock(&lk);
    hda_run(false);
    state = S_OPEN;
    open_pcm = 0;
    spin_unlock(&lk, f);
}

static void set_str(uint8_t* d, const char* s) { strcpy((char*)d, s); }

static int ctl_ioctl(uint32_t nr, void* arg) {
    switch (nr) {
    case 0: *(int*)arg = (2 << 16) | (0 << 8) | 9; return 0;
    case 1: {
        uint8_t* c = arg;
        memset(c, 0, 376);
        set_str(c + 8, "SamaraHDA");
        set_str(c + 24, "HDA-Intel");
        set_str(c + 40, "Samara HDA");
        set_str(c + 72, "Samara Intel HDA");
        set_str(c + 168, "HDA codec");
        return 0;
    }
    case 0x10: {
        elist_t* l = arg;
        static const char* nm[2] = { "Master Playback Volume", "Master Playback Switch" };
        l->count = 2;
        l->used = 0;
        for (uint32_t i = l->off; i < 2 && l->used < l->space; i++) {
            eid_t* e = &l->ids[l->used++];
            memset(e, 0, sizeof *e);
            e->numid = i + 1;
            e->iface = 2;
            set_str(e->name, nm[i]);
        }
        return 0;
    }
    case 0x11: {
        einfo_t* e = arg;
        uint32_t n = e->id.numid;
        if (!n) n = !strcmp((char*)e->id.name, "Master Playback Switch") ? 2 : 1;
        if (n != 1 && n != 2) return -2;
        eid_t id = e->id;
        memset(e, 0, sizeof *e);
        e->id = id;
        e->id.numid = n;
        e->id.iface = 2;
        set_str(e->id.name, n == 1 ? "Master Playback Volume" : "Master Playback Switch");
        e->type = n == 1 ? 2 : 1;
        e->access = 3;
        e->count = 2;
        if (n == 1) { e->min = 0; e->max = hda_vol_max(); e->step = 1; }
        return 0;
    }
    case 0x12: case 0x13: {
        eval_t* v = arg;
        uint32_t n = v->id.numid;
        if (!n) n = !strcmp((char*)v->id.name, "Master Playback Switch") ? 2 : 1;
        if (n != 1 && n != 2) return -2;
        int l, r, sw;
        hda_vol_get(&l, &r, &sw);
        if (nr == 0x12) {
            v->v[0] = n == 1 ? l : sw;
            v->v[1] = n == 1 ? r : sw;
            return 0;
        }
        if (n == 1) hda_vol_set((int)v->v[0], (int)v->v[1], sw);
        else hda_vol_set(l, r, v->v[0] || v->v[1]);
        return 1;
    }
    case 0x14: case 0x15: case 0x16: return 0;
    case 0x30: { int* d = arg; *d = *d < 0 ? 0 : -1; return 0; }
    case 0x31: {
        uint8_t* p = arg;
        uint32_t dev = *(uint32_t*)p, sub = *(uint32_t*)(p + 4);
        int stream = *(int*)(p + 8);
        if (dev != 0 || sub != 0 || stream != 0) return -2;
        memset(p + 16, 0, 288 - 16);
        set_str(p + 16, "HDA Codec");
        set_str(p + 80, "HDA Codec");
        set_str(p + 160, "subdevice #0");
        *(uint32_t*)(p + 200) = 1;
        *(uint32_t*)(p + 204) = open_pcm ? 0 : 1;
        return 0;
    }
    case 0x32: return 0;
    }
    return -ENOTTY;
}

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

// intervals: 0 sbits 1 fbits 2 ch 3 rate 4 ptime 5 psize 6 pbytes 7 periods 8 btime 9 bsize 10 bbytes 11 tick
enum { I_SB, I_FB, I_CH, I_RATE, I_PT, I_PS, I_PB, I_PN, I_BT, I_BS, I_BB, I_TICK };

static int iv_norm(iv_t* v) {
    if (v->fl & 8) return -EINVAL;
    if (v->fl & 1) v->min++;
    if (v->fl & 2) { if (v->max == 0) return -EINVAL; v->max--; }
    v->fl = 4;
    return v->min > v->max ? -EINVAL : 0;
}

static void iv_set(iv_t* v, uint32_t lo, uint32_t hi) { v->min = lo; v->max = hi; v->fl = 4; }

static void iv_and(iv_t* v, uint64_t lo, uint64_t hi) {
    if (lo > 0xFFFFFFFFull) lo = 0xFFFFFFFFull;
    if (hi > 0xFFFFFFFFull) hi = 0xFFFFFFFFull;
    if (lo > v->min) v->min = (uint32_t)lo;
    if (hi < v->max) v->max = (uint32_t)hi;
}

static int hw_refine(hwp_t* p) {
    hwp_t old = *p;
    p->masks[0].bits[0] &= (1 << 3) | 1;       // rw + mmap interleaved
    p->masks[1].bits[0] &= 1 << 2;       // s16le
    p->masks[2].bits[0] &= 1;
    if (!p->masks[0].bits[0] || !p->masks[1].bits[0] || !p->masks[2].bits[0]) return -EINVAL;
    uint32_t tfl[2] = {0, 0};
    for (int i = 0; i < 12; i++) {
        if (i == I_PT || i == I_BT) { tfl[i == I_BT] = p->iv[i].fl; p->iv[i].fl = 0; continue; }   // time is not integer, keep raw bounds
        if (iv_norm(&p->iv[i])) return -EINVAL;
    }
    iv_t* v = p->iv;
    iv_and(&v[I_SB], 16, 16); iv_and(&v[I_FB], 32, 32); iv_and(&v[I_CH], 2, 2);
    iv_and(&v[I_TICK], 0, 0xFFFFFFFF);
    uint32_t lo = v[I_RATE].min <= 44100 ? 44100 : v[I_RATE].min <= 48000 ? 48000 : 0;
    uint32_t hi = v[I_RATE].max >= 48000 ? 48000 : v[I_RATE].max >= 44100 ? 44100 : 0;
    if (!lo || !hi || lo > hi) return -EINVAL;
    v[I_RATE].min = lo; v[I_RATE].max = hi;
    iv_and(&v[I_PS], 256, 8192);
    iv_and(&v[I_PN], 2, 32);
    iv_and(&v[I_BS], 1024, HDA_BUF / 4);
    for (int k = 0; k < 4; k++) {
        uint64_t r0 = v[I_RATE].min, r1 = v[I_RATE].max;
        iv_and(&v[I_PS], (v[I_PB].min + 3) / 4, v[I_PB].max / 4);
        iv_and(&v[I_BS], (v[I_BB].min + 3) / 4, v[I_BB].max / 4);
        for (int k = 0; k < 2; k++) {
            iv_t* t = &v[k ? I_BT : I_PT];
            uint64_t lo = (uint64_t)t->min * r0, hi = (uint64_t)t->max * r1;
            lo = (tfl[k] & 1) ? lo / 1000000 + 1 : (lo + 999999) / 1000000;
            hi = (tfl[k] & 2) ? (hi + 999999) / 1000000 - 1 : hi / 1000000;
            if (hi > 0xFFFFFFF0ull && t->max == 0xFFFFFFFFu) hi = 0xFFFFFFFFull;
            iv_and(k ? &v[I_BS] : &v[I_PS], lo, hi);
        }
        iv_and(&v[I_PS], ((uint64_t)v[I_BS].min + v[I_PN].max - 1) / v[I_PN].max, v[I_BS].max / v[I_PN].min);
        iv_and(&v[I_PN], ((uint64_t)v[I_BS].min + v[I_PS].max - 1) / v[I_PS].max, v[I_BS].max / v[I_PS].min);
        iv_and(&v[I_BS], (uint64_t)v[I_PS].min * v[I_PN].min, (uint64_t)v[I_PS].max * v[I_PN].max);
        v[I_PS].min = (v[I_PS].min + 31) & ~31u;
        v[I_PS].max &= ~31u;
        v[I_BS].min = (v[I_BS].min + 31) & ~31u;
        v[I_BS].max &= ~31u;
        for (int i = 0; i < 12; i++) if (v[i].min > v[i].max) return -EINVAL;
        iv_set(&v[I_PB], v[I_PS].min * 4, v[I_PS].max * 4);
        iv_set(&v[I_BB], v[I_BS].min * 4, v[I_BS].max * 4);
        iv_and(&v[I_PT], (uint64_t)v[I_PS].min * 1000000 / r1, ((uint64_t)v[I_PS].max * 1000000 + r0 - 1) / r0);
        iv_and(&v[I_BT], (uint64_t)v[I_BS].min * 1000000 / r1, ((uint64_t)v[I_BS].max * 1000000 + r0 - 1) / r0);
    }
    for (int i = 0; i < 12; i++) if (v[i].min > v[i].max) return -EINVAL;
    // time is ps * 1e6 / rate, alsa-lib wants the open bounds like the real kernel gives
    {
        uint64_t r0 = v[I_RATE].min, r1 = v[I_RATE].max;
        iv_t* t = &v[I_PT];
        for (int k = 0; k < 2; k++, t = &v[I_BT]) {
            uint64_t a = (uint64_t)(k ? v[I_BS].min : v[I_PS].min) * 1000000, b = (uint64_t)(k ? v[I_BS].max : v[I_PS].max) * 1000000;
            uint32_t fl = 0;
            if (a % r1) fl |= 1;
            if (b % r0) fl |= 2;
            if (a / r1 > t->min) t->min = a / r1;
            if ((b + r0 - 1) / r0 < t->max) t->max = (b + r0 - 1) / r0;
            t->fl = fl;
            if (t->min > t->max || (t->min == t->max && fl)) return -EINVAL;
        }
    }
    p->rate_num = v[I_RATE].min; p->rate_den = 1;
    p->msbits = 16;
    p->info = 0x100 | 0x10000 | 0x80000;
    p->cmask = 0;
    for (int i = 0; i < 3; i++) if (memcmp(&old.masks[i], &p->masks[i], sizeof(mask_t))) p->cmask |= 1u << i;
    for (int i = 0; i < 12; i++) if (old.iv[i].min != p->iv[i].min || old.iv[i].max != p->iv[i].max) p->cmask |= 1u << (8 + i);
    return 0;
}

static int hw_params(hwp_t* p) {
    int r = hw_refine(p);
    if (r) return r;
    iv_t* v = p->iv;
    iv_set(&v[I_RATE], v[I_RATE].min, v[I_RATE].min);
    r = hw_refine(p);
    if (r) return r;
    uint32_t ps = (clampu(1024, v[I_PS].min, v[I_PS].max) + 31) & ~31u;
    if (ps > v[I_PS].max) ps = v[I_PS].max;
    iv_set(&v[I_PS], ps, ps);
    r = hw_refine(p);
    if (r) return r;
    uint32_t bs = clampu(ps * 4, v[I_BS].min, v[I_BS].max);
    bs = bs / ps * ps;
    if (bs < ps * 2) bs = ps * 2;
    if (bs > v[I_BS].max || bs / ps > 32) return -EINVAL;
    iv_set(&v[I_BS], bs, bs);
    r = hw_refine(p);
    if (r) return r;
    rate = v[I_RATE].min;
    pfr = v[I_PS].min; bfr = v[I_BS].min;
    perb = pfr * 4; bufb = bfr * 4;
    return 0;
}

static void do_prepare(void) {
    hda_run(false);
    hda_setup(rate, bufb, perb);
    appl = hw = 0;
    last_lpib = 0;
    drain_at = 0;
    state = S_PREP;
    boundary = bfr;
    while (boundary * 2 <= 0x7FFFFFFFFFFFFFFFull - bfr) boundary *= 2;
}

static void do_start(void) {
    last_lpib = hda_lpib();
    hda_run(true);
    state = S_RUN;
}

static int wait_room(bool nb) {
    if (nb) return -EAGAIN;
    if (proc_interrupted()) return -EINTR;
    if (kern) cpu_wait();
    else { WQ_W(w); wq_wait(&wq, &w, 10); }
    return 0;
}

static int64_t pcm_write(const uint8_t* src, uint64_t n, bool nb) {
    int64_t done = 0;
    while (n) {
        uint64_t f = spin_lock(&lk);
        hw_update();
        int st = state;
        uint64_t av = bfr - (appl - hw);
        spin_unlock(&lk, f);
        if (st == S_XRUN) return done ? done : -EPIPE;
        if (st != S_PREP && st != S_RUN && st != S_PAUSE) return done ? done : -EBADFD;
        if (!av) {
            if (st == S_PREP) { f = spin_lock(&lk); do_start(); spin_unlock(&lk, f); continue; }
            if (done && nb) return done;
            int r = wait_room(nb);
            if (r) return done ? done : r;
            continue;
        }
        uint64_t k = n < av ? n : av;
        uint64_t pos = appl % bfr;
        if (k > bfr - pos) k = bfr - pos;
        memcpy(hda_ring() + pos * 4, src, k * 4);
        f = spin_lock(&lk);
        appl += k;
        if (state == S_PREP && appl >= (start_th < bfr ? start_th : bfr)) do_start();
        spin_unlock(&lk, f);
        src += k * 4;
        n -= k;
        done += k;
    }
    return done;
}

static int pcm_drain(bool nb) {
    uint64_t f = spin_lock(&lk);
    hw_update();
    if (state == S_PREP && appl) do_start();
    if (state == S_PREP) state = S_SETUP;
    if (state == S_RUN) state = S_DRAIN;
    int st = state;
    spin_unlock(&lk, f);
    if (st == S_XRUN) return -EPIPE;
    if (st == S_PAUSE) return -EBADFD;
    if (nb && st == S_DRAIN) return -EAGAIN;
    while (st == S_DRAIN) {
        if (proc_interrupted()) return -EINTR;
        if (kern) cpu_wait();
        else { WQ_W(w); wq_wait(&wq, &w, 10); }
        f = spin_lock(&lk);
        hw_update();
        st = state;
        spin_unlock(&lk, f);
    }
    return 0;
}

bool snd_writable(struct snd_fd* s) {
    if (!s->pcm) return false;
    uint64_t f = spin_lock(&lk);
    hw_update();
    uint64_t av = bfr - (appl - hw);
    bool r = state == S_XRUN || (state != S_RUN && state != S_PREP && state != S_DRAIN) || av >= (avail_min ? avail_min : 1);
    if (state == S_DRAIN) r = false;
    spin_unlock(&lk, f);
    return r;
}

static int pcm_ioctl(uint32_t nr, void* arg, bool nb) {
    int r = 0;
    uint64_t f;
    switch (nr) {
    case 0: *(int*)arg = (2 << 16) | (0 << 8) | 18; return 0;
    case 1: {
        uint8_t* p = arg;
        memset(p, 0, 288);
        set_str(p + 16, "HDA Codec");
        set_str(p + 80, "HDA Codec");
        set_str(p + 160, "subdevice #0");
        *(uint32_t*)(p + 200) = 1;
        return 0;
    }
    case 2: case 3: case 4: return 0;
    case 0x10: return hw_refine(arg);
    case 0x11:
        f = spin_lock(&lk);
        if (state == S_RUN || state == S_DRAIN || state == S_PAUSE) r = -EBADFD;
        spin_unlock(&lk, f);
        if (r) return r;
        r = hw_params(arg);
        if (r) return r;
        f = spin_lock(&lk);
        hda_run(false);
        state = S_SETUP;
        avail_min = pfr; start_th = 1; stop_th = bfr;
        boundary = bfr;
        while (boundary * 2 <= 0x7FFFFFFFFFFFFFFFull - bfr) boundary *= 2;
        spin_unlock(&lk, f);
        return 0;
    case 0x12:
        f = spin_lock(&lk);
        hda_run(false);
        state = S_OPEN;
        spin_unlock(&lk, f);
        return 0;
    case 0x13: {
        swp_t* s = arg;
        f = spin_lock(&lk);
        avail_min = s->avail_min ? s->avail_min : 1;
        start_th = s->start; stop_th = s->stop;
        s->boundary = boundary;
        spin_unlock(&lk, f);
        return 0;
    }
    case 0x20: case 0x24: {
        status_t* st = arg;
        f = spin_lock(&lk);
        hw_update();
        memset(st, 0, sizeof *st);
        st->state = state;
        st->appl = appl % (boundary ? boundary : 1ull << 62);
        st->hw = hw % (boundary ? boundary : 1ull << 62);
        st->delay = (state == S_RUN || state == S_DRAIN) ? (int64_t)(appl - hw) : 0;
        st->avail = st->avail_max = bfr - (appl - hw);
        spin_unlock(&lk, f);
        return 0;
    }
    case 0x21:
        f = spin_lock(&lk);
        hw_update();
        *(int64_t*)arg = (state == S_RUN || state == S_DRAIN) ? (int64_t)(appl - hw) : 0;
        spin_unlock(&lk, f);
        return 0;
    case 0x22: return 0;
    case 0x23: {
        sync_t* y = arg;
        f = spin_lock(&lk);
        hw_update();
        if (!(y->flags & 2)) {     // flag set = kernel to user, backwards but thats how it is
            uint64_t a = *(uint64_t*)y->c;
            // only believe it when the app mmaped the ring, with write() alsa-lib sends stale appl_ptr
            if (mmapd) {
                uint64_t bn0 = boundary ? boundary : 1ull << 62;
                uint64_t d = (a + bn0 - appl % bn0) % bn0;
                if (d <= bfr - (appl - hw)) {
                    appl += d;
                    if (state == S_PREP && appl >= (start_th < bfr ? start_th : bfr)) do_start();
                }
            }
        }
        if (!(y->flags & 4)) { avail_min = *(uint64_t*)(y->c + 8); if (!avail_min) avail_min = 1; }
        uint64_t bn = boundary ? boundary : 1ull << 62;
        memset(y->s, 0, 64);
        *(int32_t*)y->s = state;
        *(uint64_t*)(y->s + 8) = hw % bn;
        *(uint64_t*)y->c = appl % bn;
        *(uint64_t*)(y->c + 8) = avail_min;
        spin_unlock(&lk, f);
        return 0;
    }
    case 0x32: {
        uint8_t* c = arg;
        uint32_t ch = *(uint32_t*)c;
        if (ch > 1) return -EINVAL;
        *(int64_t*)(c + 8) = 0;
        *(uint32_t*)(c + 16) = ch * 16;
        *(uint32_t*)(c + 20) = 32;
        return 0;
    }
    case 0x40:
        f = spin_lock(&lk);
        if (state == S_OPEN) r = -EBADFD;
        else if (state == S_RUN || state == S_DRAIN) r = -EBADFD;
        else do_prepare();
        spin_unlock(&lk, f);
        return r;
    case 0x41:
        f = spin_lock(&lk);
        if (state == S_RUN || state == S_DRAIN || state == S_PAUSE || state == S_PREP) do_prepare();
        spin_unlock(&lk, f);
        return 0;
    case 0x42:
        f = spin_lock(&lk);
        if (state == S_PREP) do_start();
        else if (state != S_RUN) r = -EBADFD;
        spin_unlock(&lk, f);
        return r;
    case 0x43:
        f = spin_lock(&lk);
        if (state == S_OPEN) r = -EBADFD;
        else { hda_run(false); state = S_SETUP; }
        spin_unlock(&lk, f);
        wq_wake(&wq);
        return r;
    case 0x44: return pcm_drain(nb);
    case 0x45:
        f = spin_lock(&lk);
        hw_update();
        if (*(int*)arg && state == S_RUN) { hda_run(false); state = S_PAUSE; }
        else if (!*(int*)arg && state == S_PAUSE) { last_lpib = hda_lpib(); hda_run(true); state = S_RUN; }
        else r = -EBADFD;
        spin_unlock(&lk, f);
        return r;
    case 0x47: return -ENOSYS;
    case 0x48:
        f = spin_lock(&lk);
        if (state == S_RUN) { hda_run(false); state = S_XRUN; }
        spin_unlock(&lk, f);
        return 0;
    case 0x50: {
        xferi_t* x = arg;
        int64_t n = pcm_write(x->buf, x->frames, nb);
        x->result = n;
        return n < 0 ? (int)n : 0;
    }
    case 0x51: return -ENOSYS;
    }
    return -ENOTTY;
}

int snd_mmap(struct snd_fd* s, uint64_t pd, uint64_t addr, uint64_t len, uint64_t off) {
    if (!s->pcm || off || len > HDA_BUF) return -EINVAL;     // status/control pages: alsa-lib falls back to sync_ptr
    uint64_t pa = V2P(hda_ring());
    for (uint64_t i = 0; i < len; i += PAGE_SIZE)
        if (vmm_map_frame(pd, addr + i, pa + i, true) < 0) return -ENOMEM;
    mmapd = true;
    return 0;
}

int snd_ioctl(struct snd_fd* s, uint32_t req, void* arg, bool nb) {
    uint32_t type = (req >> 8) & 255, nr = req & 255;
    int r = -ENOTTY;
    if (s->pcm && type == 'A') r = pcm_ioctl(nr, arg, nb);
    else if (type == 'U') r = ctl_ioctl(nr, arg);
    return r;
}

int snd_kopen(uint32_t hz) {
    if (!have || open_pcm) return -EBUSY;
    open_pcm = 1;
    kern = true;
    rate = hz == 44100 ? 44100 : 48000;
    pfr = 1024; bfr = 4096; perb = 4096; bufb = 16384;
    start_th = 2048; avail_min = pfr;
    uint64_t f = spin_lock(&lk);
    do_prepare();
    spin_unlock(&lk, f);
    return 0;
}

int snd_kwrite(const int16_t* pcm, uint32_t frames) {
    int64_t r = pcm_write((const uint8_t*)pcm, frames, false);
    return r < 0 ? (int)r : 0;
}

void snd_kclose(bool drain) {
    if (drain) pcm_drain(false);
    uint64_t f = spin_lock(&lk);
    hda_run(false);
    state = S_OPEN;
    spin_unlock(&lk, f);
    kern = false;
    open_pcm = 0;
}

static void com_s(const char* s) { while (*s) { while (!(inb(0x3F8 + 5) & 0x20)) {} outb(0x3F8, *s++); } }

void snd_init(void) {
    com_s("hda: "); com_s(hda_status()); com_s("\r\n");
    if (!hda_present()) return;
    fs_node_t* dev = fs_resolve(fs_root(), "/dev");
    if (!fs_resolve(fs_root(), "/dev/snd")) fs_create(dev, "snd", FS_DIR);
    dev = fs_resolve(fs_root(), "/dev/snd");
    fs_node_t* c = fs_create(dev, "controlC0", FS_FILE);
    if (c) { c->dev = FS_DEV_SNDC; c->mode = 0666; }
    c = fs_create(dev, "pcmC0D0p", FS_FILE);
    if (c) { c->dev = FS_DEV_SNDP; c->mode = 0666; }
    have = true;
    task_spawn("snd", snd_task);
}
