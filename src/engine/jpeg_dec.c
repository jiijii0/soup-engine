/*
 * jpeg_dec.c - baseline JPEG decoder.
 *
 * The mirror image of jpeg_enc.c, and deliberately shaped like it: the same
 * zigzag table, the same integer DCT constants, the same stuffed-0xFF
 * convention. Fixed point throughout, because the body this runs on has no
 * FPU and no divide instruction.
 */
#include "jpeg_dec.h"
#include "se_libc.h"

#define JD_MAXCOMP 4

static const uint8_t jd_zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

/* ===================================================================== */
/* Bit reader                                                             */
/* ===================================================================== */

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
    uint32_t       buf;
    int            cnt;
    int            marker;      /* a marker was met: pad from here on */

    /*
     * Optional streaming source. The 5D Classic cannot hand out a block big
     * enough to hold the preview: a develop asked for 2.2 MB with 2264 KB
     * reported free and was still refused, because no single run that large
     * exists. With this set, `p` is a small window refilled as the decoder
     * consumes it, and nothing larger than the window is ever needed.
     */
    se_jdec_read_fn read;
    void           *rctx;
    uint8_t        *win;
    uint32_t        next_off;
    uint32_t        remaining;

    /*
     * A refill that could not be satisfied.
     *
     * This has to be distinguishable from reaching the end of the entropy
     * data, and it was not: jd_refill used to set remaining = 0 and return,
     * which is exactly what running out legitimately looks like. The decoder
     * then padded from frozen DC predictors to the bottom of the frame and
     * returned success, so a card that stalled for longer than read_at was
     * willing to retry produced a complete, plausible, wrong JPEG with rc=0.
     * That is what reached the camera: correct for the first rows, then
     * flat bands of drifting colour.
     */
    int             ioerr;
} jd_bits;

#define JD_WINDOW 8192

static void jd_init(jd_bits *b, const uint8_t *p, size_t len)
{
    b->p = p; b->len = len; b->pos = 0;
    b->buf = 0; b->cnt = 0; b->marker = 0;
    b->read = 0; b->rctx = 0; b->win = 0;
    b->next_off = 0; b->remaining = 0; b->ioerr = 0;
}

/*
 * Pull the next chunk in, keeping whatever has not been consumed. The unread
 * tail matters: telling a stuffed 0xFF00 from a real marker needs a one-byte
 * lookahead, and that pair can straddle a refill.
 */
static void jd_refill(jd_bits *b)
{
    size_t   keep = b->len - b->pos;
    uint32_t want;
    size_t   k;

    if (!b->read || !b->remaining) return;
    for (k = 0; k < keep; k++) b->win[k] = b->win[b->pos + k];
    want = (uint32_t)(JD_WINDOW - keep);
    if (want > b->remaining) want = b->remaining;
    if (b->read(b->rctx, b->next_off, b->win + keep, want) != 0) {
        b->ioerr = 1;              /* NOT the same as running out */
        b->remaining = 0;
        return;
    }
    b->next_off  += want;
    b->remaining -= want;
    b->p   = b->win;
    b->len = keep + want;
    b->pos = 0;
}

static void jd_fill(jd_bits *b)
{
    while (b->cnt <= 24) {
        uint32_t c = 0;
        if (!b->marker && b->pos + 1 >= b->len) jd_refill(b);
        if (!b->marker && b->pos < b->len) {
            c = b->p[b->pos++];
            if (c == 0xff) {
                uint8_t d = (b->pos < b->len) ? b->p[b->pos] : 0xd9;
                if (d == 0x00) {
                    b->pos++;                /* a stuffed byte */
                } else {
                    b->pos--;                /* a real marker */
                    b->marker = 1;
                    c = 0;
                }
            }
        } else {
            b->marker = 1;
        }
        b->buf |= c << (24 - b->cnt);
        b->cnt += 8;
    }
}

