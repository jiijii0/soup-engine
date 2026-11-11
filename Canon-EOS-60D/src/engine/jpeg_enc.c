#include "jpeg_enc.h"
#include "se_libc.h"

/* ---------------------------------------------------------- tables ---- */

static const uint8_t g_zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

/* Annex K example quantisation tables, natural order. */
static const uint8_t g_qt_lum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};
static const uint8_t g_qt_chr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Annex K example Huffman tables. */
static const uint8_t g_dc_lum_bits[17] = { 0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
static const uint8_t g_dc_lum_val[12]  = { 0,1,2,3,4,5,6,7,8,9,10,11 };
static const uint8_t g_dc_chr_bits[17] = { 0, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
static const uint8_t g_dc_chr_val[12]  = { 0,1,2,3,4,5,6,7,8,9,10,11 };

static const uint8_t g_ac_lum_bits[17] = { 0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d };
static const uint8_t g_ac_lum_val[162] = {
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
  0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
  0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
  0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
  0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
  0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
  0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
  0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
  0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
  0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
  0xf9,0xfa
};
static const uint8_t g_ac_chr_bits[17] = { 0, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
static const uint8_t g_ac_chr_val[162] = {
  0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
  0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
  0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
  0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
  0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
  0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
  0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
  0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
  0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
  0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
  0xf9,0xfa
};

/* Derived code tables: [symbol] -> (length << 16) | code. */
static uint32_t g_dc_lum[256], g_ac_lum[256], g_dc_chr[256], g_ac_chr[256];
static int      g_tables_built;

static void build_codes(const uint8_t *bits, const uint8_t *vals, uint32_t *out)
{
    int      l, i, k = 0;
    uint32_t code = 0;
    memset(out, 0, 256 * sizeof(uint32_t));
    for (l = 1; l <= 16; l++) {
        for (i = 0; i < bits[l]; i++, k++)
            out[vals[k]] = ((uint32_t)l << 16) | code++;
        code <<= 1;
    }
}

static void build_all_codes(void)
{
    if (g_tables_built) return;
    build_codes(g_dc_lum_bits, g_dc_lum_val, g_dc_lum);
    build_codes(g_dc_chr_bits, g_dc_chr_val, g_dc_chr);
    build_codes(g_ac_lum_bits, g_ac_lum_val, g_ac_lum);
    build_codes(g_ac_chr_bits, g_ac_chr_val, g_ac_chr);
    g_tables_built = 1;
}


/* ========================================================== EXIF ===== */
/*
 * A minimal, standards-correct EXIF APP1 block: TIFF header, IFD0, and an
 * Exif sub-IFD. Values of four bytes or fewer live inline in the entry, as
 * TIFF requires; longer ones go in a data area after both directories.
 */

typedef struct {
    uint8_t *buf;
    int      cap;
    int      data;       /* cursor in the data area        */
    int      n0, n1, nth;
    int      ifd0, ifd1, ifd_th;
    int      e0, e1, e_th;
} exifw;

static void ew_u16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)(v >> 8); }
static void ew_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);       p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff); p[3] = (uint8_t)(v >> 24);
}

static int ew_blob(exifw *w, const void *src, int n)
{
    int at = w->data;
    if (at + n > w->cap) return -1;
    memcpy(w->buf + at, src, (size_t)n);
    w->data += n;
    if (w->data & 1) w->data++;      /* keep the data area word aligned */
    return at;
}

