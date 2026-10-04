/* png/jpeg/gif decoders, copied from the kernel's src/apps/imgdec.c (browser) */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define kmalloc_big(n) malloc(n)
#define kfree(p) free(p)

#define IMG_MAX 4096               /* either side */

/* ---------- inflate ---------- */
typedef struct { const uint8_t* p; int n, pos; uint32_t bit, cnt; } bits_t;
typedef struct { uint16_t cnt[16], sym[320]; } huff_t;

static int getbit(bits_t* b) {
    if (!b->cnt) {
        if (b->pos >= b->n) return -1;
        b->bit = b->p[b->pos++];
        b->cnt = 8;
    }
    int r = b->bit & 1;
    b->bit >>= 1;
    b->cnt--;
    return r;
}
static int getbits(bits_t* b, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) { int x = getbit(b); if (x < 0) return -1; v |= x << i; }
    return v;
}

static void huff_build(huff_t* h, const uint8_t* len, int n) {
    uint16_t off[16];
    memset(h->cnt, 0, sizeof h->cnt);
    for (int i = 0; i < n; i++) h->cnt[len[i]]++;
    h->cnt[0] = 0;
    off[1] = 0;
    for (int i = 1; i < 15; i++) off[i + 1] = (uint16_t)(off[i] + h->cnt[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[off[len[i]]++] = (uint16_t)i;
}

static int huff_dec(bits_t* b, const huff_t* h) {
    int code = 0, first = 0, idx = 0;
    for (int l = 1; l < 16; l++) {
        int x = getbit(b);
        if (x < 0) return -1;
        code |= x;
        int c = h->cnt[l];
        if (code - c < first) return h->sym[idx + (code - first)];
        idx += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t lbase[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  lext[]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dbase[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  dext[]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* raw deflate stream -> out (cap bytes); returns bytes written or -1 */
static int inflate(const uint8_t* src, int n, uint8_t* out, int cap) {
    bits_t b = { src, n, 0, 0, 0 };
    static huff_t lh, dh;
    int o = 0, last;
    do {
        last = getbit(&b);
        int type = getbits(&b, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {
            b.cnt = 0;
            if (b.pos + 4 > n) return -1;
            int len = src[b.pos] | src[b.pos + 1] << 8;
            b.pos += 4;
            if (b.pos + len > n || o + len > cap) return -1;
            memcpy(out + o, src + b.pos, (size_t)len);
            b.pos += len; o += len;
            continue;
        }
        if (type == 3) return -1;
        uint8_t len[320];
        if (type == 1) {
            int i = 0;
            for (; i < 144; i++) len[i] = 8;
            for (; i < 256; i++) len[i] = 9;
            for (; i < 280; i++) len[i] = 7;
            for (; i < 288; i++) len[i] = 8;
            huff_build(&lh, len, 288);
            for (i = 0; i < 30; i++) len[i] = 5;
            huff_build(&dh, len, 30);
        } else {
            int hl = getbits(&b, 5) + 257, hd = getbits(&b, 5) + 1, hc = getbits(&b, 4) + 4;
            static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t cl[19];
            memset(cl, 0, sizeof cl);
            for (int i = 0; i < hc; i++) cl[ord[i]] = (uint8_t)getbits(&b, 3);
            huff_t ch;
            huff_build(&ch, cl, 19);
            int i = 0;
            while (i < hl + hd) {
                int s = huff_dec(&b, &ch);
                if (s < 0) return -1;
                if (s < 16) { len[i++] = (uint8_t)s; continue; }
                int rep = 0, v = 0;
                if (s == 16) { if (!i) return -1; v = len[i - 1]; rep = 3 + getbits(&b, 2); }
                else if (s == 17) rep = 3 + getbits(&b, 3);
                else rep = 11 + getbits(&b, 7);
                if (i + rep > hl + hd) return -1;
                while (rep--) len[i++] = (uint8_t)v;
            }
            huff_build(&lh, len, hl);
            huff_build(&dh, len + hl, hd);
        }
        for (;;) {
            int s = huff_dec(&b, &lh);
            if (s < 0) return -1;
            if (s < 256) { if (o >= cap) return -1; out[o++] = (uint8_t)s; continue; }
            if (s == 256) break;
            s -= 257;
            if (s >= 29) return -1;
            int l = lbase[s] + getbits(&b, lext[s]);
            int d = huff_dec(&b, &dh);
            if (d < 0 || d >= 30) return -1;
            int dist = dbase[d] + getbits(&b, dext[d]);
            if (dist > o || o + l > cap) return -1;
            for (int k = 0; k < l; k++, o++) out[o] = out[o - dist];
        }
    } while (!last);
    return o;
}

/* ---------- png ---------- */
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static uint32_t* png_decode(const uint8_t* d, int n, int* ow, int* oh) {
    int pos = 8, w = 0, h = 0, depth = 0, ct = 0, il = 0;
    uint32_t pal[256];
    int npal = 0, trns = -1;
    uint8_t* z = NULL;
    int zn = 0, zcap = 0;
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u;
    while (pos + 8 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t* t = d + pos + 4;
        const uint8_t* c = d + pos + 8;
        if (len > (uint32_t)(n - pos - 12)) break;
        if (!memcmp(t, "IHDR", 4)) {
            w = (int)be32(c); h = (int)be32(c + 4); depth = c[8]; ct = c[9]; il = c[12];
            if (w <= 0 || h <= 0 || w > IMG_MAX || h > IMG_MAX || il) return NULL;
            zcap = n;
            z = kmalloc_big((uint32_t)zcap);
            if (!z) return NULL;
        } else if (!memcmp(t, "PLTE", 4)) {
            npal = (int)len / 3;
            for (int i = 0; i < npal && i < 256; i++) pal[i] = 0xFF000000u | (uint32_t)c[i * 3] << 16 | (uint32_t)c[i * 3 + 1] << 8 | c[i * 3 + 2];
        } else if (!memcmp(t, "tRNS", 4)) {
            if (ct == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i] = (pal[i] & 0xFFFFFF) | (uint32_t)c[i] << 24;
            else if (ct == 0 && len >= 2) trns = c[0] << 8 | c[1];
        } else if (!memcmp(t, "IDAT", 4) && z) {
            if (zn + (int)len > zcap) break;
            memcpy(z + zn, c, len);
            zn += (int)len;
        } else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + (int)len;
    }
    if (!z || zn < 6) { if (z) kfree(z); return NULL; }
    int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!ch || (depth != 8 && depth != 16 && !(depth < 8 && (ct == 0 || ct == 3)))) { kfree(z); return NULL; }
    int bpp = ch * depth;                         /* bits per pixel */
    int stride = (w * bpp + 7) / 8;
    int raw_n = (stride + 1) * h;
    uint8_t* raw = kmalloc_big((uint32_t)raw_n);
    uint32_t* px = raw ? kmalloc_big((uint32_t)(w * h * 4)) : NULL;
    if (!px) { kfree(z); if (raw) kfree(raw); return NULL; }
    int got = inflate(z + 2, zn - 2, raw, raw_n);   /* skip the zlib header */
    kfree(z);
    if (got < raw_n) { kfree(raw); kfree(px); return NULL; }
    int bx = (bpp + 7) / 8;
    for (int y = 0; y < h; y++) {
        uint8_t* r = raw + y * (stride + 1);
        uint8_t* cur = r + 1;
        uint8_t* prev = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
        int f = r[0];
        for (int i = 0; i < stride; i++) {
            int a = i >= bx ? cur[i - bx] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bx) ? prev[i - bx] : 0;
            switch (f) {
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            }
        }
        uint32_t* o = px + y * w;
        for (int x = 0; x < w; x++) {
            uint32_t v;
            if (depth < 8) {
                int per = 8 / depth, sh = 8 - depth - (x % per) * depth;
                int s = (cur[x / per] >> sh) & ((1 << depth) - 1);
                if (ct == 3) v = pal[s];
                else { int g = s * 255 / ((1 << depth) - 1); v = 0xFF000000u | (uint32_t)g * 0x010101u; if (s == trns) v = 0; }
            } else {
                const uint8_t* q = cur + x * ch * (depth / 8);
                int st = depth / 8;                    /* 16-bit: take the high byte */
                if (ct == 0) { v = 0xFF000000u | (uint32_t)q[0] * 0x010101u; if (trns >= 0 && (st == 1 ? q[0] : (q[0] << 8 | q[1])) == trns) v = 0; }
                else if (ct == 2) v = 0xFF000000u | (uint32_t)q[0] << 16 | (uint32_t)q[st] << 8 | q[2 * st];
                else if (ct == 3) v = pal[q[0]];
                else if (ct == 4) v = (uint32_t)q[st] << 24 | (uint32_t)q[0] * 0x010101u;
                else v = (uint32_t)q[3 * st] << 24 | (uint32_t)q[0] << 16 | (uint32_t)q[st] << 8 | q[2 * st];
            }
            o[x] = v;
        }
    }
    kfree(raw);
    *ow = w; *oh = h;
    return px;
}

/* ---------- jpeg (baseline) ---------- */
static const int16_t idct_t[8][8] = {
    {2896,4017,3784,3406,2896,2276,1567,799},
    {2896,3406,1567,-799,-2896,-4017,-3784,-2276},
    {2896,2276,-1567,-4017,-2896,799,3784,3406},
    {2896,799,-3784,-2276,2896,3406,-1567,-4017},
    {2896,-799,-3784,2276,2896,-3406,-1567,4017},
    {2896,-2276,-1567,4017,-2896,-799,3784,-3406},
    {2896,-3406,1567,799,-2896,4017,-3784,2276},
    {2896,-4017,3784,-3406,2896,-2276,1567,-799},
};
static const uint8_t zz[64] = { 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

typedef struct { uint8_t len[256]; uint8_t val[256]; int nv; uint16_t code[256]; } jh_t;

typedef struct {
    const uint8_t* p; int n, pos;
    uint32_t bits; int nb;
    uint16_t q[4][64];
    jh_t hd[4], ha[4];
    int nc, w, h, hmax, vmax, rst;
    struct { int id, hs, vs, tq, td, ta, dc; uint8_t* buf; int bw, bh; } c[3];
} jpg_t;

static void jh_build(jh_t* h, const uint8_t* cnt, const uint8_t* vals) {
    int k = 0, code = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < cnt[l - 1] && k < 256; i++) { h->len[k] = (uint8_t)l; h->val[k] = vals[k]; h->code[k] = (uint16_t)code++; k++; }
        code <<= 1;
    }
    h->nv = k;
}

static int jbit(jpg_t* j) {
    if (!j->nb) {
        if (j->pos >= j->n) return 0;
        uint8_t b = j->p[j->pos++];
        if (b == 0xFF) {
            uint8_t m = j->pos < j->n ? j->p[j->pos] : 0;
            if (m == 0) j->pos++;
            else { j->pos--; b = 0; }                 /* a marker: feed zeros */
        }
        j->bits = b;
        j->nb = 8;
    }
    j->nb--;
    return (j->bits >> j->nb) & 1;
}

static int jdec(jpg_t* j, const jh_t* h) {
    int code = 0, l = 0, k = 0;
    while (l < 16) {
        code = code << 1 | jbit(j);
        l++;
        for (; k < h->nv && h->len[k] == l; k++) if (h->code[k] == code) return h->val[k];
    }
    return -1;
}

static int jrecv(jpg_t* j, int s) {
    if (!s) return 0;
    int v = 0;
    for (int i = 0; i < s; i++) v = v << 1 | jbit(j);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static bool jblock(jpg_t* j, int ci, uint8_t* out, int stride) {
    int co[64];
    memset(co, 0, sizeof co);
    int t = jdec(j, &j->hd[j->c[ci].td]);
    if (t < 0) return false;
    j->c[ci].dc += jrecv(j, t);
    const uint16_t* q = j->q[j->c[ci].tq];
    co[0] = j->c[ci].dc * q[0];
    for (int k = 1; k < 64; ) {
        int rs = jdec(j, &j->ha[j->c[ci].ta]);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (!s) { if (r != 15) break; k += 16; continue; }
        k += r;
        if (k > 63) break;
        co[zz[k]] = jrecv(j, s) * q[k];
        k++;
    }
    int tmp[64];
    for (int v = 0; v < 8; v++)                   /* rows */
        for (int x = 0; x < 8; x++) {
            int s = 0;
            for (int u = 0; u < 8; u++) s += idct_t[x][u] * co[v * 8 + u];
            tmp[v * 8 + x] = (s + 2048) >> 12;
        }
    for (int x = 0; x < 8; x++)                   /* columns */
        for (int y = 0; y < 8; y++) {
            int s = 0;
            for (int v = 0; v < 8; v++) s += idct_t[y][v] * tmp[v * 8 + x];
            s = ((s + 8192) >> 14) + 128;         /* /4 and the table scale */
            out[y * stride + x] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
        }
    return true;
}

static uint32_t* jpeg_decode(const uint8_t* d, int n, int* ow, int* oh) {
    static jpg_t J;
    jpg_t* j = &J;
    memset(j, 0, sizeof *j);
    j->p = d; j->n = n; j->pos = 2;
    uint32_t* px = NULL;
    while (j->pos + 4 <= n) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }
        int m = d[j->pos + 1];
        j->pos += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) { if (m == 0xFF) j->pos--; continue; }
        if (m == 0xD9) break;
        int len = d[j->pos] << 8 | d[j->pos + 1];
        const uint8_t* s = d + j->pos + 2;
        if (j->pos + len > n) break;
        if (m == 0xDB) {
            for (int i = 0; i + 65 <= len - 2; ) {
                int pq = s[i] >> 4, tq = s[i] & 3;
                i++;
                for (int k = 0; k < 64; k++) { j->q[tq][k] = pq ? (uint16_t)(s[i] << 8 | s[i + 1]) : s[i]; i += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {
            for (int i = 0; i + 17 <= len - 2; ) {
                int tc = s[i] >> 4, th = s[i] & 3;
                const uint8_t* cnt = s + i + 1;
                int tot = 0;
                for (int k = 0; k < 16; k++) tot += cnt[k];
                if (tot > 256) goto bad;
                jh_build(tc ? &j->ha[th] : &j->hd[th], cnt, s + i + 17);
                i += 17 + tot;
            }
        } else if (m == 0xC0 || m == 0xC1) {
            j->h = s[1] << 8 | s[2]; j->w = s[3] << 8 | s[4]; j->nc = s[5];
            if (j->nc != 1 && j->nc != 3) goto bad;
            if (j->w <= 0 || j->h <= 0 || j->w > IMG_MAX || j->h > IMG_MAX) goto bad;
            for (int i = 0; i < j->nc; i++) {
                j->c[i].id = s[6 + i * 3];
                j->c[i].hs = s[7 + i * 3] >> 4; j->c[i].vs = s[7 + i * 3] & 15;
                j->c[i].tq = s[8 + i * 3] & 3;
                if (j->c[i].hs < 1 || j->c[i].hs > 2 || j->c[i].vs < 1 || j->c[i].vs > 2) goto bad;
                if (j->c[i].hs > j->hmax) j->hmax = j->c[i].hs;
                if (j->c[i].vs > j->vmax) j->vmax = j->c[i].vs;
            }
        } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            goto bad;                                 /* progressive, lossless, arithmetic */
        } else if (m == 0xDD) {
            j->rst = s[0] << 8 | s[1];
        } else if (m == 0xDA) {
            if (!j->nc) goto bad;
            int ns = s[0];
            for (int i = 0; i < ns; i++)
                for (int k = 0; k < j->nc; k++)
                    if (j->c[k].id == s[1 + i * 2]) { j->c[k].td = s[2 + i * 2] >> 4 & 3; j->c[k].ta = s[2 + i * 2] & 3; }
            j->pos += len;
            int mw = 8 * j->hmax, mh = 8 * j->vmax;
            int mx = (j->w + mw - 1) / mw, my = (j->h + mh - 1) / mh;
            for (int k = 0; k < j->nc; k++) {
                j->c[k].bw = mx * j->c[k].hs * 8;
                j->c[k].bh = my * j->c[k].vs * 8;
                j->c[k].buf = kmalloc_big((uint32_t)(j->c[k].bw * j->c[k].bh));
                if (!j->c[k].buf) goto bad;
            }
            int todo = j->rst;
            for (int yy = 0; yy < my; yy++)
                for (int xx = 0; xx < mx; xx++) {
                    if (j->rst && todo-- == 0) {          /* restart marker: resync */
                        j->nb = 0;
                        while (j->pos + 1 < n && !(d[j->pos] == 0xFF && d[j->pos + 1] >= 0xD0 && d[j->pos + 1] <= 0xD7)) j->pos++;
                        j->pos += 2;
                        for (int k = 0; k < j->nc; k++) j->c[k].dc = 0;
                        todo = j->rst - 1;
                    }
                    for (int k = 0; k < j->nc; k++)
                        for (int by = 0; by < j->c[k].vs; by++)
                            for (int bx = 0; bx < j->c[k].hs; bx++) {
                                int ox = (xx * j->c[k].hs + bx) * 8, oy = (yy * j->c[k].vs + by) * 8;
                                if (!jblock(j, k, j->c[k].buf + oy * j->c[k].bw + ox, j->c[k].bw)) goto bad;
                            }
                }
            px = kmalloc_big((uint32_t)(j->w * j->h * 4));
            if (!px) goto bad;
            for (int y = 0; y < j->h; y++)
                for (int x = 0; x < j->w; x++) {
                    int Y = j->c[0].buf[(y * j->c[0].vs / j->vmax) * j->c[0].bw + x * j->c[0].hs / j->hmax];
                    if (j->nc == 1) { px[y * j->w + x] = 0xFF000000u | (uint32_t)Y * 0x010101u; continue; }
                    int cb = j->c[1].buf[(y * j->c[1].vs / j->vmax) * j->c[1].bw + x * j->c[1].hs / j->hmax] - 128;
                    int cr = j->c[2].buf[(y * j->c[2].vs / j->vmax) * j->c[2].bw + x * j->c[2].hs / j->hmax] - 128;
                    int r = Y + ((91881 * cr) >> 16), g = Y - ((22554 * cb + 46802 * cr) >> 16), b = Y + ((116130 * cb) >> 16);
                    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
                    px[y * j->w + x] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                }
            break;
        }
        j->pos += len;
    }
    for (int k = 0; k < 3; k++) if (j->c[k].buf) kfree(j->c[k].buf);
    if (!px) return NULL;
    *ow = j->w; *oh = j->h;
    return px;
bad:
    for (int k = 0; k < 3; k++) if (j->c[k].buf) kfree(j->c[k].buf);
    if (px) kfree(px);
    return NULL;
}

/* ---------- gif (first frame) ---------- */
static uint32_t* gif_decode(const uint8_t* d, int n, int* ow, int* oh) {
    if (n < 13) return NULL;
    int w = d[6] | d[7] << 8, h = d[8] | d[9] << 8;
    if (w <= 0 || h <= 0 || w > IMG_MAX || h > IMG_MAX) return NULL;
    uint32_t gpal[256], lpal[256];
    int pos = 13, trans = -1;
    if (d[10] & 0x80) {
        int k = 2 << (d[10] & 7);
        for (int i = 0; i < k && pos + 3 <= n; i++, pos += 3) gpal[i] = 0xFF000000u | (uint32_t)d[pos] << 16 | (uint32_t)d[pos + 1] << 8 | d[pos + 2];
    }
    while (pos < n) {
        if (d[pos] == 0x21) {                         /* extension */
            if (pos + 2 < n && d[pos + 1] == 0xF9 && pos + 6 < n && (d[pos + 3] & 1)) trans = d[pos + 6];
            pos += 2;
            while (pos < n && d[pos]) pos += d[pos] + 1;
            pos++;
            continue;
        }
        if (d[pos] != 0x2C || pos + 10 > n) return NULL;
        int fx = d[pos + 1] | d[pos + 2] << 8, fy = d[pos + 3] | d[pos + 4] << 8;
        int fw = d[pos + 5] | d[pos + 6] << 8, fh = d[pos + 7] | d[pos + 8] << 8, fl = d[pos + 9];
        pos += 10;
        uint32_t* pal = gpal;
        if (fl & 0x80) {
            int k = 2 << (fl & 7);
            for (int i = 0; i < k && pos + 3 <= n; i++, pos += 3) lpal[i] = 0xFF000000u | (uint32_t)d[pos] << 16 | (uint32_t)d[pos + 1] << 8 | d[pos + 2];
            pal = lpal;
        }
        bool inter = fl & 0x40;
        if (trans >= 0) pal[trans] = 0;
        if (pos >= n) return NULL;
        int mcs = d[pos++];
        if (mcs < 2 || mcs > 8) return NULL;
        /* gather the sub-blocks */
        uint8_t* data = kmalloc_big((uint32_t)n);
        uint32_t* px = kmalloc_big((uint32_t)(w * h * 4));
        static uint16_t pre[4096];
        static uint8_t suf[4096], stk[4097];
        if (!data || !px) { if (data) kfree(data); if (px) kfree(px); return NULL; }
        memset(px, 0, (uint32_t)(w * h * 4));
        int dn = 0;
        while (pos < n && d[pos]) { int l = d[pos]; if (pos + 1 + l > n) break; memcpy(data + dn, d + pos + 1, (size_t)l); dn += l; pos += l + 1; }
        int clear = 1 << mcs, eoi = clear + 1, cs = mcs + 1, next = clear + 2, old = -1, first = 0;
        uint32_t acc = 0;
        int nb = 0, bp = 0, pix = 0, tot = fw * fh;
        int rows_done[4] = { 0 };
        (void)rows_done;
        for (int i = 0; i < clear; i++) { pre[i] = 0xFFFF; suf[i] = (uint8_t)i; }
        while (pix < tot) {
            while (nb < cs && bp < dn) { acc |= (uint32_t)data[bp++] << nb; nb += 8; }
            if (nb < cs) break;
            int code = acc & ((1 << cs) - 1);
            acc >>= cs; nb -= cs;
            if (code == clear) { cs = mcs + 1; next = clear + 2; old = -1; continue; }
            if (code == eoi) break;
            int sp = 0, c = code;
            if (old < 0) { stk[sp++] = suf[code]; first = code; }
            else {
                if (code >= next) { stk[sp++] = (uint8_t)first; c = old; }
                while (c >= clear && sp < 4096) { stk[sp++] = suf[c]; c = pre[c]; }
                stk[sp++] = suf[c];
                first = suf[c];
                if (next < 4096) { pre[next] = (uint16_t)old; suf[next] = (uint8_t)first; next++; }
                if (next == (1 << cs) && cs < 12) cs++;
            }
            old = code;
            while (sp && pix < tot) {
                int x = pix % fw, y = pix / fw;
                if (inter) {                          /* rows come as 0,8.. 4,12.. 2,6.. 1,3.. */
                    int p1 = (fh + 7) / 8, p2 = (fh + 3) / 8, p3 = (fh + 1) / 4;
                    y = y < p1 ? y * 8 : y < p1 + p2 ? (y - p1) * 8 + 4 : y < p1 + p2 + p3 ? (y - p1 - p2) * 4 + 2 : (y - p1 - p2 - p3) * 2 + 1;
                }
                int X = fx + x, Y = fy + y;
                uint8_t v = stk[--sp];
                if (X < w && Y < h) px[Y * w + X] = pal[v];
                pix++;
            }
        }
        kfree(data);
        *ow = w; *oh = h;
        return px;
    }
    return NULL;
}

uint32_t *img_decode(const uint8_t* d, int n, int* w, int* h) {
    if (n > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return png_decode(d, n, w, h);
    if (n > 4 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_decode(d, n, w, h);
    if (n > 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return gif_decode(d, n, w, h);
    return NULL;
}