static uint32_t jd_get(jd_bits *b, int n)
{
    uint32_t v;
    if (!n) return 0;
    if (b->cnt < n) jd_fill(b);
    v = b->buf >> (32 - n);
    b->buf <<= n;
    b->cnt  -= n;
    return v;
}

/* JPEG's sign extension: a value whose top bit is clear is negative. */
static int jd_extend(uint32_t v, int n)
{
    if (!n) return 0;
    return ((int)v < (1 << (n - 1))) ? (int)v - (1 << n) + 1 : (int)v;
}

/* ===================================================================== */
/* Huffman                                                                */
/* ===================================================================== */

/*
 * `look` is the whole reason this file is fast enough to run on the camera.
 *
 * Decoding a Huffman code one bit at a time is a call, a compare and a
 * shift per bit, and a JPEG is nothing but Huffman codes: a 2496x1664
 * preview is about 97,000 blocks and a million-odd codes. Most of those
 * codes are short -- that is what Huffman coding is for -- so eight bits of
 * the stream are peeked at once and looked up directly. Every code of eight
 * bits or fewer is resolved in one step, and the bit-at-a-time walk below
 * survives only for the rare long ones.
 *
 * Entry layout: value in the high bits, code length in the low five. Zero
 * means "longer than eight bits, take the slow path", which is unambiguous
 * because a length of zero is not a code.
 */
typedef struct {
    uint8_t  bits[17];
    uint8_t  vals[256];
    int      mincode[17];
    int      maxcode[18];
    int      valptr[17];
    uint16_t look[256];
    int      present;
} jd_huff;

static void jd_huff_build(jd_huff *h)
{
    int code = 0, k = 0, i, j;

    for (i = 0; i < 256; i++) h->look[i] = 0;

    for (i = 1; i <= 16; i++) {
        h->valptr[i]  = k;
        h->mincode[i] = code;
        /* Fill the short-code table as we go: a code of length i occupies
         * every eight-bit prefix that starts with it. */
        if (i <= 8) {
            for (j = 0; j < h->bits[i]; j++) {
                int    pfx = (code + j) << (8 - i);
                int    n   = 1 << (8 - i);
                uint16_t e = (uint16_t)((h->vals[k + j] << 5) | i);
                int    q;
                for (q = 0; q < n; q++) h->look[pfx + q] = e;
            }
        }
        code += h->bits[i];
        k    += h->bits[i];
        h->maxcode[i] = code - 1;
        if (!h->bits[i]) h->maxcode[i] = -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
}

static int jd_decode(jd_bits *b, const jd_huff *h)
{
    int l, code;
    uint32_t e;

    /*
     * jd_fill leaves at least 25 bits in the buffer, so the eight peeked
     * below are always real bits or the zero padding it supplies past a
     * marker -- never uninitialised buffer.
     */
    if (b->cnt <= 24) jd_fill(b);
    e = h->look[b->buf >> 24];
    if (e) {
        l = (int)(e & 31);
        b->buf <<= l;
        b->cnt  -= l;
        return (int)(e >> 5);
    }

    l = 1; code = (int)jd_get(b, 1);
    while (l <= 16 && code > h->maxcode[l]) {
        code = (code << 1) | (int)jd_get(b, 1);
        l++;
    }
    if (l > 16) return 0;
    return h->vals[h->valptr[l] + code - h->mincode[l]];
}

/* ===================================================================== */
/* IDCT                                                                   */
/* ===================================================================== */
/*
 * A separable 8-point inverse DCT done as a matrix multiply, in Q14.
 *
 * The hand-unrolled butterfly that usually appears here is perhaps three
 * times faster and far harder to be sure of; this version is the definition
 * written out, so it can be checked against a floating-point reference
 * coefficient by coefficient -- which tools/jpegtest does. At roughly a
 * million blocks for a 4 MP frame it costs a second or two on this body,
 * and correctness was the scarcer commodity here.
 *
 *   jd_ct[u][x] = 1/2 * C(u) * cos((2x+1) u pi / 16),  C(0) = 1/sqrt(2)
 */
static const int16_t jd_ct[8][8] = {
    {   5793,   5793,   5793,   5793,   5793,   5793,   5793,   5793 },
    {   8035,   6811,   4551,   1598,  -1598,  -4551,  -6811,  -8035 },
    {   7568,   3135,  -3135,  -7568,  -7568,  -3135,   3135,   7568 },
    {   6811,  -1598,  -8035,  -4551,   4551,   8035,   1598,  -6811 },
    {   5793,  -5793,  -5793,   5793,   5793,  -5793,  -5793,   5793 },
    {   4551,  -8035,   1598,   6811,  -6811,  -1598,   8035,  -4551 },
    {   3135,  -7568,   7568,  -3135,  -3135,   7568,  -7568,   3135 },
    {   1598,  -4551,   6811,  -8035,   8035,  -6811,   4551,  -1598 },
};

#define JD_DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))

/*
 * Flat block: every coefficient but DC is zero, so the transform collapses
 * to one value repeated. Worth testing for because it is the common case --
 * empty sky, out-of-focus background, and most chroma blocks in most
 * frames. The test costs at most 63 compares against 1024
 * multiply-accumulates.
 */
static int jd_flat(const int *in, int n)
{
    int x, y;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++)
            if ((x | y) && in[y * 8 + x]) return 0;
    return 1;
}