/* type: 2 ASCII, 3 SHORT, 4 LONG, 5 RATIONAL */
static void ew_entry(exifw *w, int which, unsigned tag, unsigned type,
                     uint32_t count, const void *data, int inline_bytes)
{
    int *cur = which == 2 ? &w->e_th : which ? &w->e1 : &w->e0;
    uint8_t *p = w->buf + *cur;
    ew_u16(p,     tag);
    ew_u16(p + 2, type);
    ew_u32(p + 4, count);
    if (inline_bytes <= 4) {
        memset(p + 8, 0, 4);
        if (data) memcpy(p + 8, data, (size_t)inline_bytes);
    } else {
        int at = ew_blob(w, data, inline_bytes);
        ew_u32(p + 8, at >= 0 ? (uint32_t)at : 0);
    }
    *cur += 12;
    if (which == 2) w->nth++;
    else if (which) w->n1++;
    else w->n0++;
}

static void ew_ascii(exifw *w, int which, unsigned tag, const char *s)
{
    int n;
    if (!s || !*s) return;
    n = (int)strlen(s) + 1;
    ew_entry(w, which, tag, 2, (uint32_t)n, s, n);
}

static void ew_short(exifw *w, int which, unsigned tag, unsigned v)
{
    uint8_t t[2];
    ew_u16(t, v);
    ew_entry(w, which, tag, 3, 1, t, 2);
}

static void ew_long(exifw *w, int which, unsigned tag, uint32_t v)
{
    uint8_t t[4];
    ew_u32(t, v);
    ew_entry(w, which, tag, 4, 1, t, 4);
}

static void ew_rational(exifw *w, int which, unsigned tag,
                        uint32_t num, uint32_t den)
{
    uint8_t t[8];
    ew_u32(t,     num);
    ew_u32(t + 4, den);
    ew_entry(w, which, tag, 5, 1, t, 8);
}

/* Exposure times are conventionally written as 1/N when shorter than a second. */
static void float_to_rational(float v, uint32_t *num, uint32_t *den)
{
    if (v <= 0.0f)      { *num = 0;  *den = 1; return; }
    if (v < 1.0f)       { *num = 1;  *den = (uint32_t)(1.0f / v + 0.5f); }
    else                { *num = (uint32_t)(v * 100.0f + 0.5f); *den = 100; }
    if (*den == 0) *den = 1;
}

/*
 * Build the whole APP1 payload (starting at "Exif\0\0") and return its
 * length, or 0 if it would not fit.
 */
