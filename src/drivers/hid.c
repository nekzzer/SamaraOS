#include "drivers/usb.h"
#include "drivers/input.h"
#include "core/heap.h"
#include "core/string.h"
#include "boot/pit.h"

int usb_ifaces(const uint8_t* d, int len, usb_if_t* out, int max) {
    int n = 0;
    usb_if_t* cur = 0;
    for (int i = 0; i + 1 < len && d[i]; i += d[i]) {
        if (d[i + 1] == 4 && n < max) {
            cur = &out[n++];
            memset(cur, 0, sizeof(*cur));
            cur->num = d[i + 2]; cur->cls = d[i + 5]; cur->sub = d[i + 6]; cur->proto = d[i + 7];
        } else if (d[i + 1] == 0x21 && cur && d[i] >= 9) {
            cur->rdlen = d[i + 7] | d[i + 8] << 8;
        } else if (d[i + 1] == 5 && cur && cur->nep < 4) {
            int k = cur->nep++;
            cur->ep[k].addr = d[i + 2]; cur->ep[k].attr = d[i + 3];
            cur->ep[k].mps = d[i + 4] | d[i + 5] << 8; cur->ep[k].interval = d[i + 6];
        }
    }
    return n;
}

// usage -> linux key code, usage 4 is 'a'
static const uint8_t ukey[102] = {
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26,
    27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
    72, 73, 82, 83, 86, 127,
};
static const uint8_t umod[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

typedef struct { uint8_t kind; uint16_t off, size; uint8_t sgn, rel, idx; int lo, hi; } fld_t;
// kind: 1 button, 2 x, 3 y, 4 wheel, 5 hwheel

struct hid {
    int id, kbd;
    uint8_t prev[8];
    uint16_t rep_code;
    uint32_t rep_at;
    int nf, has_id;
    fld_t f[24];
    uint32_t btn;
    int lastx, lasty;
};

static void parse(hid_t* h, const uint8_t* d, int len) {
    int page = 0, lmin = 0, lmax = 0, size = 0, cnt = 0, off = 0;
    int us[16], nus = 0, umin = 0, umax = 0;
    for (int i = 0; i < len; ) {
        uint8_t b = d[i];
        if (b == 0xFE) { i += 3 + (i + 1 < len ? d[i + 1] : 0); continue; }
        int sz = b & 3 ? 1 << ((b & 3) - 1) : 0, ty = b >> 2 & 3, tag = b >> 4;
        uint32_t v = 0;
        for (int k = 0; k < sz && i + 1 + k < len; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int)v;
        i += 1 + sz;
        if (ty == 1) {
            if (tag == 0) page = v;
            else if (tag == 1) lmin = sv;
            else if (tag == 2) lmax = sv;
            else if (tag == 7) size = v;
            else if (tag == 8) { h->has_id = 1; off = 0; }
            else if (tag == 9) cnt = v;
        } else if (ty == 2) {
            if (tag == 0 && nus < 16) us[nus++] = v;
            else if (tag == 1) umin = v;
            else if (tag == 2) umax = v;
        } else if (ty == 0) {
            if (tag == 8) {
                for (int k = 0; k < cnt && h->nf < 24; k++) {
                    int u = nus ? us[k < nus ? k : nus - 1] : umin + k;
                    if (!nus && umax && umin + k > umax) u = umax;
                    int kind = 0;
                    if (!(v & 1)) {
                        if (page == 9 && size == 1) kind = 1;
                        else if (page == 1 && u == 0x30) kind = 2;
                        else if (page == 1 && u == 0x31) kind = 3;
                        else if (page == 1 && u == 0x38) kind = 4;
                        else if (page == 12 && u == 0x238) kind = 5;
                    }
                    if (kind) {
                        fld_t* f = &h->f[h->nf++];
                        f->kind = kind; f->off = off + k * size + (h->has_id ? 8 : 0); f->size = size;
                        f->sgn = lmin < 0; f->rel = v & 4 ? 1 : 0; f->idx = k;
                        f->lo = lmin; f->hi = lmax;
                    }
                }
            }
            if (tag == 8) off += cnt * size;
            nus = 0; umin = umax = 0;
        }
    }
}

static int getbits(const uint8_t* b, int len, int off, int size, int sgn) {
    uint32_t v = 0;
    for (int k = 0; k < size && k < 32; k++) {
        int bit = off + k;
        if (bit / 8 >= len) break;
        if (b[bit / 8] >> (bit & 7) & 1) v |= 1u << k;
    }
    if (sgn && size < 32 && (v >> (size - 1) & 1)) v |= ~0u << size;
    return (int)v;
}

hid_t* hid_new(const char* name, uint16_t vid, uint16_t pid, int boot_kbd, const uint8_t* rd, int rlen) {
    hid_t* h = kmalloc(sizeof(*h));
    if (!h) return 0;
    memset(h, 0, sizeof(*h));
    input_dev_t d;
    memset(&d, 0, sizeof d);
    strncpy(d.name, name, 47);
    d.bus = 3; d.vid = vid; d.pid = pid; d.ver = 0x100;
    if (boot_kbd) {
        h->kbd = 1;
        d.ev = 0x120003;
        for (int c = 1; c < 0x80; c++) d.key[c / 8] |= 1 << (c & 7);
        for (int c = 96; c < 128; c++) d.key[c / 8] |= 1 << (c & 7);
    } else {
        parse(h, rd, rlen);
        if (!h->nf) { kfree(h); return 0; }
        d.ev = 3;
        int nbtn = 0;
        for (int i = 0; i < h->nf; i++) {
            fld_t* f = &h->f[i];
            if (f->kind == 1 && nbtn < 8) { int c = 0x110 + f->idx; d.key[c / 8] |= 1 << (c & 7); nbtn++; }
            else if (f->kind == 2 || f->kind == 3) {
                int a = f->kind - 2;
                if (f->rel) { d.rel |= 1 << a; d.ev |= 4; }
                else { d.abs |= 1 << a; d.ev |= 8; d.absmin[a] = f->lo; d.absmax[a] = f->hi; }
            } else if (f->kind == 4) { d.rel |= 1 << 8; d.ev |= 4; }
            else if (f->kind == 5) { d.rel |= 1 << 6; d.ev |= 4; }
        }
    }
    h->id = input_register(&d);
    if (h->id < 0) { kfree(h); return 0; }
    return h;
}

void hid_free(hid_t* h) {
    if (!h) return;
    input_unregister(h->id);
    kfree(h);
}

static void kbd_rep(hid_t* h, const uint8_t* b, int len) {
    if (len < 8) return;
    int ch = 0;
    for (int i = 0; i < 8; i++) {
        int a = b[0] >> i & 1, o = h->prev[0] >> i & 1;
        if (a != o) { input_report(h->id, IEV_KEY, umod[i], a); ch = 1; }
    }
    if (b[2] == 1) { memcpy(h->prev, b, 8); return; }          /* rollover error */
    for (int i = 2; i < 8; i++) {                              /* released */
        if (!h->prev[i]) continue;
        int still = 0;
        for (int j = 2; j < 8; j++) if (b[j] == h->prev[i]) still = 1;
        if (still) continue;
        if (h->prev[i] < 102 && ukey[h->prev[i]]) { input_report(h->id, IEV_KEY, ukey[h->prev[i]], 0); ch = 1; }
        if (h->prev[i] < 102 && h->rep_code == ukey[h->prev[i]]) h->rep_code = 0;
    }
    for (int i = 2; i < 8; i++) {                              /* pressed */
        if (!b[i]) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (h->prev[j] == b[i]) was = 1;
        if (was) continue;
        if (b[i] < 102 && ukey[b[i]]) {
            input_report(h->id, IEV_KEY, ukey[b[i]], 1);
            h->rep_code = ukey[b[i]];
            h->rep_at = pit_uptime_ms() + 500;
            ch = 1;
        }
    }
    memcpy(h->prev, b, 8);
    if (ch) input_report(h->id, IEV_SYN, 0, 0);
}

void hid_tick(hid_t* h) {
    if (!h || !h->kbd || !h->rep_code) return;
    if ((int32_t)(pit_uptime_ms() - h->rep_at) < 0) return;
    h->rep_at += 33;
    input_repeat(h->rep_code);
}

void hid_report(hid_t* h, const uint8_t* b, int len) {
    if (!h) return;
    if (h->kbd) { kbd_rep(h, b, len); return; }
    int ch = 0;
    for (int i = 0; i < h->nf; i++) {
        fld_t* f = &h->f[i];
        int v = getbits(b, len, f->off, f->size, f->sgn);
        if (f->kind == 1) {
            int c = 0x110 + f->idx;
            if (f->idx >= 8) continue;
            if ((v & 1) != (int)(h->btn >> f->idx & 1)) {
                h->btn ^= 1u << f->idx;
                input_report(h->id, IEV_KEY, c, v & 1);
                ch = 1;
            }
        } else if (f->kind == 2 || f->kind == 3) {
            int a = f->kind - 2;
            if (f->rel) { if (v) { input_report(h->id, IEV_REL, a, v); ch = 1; } }
            else {
                int *last = a ? &h->lasty : &h->lastx;
                if (v != *last) { *last = v; input_report(h->id, IEV_ABS, a, v); ch = 1; }
            }
        } else if (f->kind == 4 && v) {
            input_report(h->id, IEV_REL, 8, v);
            ch = 1;
        } else if (f->kind == 5 && v) {
            input_report(h->id, IEV_REL, 6, v);
            ch = 1;
        }
    }
    if (ch) input_report(h->id, IEV_SYN, 0, 0);
}