static void jd_fill_block(uint8_t *out, int stride, int n, int dc)
{
    int v = JD_DESCALE(JD_DESCALE(5793 * dc, 11) * 5793, 17) + 128;
    int x, y;
    uint8_t c = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) out[y * stride + x] = c;
}

static void jd_idct(const int *in, uint8_t *out, int stride)
{
    int tmp[64];
    int u, x, y;

    if (jd_flat(in, 8)) { jd_fill_block(out, stride, 8, in[0]); return; }

    /* columns: for each column x, transform the 8 vertical coefficients */
    for (x = 0; x < 8; x++) {
        for (y = 0; y < 8; y++) {
            int acc = 0;
            for (u = 0; u < 8; u++) acc += jd_ct[u][y] * in[u * 8 + x];
            tmp[y * 8 + x] = JD_DESCALE(acc, 11);
        }
    }
    /* rows */
    for (y = 0; y < 8; y++) {
        uint8_t *o = out + y * stride;
        for (x = 0; x < 8; x++) {
            int acc = 0;
            for (u = 0; u < 8; u++) acc += jd_ct[u][x] * tmp[y * 8 + u];
            acc = JD_DESCALE(acc, 17) + 128;
            o[x] = (uint8_t)(acc < 0 ? 0 : acc > 255 ? 255 : acc);
        }
    }
}

/*
 * The same transform, stopped at four points: the top-left 4x4 coefficients
 * in, a 4x4 block out, which is the image at half size.
 *
 * This is not a decode followed by a resize. The DCT basis for N=4 is the
 * even rows of the basis for N=8 restricted to the first four columns --
 *
 *     1/2 C(u) cos((2x+1) u pi / 8)  ==  1/2 C(2u) cos((2x+1) 2u pi / 16)
 *
 * -- so no new table is needed and none is kept: jd_ct[2u][x] for u,x < 4
 * is exactly it.
 *
 * The scale factor works out identically. With only DC present the 8-point
 * transform gives F00/8, and 1/4 * C(0)C(0) * F00 = F00/8 here as well, so
 * the Q14 shifts below are the ones above, unchanged. `filmdev selftest`
 * holds that to the letter: a flat field decoded half-size comes back at
 * exactly its original level, which is the assertion a wrong normalisation
 * cannot pass.
 *
 * What it buys: 128 multiply-accumulates a block instead of 1024, a quarter
 * as many pixels through the colour conversion, and a quarter as many
 * through the encoder. The high-frequency coefficients are discarded rather
 * than reconstructed and then averaged away, which is also a better
 * downscale than the 2x2 box filter it replaces -- a box filter is a poor
 * lowpass, and this one is the DCT's own.
 */