static int build_exif(uint8_t *out, int cap, const se_jpeg_exif *x,
                      int width, int height)
{
    exifw w;
    uint8_t *tiff;
    int hdr = 6;               /* "Exif\0\0" */
    int total;

    if (cap < 256) return 0;
    memcpy(out, "Exif\0\0", 6);
    tiff = out + hdr;

    memset(&w, 0, sizeof w);
    w.buf = tiff;
    w.cap = cap - hdr;

    /* TIFF header: little endian, first IFD at offset 8 */
    tiff[0] = 'I'; tiff[1] = 'I';
    ew_u16(tiff + 2, 42);
    ew_u32(tiff + 4, 8);

    /*
     * Reserve space for both directories before any data is written.
     * Reserve generously: entries are optional, so the counts are only
     * known at the end, and the terminators are placed then.
     */
    #define IFD0_SLOTS 8
    #define EXIF_SLOTS 10
    #define THUMB_SLOTS 6
    w.ifd0 = 8;
    w.e0   = w.ifd0 + 2;
    w.ifd1 = w.ifd0 + 2 + 12 * IFD0_SLOTS + 4;
    w.e1   = w.ifd1 + 2;
    w.ifd_th = w.ifd1 + 2 + 12 * EXIF_SLOTS + 4;
    w.e_th   = w.ifd_th + 2;
    w.data   = w.ifd_th + 2 + 12 * (x->thumb ? THUMB_SLOTS : 0) + 4;

    ew_ascii(&w, 0, 0x010f, x->make);
    ew_ascii(&w, 0, 0x0110, x->model);
    ew_short(&w, 0, 0x0112, 1);                 /* Orientation: normal */
    ew_ascii(&w, 0, 0x0131, x->software);
    ew_ascii(&w, 0, 0x0132, x->datetime);
    ew_long (&w, 0, 0x8769, (uint32_t)w.ifd1);  /* -> Exif IFD */

    if (x->exposure_time > 0.0f) {
        uint32_t n, d;
        float_to_rational(x->exposure_time, &n, &d);
        ew_rational(&w, 1, 0x829a, n, d);
    }
    if (x->aperture > 0.0f)
        ew_rational(&w, 1, 0x829d, (uint32_t)(x->aperture * 10.0f + 0.5f), 10);
    if (x->iso > 0)
        ew_short(&w, 1, 0x8827, (unsigned)x->iso);
    ew_ascii(&w, 1, 0x9003, x->datetime);       /* DateTimeOriginal  */
    ew_ascii(&w, 1, 0x9004, x->datetime);       /* DateTimeDigitized */
    ew_short(&w, 1, 0xa001, 1);                 /* ColorSpace: sRGB  */
    ew_long (&w, 1, 0xa002, (uint32_t)width);
    ew_long (&w, 1, 0xa003, (uint32_t)height);

    /*
     * Entry counts, and the "no next IFD" terminator immediately after the
     * last entry actually written -- a parser reads `count` entries and
     * then expects the pointer right there, so it cannot sit at the end of
     * the reserved space.
     */
    if (x->thumb && x->thumb_len > 0) {
        int at = ew_blob(&w, x->thumb, x->thumb_len);
        if (at >= 0) {
            ew_short(&w, 2, 0x0103, 6);                  /* JPEG thumbnail */
            ew_long (&w, 2, 0x0201, (uint32_t)at);
            ew_long (&w, 2, 0x0202, (uint32_t)x->thumb_len);
        }
    }

    ew_u16(tiff + w.ifd0, (unsigned)w.n0);
    ew_u32(tiff + w.ifd0 + 2 + 12 * w.n0, w.nth ? (uint32_t)w.ifd_th : 0);
    ew_u16(tiff + w.ifd1, (unsigned)w.n1);
    ew_u32(tiff + w.ifd1 + 2 + 12 * w.n1, 0);
    if (w.nth) {
        ew_u16(tiff + w.ifd_th, (unsigned)w.nth);
        ew_u32(tiff + w.ifd_th + 2 + 12 * w.nth, 0);
    }

    total = hdr + w.data;
    return total;
    #undef IFD0_SLOTS
    #undef EXIF_SLOTS
    #undef THUMB_SLOTS
}

/* ---------------------------------------------------------- output ---- */

static void jo_flush(se_jpeg *j)
{
    if (j->outpos && !j->failed) {
        if (j->sink(j->ctx, j->out, (size_t)j->outpos) != 0) j->failed = 1;
    }
    j->outpos = 0;
}

static void jo_byte(se_jpeg *j, uint8_t b)
{
    if (j->outpos >= (int)sizeof j->out) jo_flush(j);
    j->out[j->outpos++] = b;
}

static void jo_bytes(se_jpeg *j, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) jo_byte(j, p[i]);
}

static void jo_u16(se_jpeg *j, unsigned v)
{
    jo_byte(j, (uint8_t)(v >> 8));
    jo_byte(j, (uint8_t)(v & 0xff));
}

static void jo_bits(se_jpeg *j, uint32_t code, int len)
{
    j->bitcnt += len;
    j->bitbuf |= (uint32_t)code << (24 - j->bitcnt);
    while (j->bitcnt >= 8) {
        uint8_t b = (uint8_t)((j->bitbuf >> 16) & 0xff);
        jo_byte(j, b);
        if (b == 0xff) jo_byte(j, 0x00);   /* byte stuffing */
        j->bitbuf <<= 8;
        j->bitcnt  -= 8;
    }
}

static void jo_bit_flush(se_jpeg *j)
{
    while (j->bitcnt > 0) jo_bits(j, 0x7f, 7);
    j->bitbuf = 0;
    j->bitcnt = 0;
}

/* ------------------------------------------------------------- DCT ---- */

#define CONST_BITS 13
#define PASS1_BITS 2
#define DESCALE(x, n) (((x) + ((int32_t)1 << ((n) - 1))) >> (n))

#define FIX_0_298631336   2446
#define FIX_0_390180644   3196
#define FIX_0_541196100   4433
#define FIX_0_765366865   6270
#define FIX_0_899976223   7373
#define FIX_1_175875602   9633
#define FIX_1_501321110  12299
#define FIX_1_847759065  15137
#define FIX_1_961570560  16069
#define FIX_2_053119869  16819
#define FIX_2_562915447  20995
#define FIX_3_072711026  25172

/* IJG jpeg_fdct_islow, in natural (row-major) order. */
static void fdct(int32_t *d)
{
    int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int32_t tmp10, tmp11, tmp12, tmp13;
    int32_t z1, z2, z3, z4, z5;
    int32_t *p;
    int i;

    p = d;
    for (i = 0; i < 8; i++, p += 8) {
        tmp0 = p[0] + p[7]; tmp7 = p[0] - p[7];
        tmp1 = p[1] + p[6]; tmp6 = p[1] - p[6];
        tmp2 = p[2] + p[5]; tmp5 = p[2] - p[5];
        tmp3 = p[3] + p[4]; tmp4 = p[3] - p[4];

        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;

        p[0] = (tmp10 + tmp11) << PASS1_BITS;
        p[4] = (tmp10 - tmp11) << PASS1_BITS;

        z1 = (tmp12 + tmp13) * FIX_0_541196100;
        p[2] = DESCALE(z1 + tmp13 * FIX_0_765366865, CONST_BITS - PASS1_BITS);
        p[6] = DESCALE(z1 - tmp12 * FIX_1_847759065, CONST_BITS - PASS1_BITS);

        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = (z3 + z4) * FIX_1_175875602;

        tmp4 *= FIX_0_298631336;
        tmp5 *= FIX_2_053119869;
        tmp6 *= FIX_3_072711026;
        tmp7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;

        z3 += z5; z4 += z5;

        p[7] = DESCALE(tmp4 + z1 + z3, CONST_BITS - PASS1_BITS);
        p[5] = DESCALE(tmp5 + z2 + z4, CONST_BITS - PASS1_BITS);
        p[3] = DESCALE(tmp6 + z2 + z3, CONST_BITS - PASS1_BITS);
        p[1] = DESCALE(tmp7 + z1 + z4, CONST_BITS - PASS1_BITS);
    }

    p = d;
    for (i = 0; i < 8; i++, p++) {
        tmp0 = p[8 * 0] + p[8 * 7]; tmp7 = p[8 * 0] - p[8 * 7];
        tmp1 = p[8 * 1] + p[8 * 6]; tmp6 = p[8 * 1] - p[8 * 6];
        tmp2 = p[8 * 2] + p[8 * 5]; tmp5 = p[8 * 2] - p[8 * 5];
        tmp3 = p[8 * 3] + p[8 * 4]; tmp4 = p[8 * 3] - p[8 * 4];

        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;

        p[8 * 0] = DESCALE(tmp10 + tmp11, PASS1_BITS);
        p[8 * 4] = DESCALE(tmp10 - tmp11, PASS1_BITS);

        z1 = (tmp12 + tmp13) * FIX_0_541196100;
        p[8 * 2] = DESCALE(z1 + tmp13 * FIX_0_765366865, CONST_BITS + PASS1_BITS);
        p[8 * 6] = DESCALE(z1 - tmp12 * FIX_1_847759065, CONST_BITS + PASS1_BITS);

        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = (z3 + z4) * FIX_1_175875602;

        tmp4 *= FIX_0_298631336;
        tmp5 *= FIX_2_053119869;
        tmp6 *= FIX_3_072711026;
        tmp7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;

        z3 += z5; z4 += z5;

        p[8 * 7] = DESCALE(tmp4 + z1 + z3, CONST_BITS + PASS1_BITS);
        p[8 * 5] = DESCALE(tmp5 + z2 + z4, CONST_BITS + PASS1_BITS);
        p[8 * 3] = DESCALE(tmp6 + z2 + z3, CONST_BITS + PASS1_BITS);
        p[8 * 1] = DESCALE(tmp7 + z1 + z4, CONST_BITS + PASS1_BITS);
    }
}