static void jd_idct4(const int *in, uint8_t *out, int stride)
{
    int tmp[16];
    int u, x, y;

    if (jd_flat(in, 4)) { jd_fill_block(out, stride, 4, in[0]); return; }

    for (x = 0; x < 4; x++) {
        for (y = 0; y < 4; y++) {
            int acc = 0;
            for (u = 0; u < 4; u++) acc += jd_ct[u * 2][y] * in[u * 8 + x];
            tmp[y * 4 + x] = JD_DESCALE(acc, 11);
        }
    }
    for (y = 0; y < 4; y++) {
        uint8_t *o = out + y * stride;
        for (x = 0; x < 4; x++) {
            int acc = 0;
            for (u = 0; u < 4; u++) acc += jd_ct[u * 2][x] * tmp[y * 4 + u];
            acc = JD_DESCALE(acc, 17) + 128;
            o[x] = (uint8_t)(acc < 0 ? 0 : acc > 255 ? 255 : acc);
        }
    }
}

/*
 * And stopped at two: the top-left 2x2 coefficients in, a 2x2 block out,
 * the image at quarter size. The same identity one step further -- the
 * N=2 basis is every fourth row of the N=8 basis restricted to the first
 * two columns, jd_ct[4u][x] for u,x < 2 -- and the same normalisation,
 * DC alone giving F00/8, so the shifts are again unchanged. 8 multiply-
 * accumulates a block.
 *
 * This exists for the 700D, whose preview is 18 MP and whose processor
 * sustains about 150 M instructions a second: a half-size frame took 42
 * seconds there, and a quarter-size one (1296x864, larger than the 5D's
 * default output) is the "quick shareable frame" that body can deliver
 * in a fraction of that. `filmdev selftest` holds it to the same three
 * assertions as the half-size transform.
 */
static void jd_idct2(const int *in, uint8_t *out, int stride)
{
    int tmp[4];
    int u, x, y;

    if (jd_flat(in, 2)) { jd_fill_block(out, stride, 2, in[0]); return; }

    for (x = 0; x < 2; x++) {
        for (y = 0; y < 2; y++) {
            int acc = 0;
            for (u = 0; u < 2; u++) acc += jd_ct[u * 4][y] * in[u * 8 + x];
            tmp[y * 2 + x] = JD_DESCALE(acc, 11);
        }
    }
    for (y = 0; y < 2; y++) {
        uint8_t *o = out + y * stride;
        for (x = 0; x < 2; x++) {
            int acc = 0;
            for (u = 0; u < 2; u++) acc += jd_ct[u * 4][x] * tmp[y * 2 + u];
            acc = JD_DESCALE(acc, 17) + 128;
            o[x] = (uint8_t)(acc < 0 ? 0 : acc > 255 ? 255 : acc);
        }
    }
}

/* ===================================================================== */
/* Frame state                                                            */
/* ===================================================================== */

typedef struct {
    int id, hs, vs, tq, td, ta;
    int dcpred;
    uint8_t *plane;       /* hs*8 x vs*8 per MCU, full image width */
    int      pw, ph;      /* plane dimensions in samples */
} jd_comp;

typedef struct {
    int      w, h, ncomp;          /* as coded                           */
    int      bs;                   /* samples per block side: 8 or 4      */
    int      ow, oh;               /* as delivered, after bs              */
    int      hmax, vmax;
    int      mcux, mcuy, mcuw, mcuh;   /* mcuw/mcuh in delivered samples  */
    int      restart;
    uint16_t qt[4][64];
    jd_huff  hdc[4], hac[4];
    jd_comp  c[JD_MAXCOMP];
} jd_state;