/* --------------------------------------------------------- encoding --- */

static int magnitude(int v)
{
    int n = 0;
    if (v < 0) v = -v;
    while (v) { n++; v >>= 1; }
    return n;
}

static void encode_block(se_jpeg *j, int32_t *blk, int qtbl, int comp,
                         const uint32_t *dc, const uint32_t *ac)
{
    int32_t  q[64];
    int      i, k, run, dcdiff, m;
    uint32_t c;

    fdct(blk);

    for (i = 0; i < 64; i++) {
        int32_t  v = blk[i];
        uint32_t r = j->qrecip[qtbl][i];
        /* reciprocal multiply instead of a divide: no hardware divider here */
        if (v >= 0) q[i] =  (int32_t)(((int64_t) v * r + 32768) >> 16);
        else        q[i] = -(int32_t)(((int64_t)-v * r + 32768) >> 16);
    }

    /* --- DC: differential against the previous block of this component -- */
    dcdiff = q[0] - j->dcpred[comp];
    j->dcpred[comp] = q[0];
    m = magnitude(dcdiff);
    c = dc[m];
    jo_bits(j, c & 0xffff, (int)(c >> 16));
    if (m)
        jo_bits(j, (uint32_t)(dcdiff < 0 ? dcdiff - 1 : dcdiff) & ((1u << m) - 1), m);

    /* --- AC, in zig-zag order ------------------------------------------ */
    run = 0;
    for (k = 1; k < 64; k++) {
        int32_t v = q[g_zigzag[k]];
        if (v == 0) { run++; continue; }
        while (run > 15) {
            c = ac[0xf0];                       /* ZRL: 16 zeroes */
            jo_bits(j, c & 0xffff, (int)(c >> 16));
            run -= 16;
        }
        m = magnitude(v);
        c = ac[(run << 4) | m];
        jo_bits(j, c & 0xffff, (int)(c >> 16));
        jo_bits(j, (uint32_t)(v < 0 ? v - 1 : v) & ((1u << m) - 1), m);
        run = 0;
    }
    if (run) {
        c = ac[0x00];                           /* EOB */
        jo_bits(j, c & 0xffff, (int)(c >> 16));
    }
}


/* --------------------------------------------------------- headers ---- */

static int dht_bytes(const uint8_t *bits)
{
    int i, n = 17;
    for (i = 1; i <= 16; i++) n += bits[i];
    return n;
}

static void emit_dht_one(se_jpeg *j, int tc, int th,
                         const uint8_t *bits, const uint8_t *vals)
{
    int i, total = 0;
    jo_byte(j, (uint8_t)((tc << 4) | th));
    for (i = 1; i <= 16; i++) {
        jo_byte(j, bits[i]);
        total += bits[i];
    }
    jo_bytes(j, vals, (size_t)total);
}

/* One DHT marker, the way a 50D JPEG packs its four tables. */
static void emit_dht(se_jpeg *j)
{
    int n = dht_bytes(g_dc_lum_bits) + dht_bytes(g_ac_lum_bits)
          + dht_bytes(g_dc_chr_bits) + dht_bytes(g_ac_chr_bits);
    jo_u16(j, 0xffc4);
    jo_u16(j, (unsigned)(2 + n));
    emit_dht_one(j, 0, 0, g_dc_lum_bits, g_dc_lum_val);
    emit_dht_one(j, 1, 0, g_ac_lum_bits, g_ac_lum_val);
    emit_dht_one(j, 0, 1, g_dc_chr_bits, g_dc_chr_val);
    emit_dht_one(j, 1, 1, g_ac_chr_bits, g_ac_chr_val);
}

se_err se_jpeg_begin(se_jpeg *j, int width, int height, int quality,
                     uint8_t *strip, const se_jpeg_exif *exif,
                     se_jpeg_sink sink, void *ctx)
{
    int i, scale;

    if (width <= 0 || height <= 0 || !strip || !sink) return SE_E_BADFILE;

    memset(j, 0, sizeof *j);
    build_all_codes();

    /* 4:2:2 MCUs are 16 pixels wide. The 50D playback decoder rejects a
     * width that is not a multiple of 16. Drop the extra columns. */
    j->stride = width;
    if (width & 15) width &= ~15;
    if (height & 7) height &= ~7;
    if (width <= 0 || height <= 0) return SE_E_BADFILE;

    j->sink = sink; j->ctx = ctx;
    j->width = width; j->height = height;
    j->strip = strip;
    j->quality = SE_CLAMP(quality, 1, 100);

    /* Standard IJG quality scaling. */
    scale = (j->quality < 50) ? (5000 / j->quality) : (200 - j->quality * 2);
    for (i = 0; i < 64; i++) {
        int l = (g_qt_lum[i] * scale + 50) / 100;
        int c = (g_qt_chr[i] * scale + 50) / 100;
        j->qt[0][i] = (uint8_t)SE_CLAMP(l, 1, 255);
        j->qt[1][i] = (uint8_t)SE_CLAMP(c, 1, 255);
        /*
         * The islow DCT returns coefficients scaled up by 8, so the real
         * divisor is quantval * 8 -- the same adjustment libjpeg makes in
         * jcdctmgr.c. Dividing by quantval alone overflows the Huffman
         * magnitude categories and desynchronises the whole stream.
         */
        j->qrecip[0][i] = (uint32_t)((65536u + 4u * j->qt[0][i]) / (8u * j->qt[0][i]));
        j->qrecip[1][i] = (uint32_t)((65536u + 4u * j->qt[1][i]) / (8u * j->qt[1][i]));
    }

    /* SOI, then EXIF. A 50D file has no JFIF marker in front of it. */
    jo_u16(j, 0xffd8);

    /* APP1 EXIF, which must follow immediately */
    if (exif) {
        int n = build_exif(j->exifbuf, (int)sizeof j->exifbuf, exif, width, height);
        if (n > 0) {
            jo_u16(j, 0xffe1);
            jo_u16(j, (unsigned)(n + 2));
            jo_bytes(j, j->exifbuf, (size_t)n);
        }
    }

    /* DQT, written in zig-zag order as the standard requires */
    jo_u16(j, 0xffdb);
    jo_u16(j, 2 + 2 * (1 + 64));
    jo_byte(j, 0x00);
    for (i = 0; i < 64; i++) jo_byte(j, j->qt[0][g_zigzag[i]]);
    jo_byte(j, 0x01);
    for (i = 0; i < 64; i++) jo_byte(j, j->qt[1][g_zigzag[i]]);

    /* SOF0, 4:2:2: Y is 2x1, Cb and Cr are 1x1. Same factors as a 50D JPEG. */
    jo_u16(j, 0xffc0);
    jo_u16(j, 8 + 3 * 3);
    jo_byte(j, 8);
    jo_u16(j, (unsigned)height);
    jo_u16(j, (unsigned)width);
    jo_byte(j, 3);
    jo_byte(j, 1); jo_byte(j, 0x21); jo_byte(j, 0);
    jo_byte(j, 2); jo_byte(j, 0x11); jo_byte(j, 1);
    jo_byte(j, 3); jo_byte(j, 0x11); jo_byte(j, 1);

    emit_dht(j);

    /* SOS */
    jo_u16(j, 0xffda);
    jo_u16(j, 6 + 2 * 3);
    jo_byte(j, 3);
    jo_byte(j, 1); jo_byte(j, 0x00);
    jo_byte(j, 2); jo_byte(j, 0x11);
    jo_byte(j, 3); jo_byte(j, 0x11);
    jo_byte(j, 0); jo_byte(j, 63); jo_byte(j, 0);

    return j->failed ? SE_E_IO : SE_OK;
}