static uint16_t jd_rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/*
 * Walk the marker segments. `upto_sos` stops at the scan header, which is
 * all se_jpeg_dec_info needs.
 */
static se_err jd_parse(const uint8_t *d, size_t len, jd_state *s,
                       size_t *scan_off, int header_only)
{
    size_t p = 0;

    if (len < 4 || d[0] != 0xff || d[1] != 0xd8) return SE_E_BADFILE;
    p = 2;

    for (;;) {
        uint16_t m, l;
        while (p < len && d[p] != 0xff) p++;
        while (p < len && d[p] == 0xff) p++;
        if (p >= len) return SE_E_TRUNCATED;
        m = d[p++];

        if (m == 0xd9) return SE_E_BADFILE;              /* EOI before SOS */
        if (m == 0x01 || (m >= 0xd0 && m <= 0xd7)) continue;
        if (p + 2 > len) return SE_E_TRUNCATED;
        l = jd_rd16(d + p);
        if (l < 2 || p + l > len) return SE_E_TRUNCATED;

        switch (m) {
        case 0xc0:                                        /* SOF0 baseline */
        case 0xc1: {
            const uint8_t *q = d + p + 2;
            int i;
            if (q[0] != 8) return SE_E_UNSUPPORTED;       /* 8-bit only */
            s->h = jd_rd16(q + 1);
            s->w = jd_rd16(q + 3);
            s->ncomp = q[5];
            if (s->ncomp < 1 || s->ncomp > 3) return SE_E_UNSUPPORTED;
            s->hmax = s->vmax = 1;
            for (i = 0; i < s->ncomp; i++) {
                const uint8_t *r = q + 6 + i * 3;
                s->c[i].id = r[0];
                s->c[i].hs = r[1] >> 4;
                s->c[i].vs = r[1] & 15;
                s->c[i].tq = r[2];
                if (s->c[i].hs < 1 || s->c[i].hs > 2 ||
                    s->c[i].vs < 1 || s->c[i].vs > 2) return SE_E_UNSUPPORTED;
                if (s->c[i].hs > s->hmax) s->hmax = s->c[i].hs;
                if (s->c[i].vs > s->vmax) s->vmax = s->c[i].vs;
            }
            break;
        }
        case 0xc2: case 0xc3: case 0xc5: case 0xc6: case 0xc7:
        case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
            /* progressive, lossless, arithmetic: say so rather than guess */
            return SE_E_UNSUPPORTED;

        case 0xc4: {                                      /* DHT */
            const uint8_t *q = d + p + 2;
            const uint8_t *e = d + p + l;
            while (q < e) {
                int tc = q[0] >> 4, th = q[0] & 15, i, n = 0;
                jd_huff *t;
                if (th > 3) return SE_E_CORRUPT;
                t = tc ? &s->hac[th] : &s->hdc[th];
                q++;
                t->bits[0] = 0;
                for (i = 1; i <= 16; i++) { t->bits[i] = *q++; n += t->bits[i]; }
                if (n > 256 || q + n > e) return SE_E_CORRUPT;
                for (i = 0; i < n; i++) t->vals[i] = q[i];
                q += n;
                jd_huff_build(t);
                t->present = 1;
            }
            break;
        }
        case 0xdb: {                                      /* DQT */
            const uint8_t *q = d + p + 2;
            const uint8_t *e = d + p + l;
            while (q < e) {
                int pq = q[0] >> 4, tq = q[0] & 15, i;
                if (tq > 3) return SE_E_CORRUPT;
                q++;
                for (i = 0; i < 64; i++) {
                    s->qt[tq][jd_zigzag[i]] =
                        pq ? jd_rd16(q + i * 2) : q[i];
                }
                q += pq ? 128 : 64;
            }
            break;
        }
        case 0xdd:                                        /* DRI */
            s->restart = jd_rd16(d + p + 2);
            break;

        case 0xda: {                                      /* SOS */
            const uint8_t *q = d + p + 2;
            int ns = q[0], i, j;
            for (i = 0; i < ns; i++) {
                int cid = q[1 + i * 2], tt = q[2 + i * 2];
                for (j = 0; j < s->ncomp; j++)
                    if (s->c[j].id == cid) {
                        s->c[j].td = tt >> 4;
                        s->c[j].ta = tt & 15;
                    }
            }
            *scan_off = p + l;
            return SE_OK;
        }
        default:
            break;                                        /* APPn, COM, ... */
        }
        p += l;
        if (header_only && s->w && s->ncomp) {
            /* keep going: the tables still have to be read before the scan */
        }
    }
}