/* Rec.601, level-shifted by -128. Cb and Cr are already signed. */
static void yuv_of(const uint8_t *p, int *y, int *cb, int *cr)
{
    int r = p[0], g = p[1], b = p[2];
    *y  = (( 19595 * r + 38470 * g +  7471 * b + 32768) >> 16) - 128;
    *cb =  (-11056 * r - 21712 * g + 32768 * b + 32768) >> 16;
    *cr =  ( 32768 * r - 27440 * g -  5328 * b + 32768) >> 16;
}

/* Encode the 8 rows currently held in j->strip as 16x8 MCUs. */
static void encode_strip(se_jpeg *j)
{
    int w = j->width;
    int mcu, by, col;

    for (mcu = 0; mcu * 16 < w; mcu++) {
        int32_t Y0[64], Y1[64], Cb[64], Cr[64];
        int x0 = mcu * 16;
        int cb0 = 0, cr0 = 0;

        for (by = 0; by < 8; by++) {
            const uint8_t *src = j->strip + (size_t)by * w * 3;
            for (col = 0; col < 16; col++) {
                int x = x0 + col;
                int y, cb, cr;
                if (x >= w) x = w - 1;
                yuv_of(src + x * 3, &y, &cb, &cr);
                if (col < 8) Y0[by * 8 + col] = y;
                else         Y1[by * 8 + col - 8] = y;
                if ((col & 1) == 0) {
                    cb0 = cb;
                    cr0 = cr;
                } else {
                    int i = col >> 1;
                    Cb[by * 8 + i] = (cb0 + cb) >> 1;
                    Cr[by * 8 + i] = (cr0 + cr) >> 1;
                }
            }
        }

        encode_block(j, Y0, 0, 0, g_dc_lum, g_ac_lum);
        encode_block(j, Y1, 0, 0, g_dc_lum, g_ac_lum);
        encode_block(j, Cb, 1, 1, g_dc_chr, g_ac_chr);
        encode_block(j, Cr, 1, 2, g_dc_chr, g_ac_chr);
    }
}


se_err se_jpeg_write_rows(se_jpeg *j, const uint8_t *rgb, int nrows)
{
    int i;
    for (i = 0; i < nrows; i++) {
        if (j->rows_done >= j->height) break;
        memcpy(j->strip + (size_t)j->strip_rows * j->width * 3,
               rgb + (size_t)i * (size_t)j->stride * 3,
               (size_t)j->width * 3);
        j->strip_rows++;
        j->rows_done++;
        if (j->strip_rows == 8) {
            encode_strip(j);
            j->strip_rows = 0;
        }
    }
    return j->failed ? SE_E_IO : SE_OK;
}

se_err se_jpeg_end(se_jpeg *j)
{
    if (j->strip_rows) {
        /* pad the final partial MCU row by repeating the last line */
        int w = j->width;
        while (j->strip_rows < 8) {
            memcpy(j->strip + (size_t)j->strip_rows * w * 3,
                   j->strip + (size_t)(j->strip_rows - 1) * w * 3,
                   (size_t)w * 3);
            j->strip_rows++;
        }
        encode_strip(j);
        j->strip_rows = 0;
    }
    jo_bit_flush(j);
    jo_u16(j, 0xffd9);   /* EOI */
    jo_flush(j);
    return j->failed ? SE_E_IO : SE_OK;
}