se_err se_jpeg_dec_info(const void *buf, size_t len, se_jdec_info *out)
{
    jd_state s;
    size_t so = 0;
    se_err rc;
    memset(&s, 0, sizeof s);
    rc = jd_parse((const uint8_t *)buf, len, &s, &so, 1);
    if (rc != SE_OK) return rc;
    out->width = s.w; out->height = s.h; out->ncomp = s.ncomp;
    return SE_OK;
}

/* ===================================================================== */
/* Colour                                                                 */
/* ===================================================================== */
/* Q16 BT.601, the inverse of what jpeg_enc.c uses on the way in. */
static void jd_ycc_row(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                       uint8_t *rgb, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int Y  = y[i] << 16;
        int Cb = cb[i] - 128, Cr = cr[i] - 128;
        int r = (Y + 91881 * Cr) >> 16;
        int g = (Y - 22554 * Cb - 46802 * Cr) >> 16;
        int b = (Y + 116130 * Cb) >> 16;
        rgb[i * 3 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        rgb[i * 3 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
        rgb[i * 3 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
}

/* ===================================================================== */
/* Decode                                                                 */
/* ===================================================================== */

/*
 * Both entry points run the same decoder: `d`/`len` describe a buffer that
 * holds at least the header, and when `rd` is set everything past the scan
 * header is pulled through a window instead of being addressed directly.
 */
static se_err jd_run(const uint8_t *d, size_t len,
                     se_jdec_read_fn rd, void *rctx, uint32_t total,
                     int scale, se_arena *a, se_jdec_sink sink, void *ctx,
                     se_jdec_info *info)
{
    jd_state *s;
    jd_bits   bits;
    size_t    scan = 0;
    se_err    rc;
    int       i, mx, my, blk[64];
    uint8_t  *rgb;

    if (scale != 1 && scale != 2 && scale != 4) return SE_E_UNSUPPORTED;

    s = SE_NEW(a, jd_state, 1);
    if (!s) return SE_E_NOMEM;
    memset(s, 0, sizeof *s);
    s->bs = 8 / scale;

    rc = jd_parse(d, len, s, &scan, 0);
    if (rc != SE_OK) return rc;
    if (!s->w || !s->h) return SE_E_CORRUPT;

    /*
     * The MCU grid is a property of the coded stream, so it is counted in
     * coded samples whatever we intend to deliver. Only the sizes of what
     * comes out of the transform follow s->bs.
     */
    s->mcux = (s->w + s->hmax * 8 - 1) / (s->hmax * 8);
    s->mcuy = (s->h + s->vmax * 8 - 1) / (s->vmax * 8);
    s->mcuw = s->hmax * s->bs;
    s->mcuh = s->vmax * s->bs;
    se_jpeg_scaled_size(s->w, s->h, scale, &s->ow, &s->oh);

    /* One MCU row per component, full image width. */
    for (i = 0; i < s->ncomp; i++) {
        s->c[i].pw = s->mcux * s->c[i].hs * s->bs;
        s->c[i].ph = s->c[i].vs * s->bs;
        s->c[i].plane = SE_NEW(a, uint8_t, (size_t)s->c[i].pw * s->c[i].ph);
        if (!s->c[i].plane) return SE_E_NOMEM;
        s->c[i].dcpred = 0;
    }
    rgb = SE_NEW(a, uint8_t, (size_t)s->mcux * s->mcuw * 3 * s->mcuh);
    if (!rgb) return SE_E_NOMEM;

    if (rd) {
        if (s->restart) {
            /* Resynchronising means scanning forward for a marker, which a
             * window that only moves forward cannot do. Canon's previews
             * carry no DRI, so refuse rather than decode out of step. */
            se_log("jpeg: restart intervals are not supported when streaming");
            return SE_E_UNSUPPORTED;
        }
        jd_init(&bits, 0, 0);
        bits.win = (uint8_t *)se_arena_alloc(a, JD_WINDOW, 32);
        if (!bits.win) return SE_E_NOMEM;
        bits.read      = rd;
        bits.rctx      = rctx;
        bits.next_off  = (uint32_t)scan;
        bits.remaining = total - (uint32_t)scan;
        bits.p   = bits.win;
        bits.len = bits.pos = 0;
        jd_refill(&bits);
    } else {
        jd_init(&bits, d + scan, len - scan);
    }

    for (my = 0; my < s->mcuy; my++) {
        int rows, y;

        for (mx = 0; mx < s->mcux; mx++) {
            int ci;

            if (s->restart && ((my * s->mcux + mx) % s->restart) == 0 &&
                (my || mx)) {
                /* resynchronise: skip to the RSTn and reset the predictors */
                bits.cnt = 0; bits.buf = 0; bits.marker = 0;
                while (bits.pos + 1 < bits.len &&
                       !(bits.p[bits.pos] == 0xff &&
                         bits.p[bits.pos + 1] >= 0xd0 &&
                         bits.p[bits.pos + 1] <= 0xd7))
                    bits.pos++;
                bits.pos += 2;
                for (ci = 0; ci < s->ncomp; ci++) s->c[ci].dcpred = 0;
            }

            for (ci = 0; ci < s->ncomp; ci++) {
                jd_comp *c = &s->c[ci];
                int bx, by;
                for (by = 0; by < c->vs; by++)
                    for (bx = 0; bx < c->hs; bx++) {
                        int t, k = 1, diff;
                        memset(blk, 0, sizeof blk);
                        t = jd_decode(&bits, &s->hdc[c->td]);
                        diff = jd_extend(jd_get(&bits, t), t);
                        c->dcpred += diff;
                        blk[0] = c->dcpred * s->qt[c->tq][0];
                        while (k < 64) {
                            int rs = jd_decode(&bits, &s->hac[c->ta]);
                            int r = rs >> 4, sz = rs & 15;
                            if (!sz) { if (r != 15) break; k += 16; continue; }
                            k += r;
                            if (k > 63) break;
                            blk[jd_zigzag[k]] =
                                jd_extend(jd_get(&bits, sz), sz) *
                                s->qt[c->tq][jd_zigzag[k]];
                            k++;
                        }
                        {
                            uint8_t *o = c->plane
                                + (size_t)by * s->bs * c->pw
                                + (size_t)(mx * c->hs + bx) * s->bs;
                            if (s->bs == 8)      jd_idct(blk, o, c->pw);
                            else if (s->bs == 4) jd_idct4(blk, o, c->pw);
                            else                 jd_idct2(blk, o, c->pw);
                        }
                    }
            }
        }

        /* This MCU row is complete: upsample, convert, hand it over. */
        rows = s->mcuh;
        if (my * s->mcuh + rows > s->oh) rows = s->oh - my * s->mcuh;

        for (y = 0; y < rows; y++) {
            uint8_t *orow = rgb + (size_t)y * s->ow * 3;
            if (s->ncomp == 1) {
                const uint8_t *yy = s->c[0].plane + (size_t)y * s->c[0].pw;
                int x;
                for (x = 0; x < s->ow; x++) {
                    orow[x * 3] = orow[x * 3 + 1] = orow[x * 3 + 2] = yy[x];
                }
            } else {
                const jd_comp *Y = &s->c[0], *Cb = &s->c[1], *Cr = &s->c[2];
                const uint8_t *yy = Y->plane + (size_t)y * Y->pw;
                const uint8_t *bb = Cb->plane +
                    (size_t)(y * Cb->vs / s->vmax) * Cb->pw;
                const uint8_t *rr = Cr->plane +
                    (size_t)(y * Cr->vs / s->vmax) * Cr->pw;
                int x;
                for (x = 0; x < s->ow; x++) {
                    int xb = x * Cb->hs / s->hmax;
                    int xr = x * Cr->hs / s->hmax;
                    int Yv  = yy[x] << 16;
                    int Cbv = bb[xb] - 128, Crv = rr[xr] - 128;
                    int r = (Yv + 91881 * Crv) >> 16;
                    int g = (Yv - 22554 * Cbv - 46802 * Crv) >> 16;
                    int b = (Yv + 116130 * Cbv) >> 16;
                    orow[x * 3 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
                    orow[x * 3 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
                    orow[x * 3 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
                }
            }
        }
        if (sink(ctx, rgb, rows, s->ow) != 0) return SE_E_IO;
        /*
         * Checked once per MCU row rather than per read: it costs nothing
         * here, and it means a frame that lost its data stops within one row
         * band of where it happened instead of being finished from stale
         * state and handed back as if it were fine.
         */
        if (bits.ioerr) return SE_E_IO;
        se_progress("jpeg", my * 100 / (s->mcuy ? s->mcuy : 1));
    }

    if (bits.ioerr) return SE_E_IO;
    /* the delivered size, not the coded one -- see jpeg_dec.h */
    if (info) { info->width = s->ow; info->height = s->oh; info->ncomp = s->ncomp; }
    se_progress("jpeg", 100);
    (void)jd_ycc_row;
    return SE_OK;
}

void se_jpeg_scaled_size(int w, int h, int scale, int *ow, int *oh)
{
    int bs = (scale == 4) ? 2 : (scale == 2) ? 4 : 8;
    /* round up: a 2495-wide frame still delivers its last column */
    if (ow) *ow = (w * bs + 7) / 8;
    if (oh) *oh = (h * bs + 7) / 8;
}

se_err se_jpeg_decode(const void *buf, size_t len, se_arena *a,
                      se_jdec_sink sink, void *ctx, se_jdec_info *info)
{
    return jd_run((const uint8_t *)buf, len, 0, 0, 0, 1, a, sink, ctx, info);
}

se_err se_jpeg_decode_scaled(const void *buf, size_t len, int scale,
                             se_arena *a, se_jdec_sink sink, void *ctx,
                             se_jdec_info *info)
{
    return jd_run((const uint8_t *)buf, len, 0, 0, 0, scale, a, sink, ctx, info);
}

se_err se_jpeg_decode_stream(const void *hdr, size_t hdrlen, uint32_t total,
                             se_jdec_read_fn read, void *rctx, se_arena *a,
                             se_jdec_sink sink, void *ctx, se_jdec_info *info)
{
    return se_jpeg_decode_stream_scaled(hdr, hdrlen, total, read, rctx, 1,
                                        a, sink, ctx, info);
}

se_err se_jpeg_decode_stream_scaled(const void *hdr, size_t hdrlen,
                                    uint32_t total, se_jdec_read_fn read,
                                    void *rctx, int scale, se_arena *a,
                                    se_jdec_sink sink, void *ctx,
                                    se_jdec_info *info)
{
    if (!read) return SE_E_IO;
    return jd_run((const uint8_t *)hdr, hdrlen, read, rctx, total, scale,
                  a, sink, ctx, info);
}
