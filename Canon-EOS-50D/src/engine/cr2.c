#include "cr2.h"
#include "se_libc.h"

/* ===================================================================== */
/* TIFF                                                                   */
/* ===================================================================== */

#define TIFF_BYTE 1
#define TIFF_ASCII 2
#define TIFF_SHORT 3
#define TIFF_LONG 4
#define TIFF_RATIONAL 5
#define TIFF_UNDEFINED 7
#define TIFF_SSHORT 8
#define TIFF_SLONG 9
#define TIFF_SRATIONAL 10

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t voff;   /* absolute file offset of the first value */
} tif_entry;

static size_t tiff_typesize(uint16_t t)
{
    switch (t) {
    case TIFF_BYTE: case TIFF_ASCII: case TIFF_UNDEFINED:          return 1;
    case TIFF_SHORT: case TIFF_SSHORT:                             return 2;
    case TIFF_LONG: case TIFF_SLONG:                               return 4;
    case TIFF_RATIONAL: case TIFF_SRATIONAL:                       return 8;
    default:                                                       return 0;
    }
}

/* Read entry `idx` of the IFD at `ifd_off`. Returns 0 on success. */
static int tiff_entry(se_rd *r, uint32_t ifd_off, int idx, tif_entry *e)
{
    size_t base = (size_t)ifd_off + 2 + (size_t)idx * 12;
    size_t vsz;
    se_rd_seek(r, base);
    e->tag   = se_rd_u16(r);
    e->type  = se_rd_u16(r);
    e->count = se_rd_u32(r);
    vsz = tiff_typesize(e->type) * (size_t)e->count;
    if (vsz > 4) {
        e->voff = se_rd_u32(r);
    } else {
        e->voff = (uint32_t)(base + 8);   /* value is inline in the entry */
    }
    return r->err ? -1 : 0;
}

static int tiff_count(se_rd *r, uint32_t ifd_off)
{
    int n;
    se_rd_seek(r, ifd_off);
    n = (int)se_rd_u16(r);
    return r->err ? -1 : n;
}

/* Fetch value `i` of an entry as an unsigned 32-bit integer. */
static uint32_t tiff_val(se_rd *r, const tif_entry *e, uint32_t i)
{
    size_t sz = tiff_typesize(e->type);
    if (i >= e->count || sz == 0) return 0;
    se_rd_seek(r, e->voff + i * sz);
    switch (sz) {
    case 1: return se_rd_u8(r);
    case 2: return se_rd_u16(r);
    default: return se_rd_u32(r);
    }
}

/* Fetch value `i` of a RATIONAL entry as a float. */
static float tiff_rational(se_rd *r, const tif_entry *e, uint32_t i)
{
    uint32_t num, den;
    if (e->type != TIFF_RATIONAL && e->type != TIFF_SRATIONAL) return 0.0f;
    if (i >= e->count) return 0.0f;
    se_rd_seek(r, e->voff + i * 8);
    num = se_rd_u32(r);
    den = se_rd_u32(r);
    if (!den) return 0.0f;
    if (e->type == TIFF_SRATIONAL)
        return (float)(int32_t)num / (float)(int32_t)den;
    return (float)num / (float)den;
}

static void tiff_string(se_rd *r, const tif_entry *e, char *dst, size_t dstsz)
{
    size_t n = e->count;
    size_t i;
    if (n > dstsz - 1) n = dstsz - 1;
    se_rd_seek(r, e->voff);
    for (i = 0; i < n; i++) dst[i] = (char)se_rd_u8(r);
    dst[n] = 0;
    /* trim trailing spaces/NULs that Canon likes to pad with */
    while (n && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
}

/* ===================================================================== */
/* Camera database                                                        */
/* ===================================================================== */
/*
 * XYZ(D65) -> camera RGB matrices, x1e4, matching the Adobe/dcraw values.
 * These describe the sensor's own spectral response; using them is what
 * lets us go from raw to a real colour space without Canon's firmware.
 */
typedef struct {
    const char *model;
    int   white;          /* saturation point at the coded bit depth */
    int   bits;
    short cam_xyz[9];
} se_cam_entry;

static const se_cam_entry g_cams[] = {
    { "Canon EOS 5D",          0x0e6c, 12, { 6347,-479,-972,-8297,15954,2480,-1968,2131,7649 } },
    { "Canon EOS 50D",         0x3cf0, 14, { 4920, 616,-593,-6493,13964,2784,-1774,3178,7005 } },
    { "Canon EOS 5D Mark II",  0x3cf0, 14, { 4716, 603,-830,-7798,15474,2480,-1496,1937,6651 } },
};

static const se_cam_entry *cam_lookup(const char *model)
{
    size_t i;
    /* longest match first so "5D Mark II" never matches the "5D" prefix */
    const se_cam_entry *best = NULL;
    size_t bestlen = 0;
    for (i = 0; i < SE_ARRAY_LEN(g_cams); i++) {
        size_t n = strlen(g_cams[i].model);
        if (strncmp(model, g_cams[i].model, n) == 0 && n > bestlen) {
            best = &g_cams[i];
            bestlen = n;
        }
    }
    return best;
}

/* ===================================================================== */
/* Metadata parse                                                         */
/* ===================================================================== */

static void parse_makernote(se_rd *r, uint32_t off, se_cr2_info *out)
{
    int n = tiff_count(r, off), i;
    if (n < 0 || n > 512) return;

    for (i = 0; i < n; i++) {
        tif_entry e;
        if (tiff_entry(r, off, i, &e) != 0) return;

        switch (e.tag) {
        case 0x00e0: /* SensorInfo */
            if (e.count >= 9) {
                /* The raw IFD of a CR2 carries no ImageWidth/ImageLength;
                 * these are the only authoritative frame dimensions. */
                out->raw_width   = (int)tiff_val(r, &e, 1);
                out->raw_height  = (int)tiff_val(r, &e, 2);
                out->crop_left   = (int)tiff_val(r, &e, 5);
                out->crop_top    = (int)tiff_val(r, &e, 6);
                out->crop_right  = (int)tiff_val(r, &e, 7);
                out->crop_bottom = (int)tiff_val(r, &e, 8);
            }
            break;

        case 0x4001: { /* ColorData - as-shot white balance lives inside */
            /*
             * Canon versioned this block by changing its length, and the
             * position of WB_RGGBLevelsAsShot moved with it. Offsets are
             * word indices, cross-checked against ExifTool's Canon tables.
             */
            uint32_t idx;
            switch (e.count) {
            case 582:  idx = 25; break;   /* ColorData1: 20D, 350D          */
            case 653:  idx = 24; break;   /* ColorData2: 1D Mk II           */
            case 796:  idx = 63; break;   /* ColorData3: 5D, 30D, 400D      */
            case 674: case 692: case 702:
            case 1227: case 1250: case 1251:
            case 1337: case 1338: case 1346:
                       idx = 63; break;   /* ColorData4: 50D, 5D Mk II, ... */
            default:
                idx = 63;
                se_log("cr2: unknown ColorData length %u, assuming offset 63",
                       (unsigned)e.count);
                break;
            }
            if (idx + 4 <= e.count) {
                /* stored as R, G1, G2, B */
                out->wb[0] = (float)tiff_val(r, &e, idx + 0);
                out->wb[1] = (float)tiff_val(r, &e, idx + 1);
                out->wb[3] = (float)tiff_val(r, &e, idx + 2);
                out->wb[2] = (float)tiff_val(r, &e, idx + 3);
                if (out->wb[0] > 0 && out->wb[1] > 0 && out->wb[2] > 0)
                    out->wb_present = 1;
            }
            /*
             * The presets follow as-shot on a 5-word stride: four RGGB
             * levels then a colour temperature. Verified against ExifTool
             * on a real EOS 5D frame.
             */
            {
                int p;
                for (p = 0; p < SE_WB_COUNT; p++) {
                    uint32_t o = idx + (uint32_t)p * 5;
                    if (o + 4 > e.count) break;
                    out->wb_preset[p][0] = (float)tiff_val(r, &e, o + 0);
                    out->wb_preset[p][1] = (float)tiff_val(r, &e, o + 1);
                    out->wb_preset[p][3] = (float)tiff_val(r, &e, o + 2);
                    out->wb_preset[p][2] = (float)tiff_val(r, &e, o + 3);
                    if (out->wb_preset[p][0] > 0) out->wb_presets_present = 1;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

static void parse_exif(se_rd *r, uint32_t off, se_cr2_info *out)
{
    int n = tiff_count(r, off), i;
    if (n < 0 || n > 256) return;

    for (i = 0; i < n; i++) {
        tif_entry e;
        if (tiff_entry(r, off, i, &e) != 0) return;
        switch (e.tag) {
        case 0x829a: out->exposure_time = tiff_rational(r, &e, 0); break;
        case 0x829d: out->aperture      = tiff_rational(r, &e, 0); break;
        case 0x8827: out->iso           = (int)tiff_val(r, &e, 0); break;
        case 0x9003: tiff_string(r, &e, out->datetime, sizeof out->datetime); break;
        case 0x927c: parse_makernote(r, e.voff, out);              break;
        default: break;
        }
    }
}

static void parse_ifd0(se_rd *r, uint32_t off, se_cr2_info *out)
{
    int n = tiff_count(r, off), i;
    if (n < 0 || n > 256) return;

    for (i = 0; i < n; i++) {
        tif_entry e;
        if (tiff_entry(r, off, i, &e) != 0) return;
        switch (e.tag) {
        case 0x010f: tiff_string(r, &e, out->make,  sizeof out->make);  break;
        case 0x0110: tiff_string(r, &e, out->model, sizeof out->model); break;
        case 0x8769: parse_exif(r, tiff_val(r, &e, 0), out);            break;
        default: break;
        }
    }
}

static se_err parse_raw_ifd(se_rd *r, uint32_t off, se_cr2_info *out)
{
    int n = tiff_count(r, off), i;
    int compression = 0;
    if (n < 0 || n > 256) return SE_E_BADFILE;

    for (i = 0; i < n; i++) {
        tif_entry e;
        if (tiff_entry(r, off, i, &e) != 0) return SE_E_TRUNCATED;
        switch (e.tag) {
        case 0x0100: out->raw_width    = (int)tiff_val(r, &e, 0);      break;
        case 0x0101: out->raw_height   = (int)tiff_val(r, &e, 0);      break;
        case 0x0103: compression       = (int)tiff_val(r, &e, 0);      break;
        case 0x0111: out->strip_offset = tiff_val(r, &e, 0);           break;
        case 0x0117: out->strip_bytes  = tiff_val(r, &e, 0);           break;
        case 0xc640: /* CR2 slice descriptor */
            if (e.count >= 3) {
                out->slice_count = (int)tiff_val(r, &e, 0);
                out->slice_width = (int)tiff_val(r, &e, 1);
                out->slice_last  = (int)tiff_val(r, &e, 2);
            }
            break;
        default: break;
        }
    }

    if (!out->raw_width && out->slice_count >= 0 && out->slice_width > 0)
        out->raw_width = out->slice_count * out->slice_width + out->slice_last;

    if (compression != 6) {
        se_log("cr2: raw IFD compression=%d, expected 6 (old JPEG)", compression);
        return SE_E_UNSUPPORTED;
    }
    if (!out->strip_offset || !out->strip_bytes)
        return SE_E_BADFILE;
    return SE_OK;
}

se_err se_cr2_parse(const void *buf, size_t len, se_cr2_info *out)
{
    const uint8_t *b = (const uint8_t *)buf;
    se_rd r;
    uint32_t ifd0_off, raw_ifd_off;
    const se_cam_entry *cam;
    se_err rc;

    memset(out, 0, sizeof *out);
    if (len < 16) return SE_E_TRUNCATED;

    /* CR2 header: "II*\0" <ifd0> "CR" <major> <minor> <raw ifd> */
    if (!(b[0] == 'I' && b[1] == 'I' && b[2] == 0x2a && b[3] == 0x00))
        return SE_E_BADFILE;
    if (!(b[8] == 'C' && b[9] == 'R'))
        return SE_E_BADFILE;

    se_rd_init(&r, buf, len, 0 /* little endian */);
    se_rd_seek(&r, 4);
    ifd0_off = se_rd_u32(&r);
    se_rd_seek(&r, 12);
    raw_ifd_off = se_rd_u32(&r);
    if (r.err) return SE_E_TRUNCATED;

    parse_ifd0(&r, ifd0_off, out);
    rc = parse_raw_ifd(&r, raw_ifd_off, out);
    if (rc != SE_OK) return rc;

    /* Fall back to the full frame if SensorInfo was missing. */
    if (out->crop_right <= out->crop_left || out->crop_bottom <= out->crop_top) {
        out->crop_left = out->crop_top = 0;
        out->crop_right  = out->raw_width  - 1;
        out->crop_bottom = out->raw_height - 1;
    }
    out->active_width  = out->crop_right  - out->crop_left + 1;
    out->active_height = out->crop_bottom - out->crop_top  + 1;

    /*
     * Canon's CFA is RGGB at the top-left of the *raw* frame; the phase of
     * the active area therefore flips with the parity of the crop origin.
     */
    out->cfa_phase = ((out->crop_top & 1) << 1) | (out->crop_left & 1);

    cam = cam_lookup(out->model);
    if (cam) {
        int i;
        out->white = cam->white;
        out->bits  = cam->bits;
        for (i = 0; i < 9; i++) out->cam_xyz[i] = cam->cam_xyz[i] / 10000.0f;
        out->cam_xyz_present = 1;
    } else {
        se_log("cr2: unknown model '%s', guessing 12-bit", out->model);
        out->bits  = 12;
        out->white = 4095;
    }

    if (!out->wb_present) {
        out->wb[0] = out->wb[1] = out->wb[2] = out->wb[3] = 1024.0f;
    }
    return SE_OK;
}

/* ===================================================================== */
/* Lossless JPEG (SOF3) decoder                                           */
/* ===================================================================== */

#define JPEG_FAST_BITS 9
#define JPEG_MAX_COMP  4

typedef struct {
    /* canonical tables */
    uint8_t  nbits[17];                      /* count of codes per length */
    uint8_t  huffval[256];
    int32_t  mincode[18], maxcode[18], valptr[18];
    /* fast path: (length << 8) | symbol, or 0 for "take the slow path" */
    uint16_t fast[1 << JPEG_FAST_BITS];
    int      present;
} jhuff;

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
    uint32_t       buf;    /* MSB-aligned bit reservoir */
    int            cnt;    /* valid bits in buf         */
    int            marker; /* hit a marker / end of data */

    /*
     * Optional streaming source. When `read` is set, `p` points at a small
     * window rather than at the whole strip, and it is refilled as the
     * decoder consumes it. This is what lets the camera decode an 11 MB
     * coded frame: Magic Lantern shrinks Canon's heap to 7 MB on this body,
     * so the file simply cannot be held whole.
     */
    se_cr2_read_fn read;
    void          *rctx;
    uint8_t       *win;        /* the window, JB_WINDOW bytes */
    uint32_t       next_off;   /* next absolute file offset to read */
    uint32_t       remaining;  /* coded bytes not yet windowed */

    /*
     * A refill that could not be satisfied, which is NOT the end of the
     * data. The line below used to read "I/O failed: behave like end of
     * data", and behaving like the end of data means the decoder finishes
     * the frame from whatever it last held and reports success.
     *
     * This path streams eleven megabytes off the card -- seven times the
     * reads the preview path makes -- so it meets a stalled card far more
     * often, and the note in the handoff that the raw pipeline "never
     * decoded correctly on the body" is exactly what this would look like.
     * The same mistake in jpeg_dec.c produced a plausible, wrong frame that
     * reached the camera and was reported rc=0.
     */
    int            ioerr;
} jbits;

#define JB_WINDOW 8192

static void jb_init(jbits *b, const uint8_t *p, size_t len)
{
    b->p = p; b->len = len; b->pos = 0;
    b->buf = 0; b->cnt = 0; b->marker = 0;
    b->read = 0; b->rctx = 0; b->win = 0;
    b->next_off = 0; b->remaining = 0; b->ioerr = 0;
}

/*
 * Pull the next chunk of coded data into the window, keeping whatever has
 * not been consumed yet. The unread tail matters: jb_fill has to be able to
 * look one byte past the one it is taking, to tell a stuffed 0xFF00 from a
 * real marker, and that pair can straddle a refill.
 */
static void jb_refill(jbits *b)
{
    size_t   keep = b->len - b->pos;
    uint32_t want;

    if (!b->read || !b->remaining) return;

    /*
     * Move the unread tail down. A forward loop rather than memcpy because
     * the regions overlap, and rather than memmove because that is not in
     * the engine's libc surface -- `keep` is at most one byte here anyway,
     * since the refill is triggered the moment fewer than two remain.
     */
    {
        size_t k;
        for (k = 0; k < keep; k++) b->win[k] = b->win[b->pos + k];
    }
    want = (uint32_t)(JB_WINDOW - keep);
    if (want > b->remaining) want = b->remaining;

    if (b->read(b->rctx, b->next_off, b->win + keep, want) != 0) {
        b->ioerr = 1;              /* a failure, not the end of the data */
        b->remaining = 0;
        return;
    }
    b->next_off  += want;
    b->remaining -= want;
    b->p   = b->win;
    b->len = keep + want;
    b->pos = 0;
}

/* Top up the reservoir to at least 25 bits, unstuffing 0xFF00 as we go. */
static void jb_fill(jbits *b)
{
    while (b->cnt <= 24) {
        uint32_t c = 0xff;   /* pad with 1-bits past the end, like libjpeg */
        /* Keep at least two bytes available so the 0xFF lookahead below
         * never falls off the end of the window. */
        if (b->pos + 1 >= b->len) jb_refill(b);
        if (b->pos < b->len) {
            c = b->p[b->pos++];
            if (c == 0xff) {
                uint8_t d = (b->pos < b->len) ? b->p[b->pos] : 0xd9;
                if (d == 0x00) {
                    b->pos++;             /* stuffed byte */
                } else {
                    b->pos--;             /* real marker: stop consuming */
                    b->marker = 1;
                    c = 0;
                }
            }
        } else {
            b->marker = 1;
            c = 0;
        }
        b->buf |= c << (24 - b->cnt);
        b->cnt += 8;
    }
}

static void jb_skip(jbits *b, int n)
{
    b->buf <<= n;
    b->cnt  -= n;
}

static uint32_t jb_get(jbits *b, int n)
{
    uint32_t v;
    if (n == 0) return 0;
    if (b->cnt < n) jb_fill(b);
    v = b->buf >> (32 - n);
    jb_skip(b, n);
    return v;
}

static void jhuff_build(jhuff *h)
{
    int l, i, k = 0, code = 0;
    int32_t huffcode[257];

    /* Canonical code assignment: codes of each length are consecutive, and
     * each step up in length shifts the running code left by one. */
    for (l = 1; l <= 16; l++) {
        h->valptr[l]  = k;
        h->mincode[l] = code;
        for (i = 0; i < h->nbits[l]; i++) huffcode[k++] = code++;
        h->maxcode[l] = h->nbits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;

    memset(h->fast, 0, sizeof h->fast);
    k = 0;
    for (l = 1; l <= 16; l++) {
        for (i = 0; i < h->nbits[l]; i++, k++) {
            if (l <= JPEG_FAST_BITS) {
                int32_t base = huffcode[k] << (JPEG_FAST_BITS - l);
                int32_t j, span = 1 << (JPEG_FAST_BITS - l);
                for (j = 0; j < span; j++)
                    h->fast[base + j] = (uint16_t)((l << 8) | h->huffval[k]);
            }
        }
    }
    h->present = 1;
}

static int jhuff_decode(jbits *b, const jhuff *h)
{
    uint32_t look;
    int l, code;

    if (b->cnt < 16) jb_fill(b);
    look = b->buf >> (32 - JPEG_FAST_BITS);
    if (h->fast[look]) {
        uint16_t f = h->fast[look];
        jb_skip(b, f >> 8);
        return f & 0xff;
    }

    /* slow path for codes longer than JPEG_FAST_BITS */
    code = 0;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | (int)jb_get(b, 1);
        if (h->nbits[l] && code <= h->maxcode[l])
            return h->huffval[h->valptr[l] + (code - h->mincode[l])];
    }
    return 0;
}

/* Decode one DC difference: a magnitude category then that many raw bits. */
static int jhuff_diff(jbits *b, const jhuff *h)
{
    int s = jhuff_decode(b, h);
    int v;
    if (s == 0)  return 0;
    if (s == 16) return -32768;          /* lossless JPEG special case */
    if (s > 16)  return 0;
    v = (int)jb_get(b, s);
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
}

typedef struct {
    int      prec;                    /* P: sample precision      */
    int      high, wide;              /* Y, X from SOF3           */
    int      clrs;                    /* Nf: components per group */
    int      psv;                     /* predictor selector       */
    int      pt;                      /* point transform          */
    int      restart;                 /* MCU rows between RSTn    */
    uint8_t  comp_td[JPEG_MAX_COMP];  /* DC table index per comp  */
    jhuff    huff[JPEG_MAX_COMP];
    size_t   scan_off;                /* file offset of entropy data */
} jhead;

static se_err ljpeg_header(const uint8_t *p, size_t len, jhead *jh)
{
    size_t pos = 0;
    int i;

    memset(jh, 0, sizeof *jh);
    jh->restart = 0x7fffffff;

    if (len < 4 || p[0] != 0xff || p[1] != 0xd8) {
        se_log("ljpeg: missing SOI");
        return SE_E_BADFILE;
    }
    pos = 2;

    for (;;) {
        uint16_t marker, seglen;
        size_t   seg;

        if (pos + 4 > len) return SE_E_TRUNCATED;
        if (p[pos] != 0xff) return SE_E_CORRUPT;
        while (pos < len && p[pos] == 0xff) pos++;      /* fill bytes */
        if (pos >= len) return SE_E_TRUNCATED;
        marker = p[pos++];
        if (marker == 0xd8 || marker == 0xd9) continue;

        if (pos + 2 > len) return SE_E_TRUNCATED;
        seglen = (uint16_t)((p[pos] << 8) | p[pos + 1]);
        if (seglen < 2 || pos + seglen > len) return SE_E_TRUNCATED;
        seg = pos + 2;

        switch (marker) {
        case 0xc3: {  /* SOF3: lossless, Huffman */
            if (seg + 6 > len) return SE_E_TRUNCATED;
            jh->prec = p[seg];
            jh->high = (p[seg + 1] << 8) | p[seg + 2];
            jh->wide = (p[seg + 3] << 8) | p[seg + 4];
            jh->clrs = p[seg + 5];
            if (jh->clrs < 1 || jh->clrs > JPEG_MAX_COMP) {
                se_log("ljpeg: %d components, unsupported", jh->clrs);
                return SE_E_UNSUPPORTED;
            }
            for (i = 0; i < jh->clrs; i++) {
                size_t c = seg + 6 + (size_t)i * 3;
                if (c + 3 > len) return SE_E_TRUNCATED;
                if (p[c + 1] != 0x11) {
                    se_log("ljpeg: component %d subsampled (%02x)", i, p[c + 1]);
                    return SE_E_UNSUPPORTED;
                }
            }
            break;
        }

        case 0xc4: {  /* DHT */
            size_t c = seg;
            while (c < pos + seglen) {
                int tc, th, total = 0, l;
                jhuff *h;
                if (c + 17 > len) return SE_E_TRUNCATED;
                tc = p[c] >> 4;
                th = p[c] & 15;
                c++;
                if (tc != 0 || th >= JPEG_MAX_COMP) {
                    se_log("ljpeg: DHT class=%d id=%d unsupported", tc, th);
                    return SE_E_UNSUPPORTED;
                }
                h = &jh->huff[th];
                memset(h, 0, sizeof *h);
                for (l = 1; l <= 16; l++) {
                    h->nbits[l] = p[c + l - 1];
                    total += h->nbits[l];
                }
                c += 16;
                if (total > 256 || c + (size_t)total > len) return SE_E_TRUNCATED;
                for (l = 0; l < total; l++) h->huffval[l] = p[c + l];
                c += total;
                jhuff_build(h);
            }
            break;
        }

        case 0xdd:   /* DRI */
            if (seg + 2 <= len) {
                int ri = (p[seg] << 8) | p[seg + 1];
                if (ri) jh->restart = ri;
            }
            break;

        case 0xda: {  /* SOS: the scan header, then entropy data */
            int ns;
            size_t c = seg;
            if (c + 1 > len) return SE_E_TRUNCATED;
            ns = p[c++];
            if (ns != jh->clrs) {
                se_log("ljpeg: SOS has %d components, SOF had %d", ns, jh->clrs);
                return SE_E_UNSUPPORTED;
            }
            for (i = 0; i < ns; i++) {
                if (c + 2 > len) return SE_E_TRUNCATED;
                jh->comp_td[i] = p[c + 1] >> 4;
                c += 2;
            }
            if (c + 3 > len) return SE_E_TRUNCATED;
            jh->psv = p[c];
            jh->pt  = p[c + 2] & 15;
            jh->scan_off = pos + seglen;
            if (jh->psv != 1) {
                /*
                 * Canon only ever emits predictor 1. Anything else means we
                 * are looking at a file we do not understand, and guessing
                 * would silently corrupt every pixel.
                 */
                se_log("ljpeg: predictor %d unsupported (Canon uses 1)", jh->psv);
                return SE_E_UNSUPPORTED;
            }
            for (i = 0; i < jh->clrs; i++) {
                if (!jh->huff[jh->comp_td[i]].present) {
                    se_log("ljpeg: component %d references empty table %d",
                           i, jh->comp_td[i]);
                    return SE_E_CORRUPT;
                }
            }
            return SE_OK;
        }

        default:
            break;
        }
        pos += seglen;
    }
}

/* ===================================================================== */
/* Full raw decode                                                        */
/* ===================================================================== */

static se_err cr2_decode_core(const void *buf, size_t len,
                              se_cr2_read_fn rdfn, void *rctx,
                              const se_cr2_info *info,
                              se_cr2_sink sink, void *ctx, se_arena *scratch)
{
    const uint8_t *strip;
    size_t striplen;
    jhead *jh;            /* ~5 KB of Huffman tables: arena, not stack */
    jbits  bits;
    se_err rc;
    uint16_t *rowbuf;
    int vpred[JPEG_MAX_COMP];
    int jrow, jcol, c, jwide;
    int last_pct = -1;
    long expected;
    uint8_t *hdrbuf = 0;

    jh = SE_NEW(scratch, jhead, 1);
    if (!jh) return SE_E_NOMEM;

    if (rdfn) {
        /*
         * Streaming: pull just enough of the front of the strip to parse the
         * markers. SOI through SOS is a few hundred bytes for a Canon frame
         * -- two or four Huffman tables and a SOF3 -- so a window this size
         * is far more than the header can need.
         */
        uint32_t want = info->strip_bytes < 8192u ? info->strip_bytes : 8192u;
        /* Aligned, not SE_NEW'd: SE_NEW would align a uint8_t array to 1,
         * and this is a buffer a storage driver reads into. */
        hdrbuf = (uint8_t *)se_arena_alloc(scratch, 8192, 32);
        if (!hdrbuf) return SE_E_NOMEM;
        if (rdfn(rctx, info->strip_offset, hdrbuf, want) != 0) return SE_E_IO;
        strip    = hdrbuf;
        striplen = want;
    } else {
        if ((size_t)info->strip_offset + info->strip_bytes > len)
            return SE_E_TRUNCATED;
        strip    = (const uint8_t *)buf + info->strip_offset;
        striplen = info->strip_bytes;
    }

    rc = ljpeg_header(strip, striplen, jh);
    if (rc != SE_OK) return rc;

    jwide = jh->wide * jh->clrs;
    expected = (long)info->raw_width * info->raw_height;

    se_log("ljpeg: %dx%d x%d comps, %d-bit, pt=%d -> %ld samples (need %ld)",
           jh->wide, jh->high, jh->clrs, jh->prec, jh->pt,
           (long)jwide * jh->high, expected);

    if ((long)jwide * jh->high != expected) {
        se_log("ljpeg: sample count mismatch, refusing to guess the layout");
        return SE_E_CORRUPT;
    }

    rowbuf = SE_NEW(scratch, uint16_t, jwide);
    if (!rowbuf) return SE_E_NOMEM;

    if (rdfn) {
        /*
         * Resynchronising after a restart interval means scanning forward
         * for a marker, which cannot be done through a window that only
         * moves forwards. Canon never emits them, so refuse rather than
         * decode the rest of the frame out of step.
         */
        if (jh->restart != 0x7fffffff) {
            se_log("ljpeg: restart intervals are not supported when streaming");
            return SE_E_UNSUPPORTED;
        }
        jb_init(&bits, 0, 0);
        bits.win = (uint8_t *)se_arena_alloc(scratch, JB_WINDOW, 32);
        if (!bits.win) return SE_E_NOMEM;
        bits.read      = rdfn;
        bits.rctx      = rctx;
        bits.next_off  = info->strip_offset + (uint32_t)jh->scan_off;
        bits.remaining = info->strip_bytes - (uint32_t)jh->scan_off;
        bits.p         = bits.win;
        bits.len = bits.pos = 0;
        jb_refill(&bits);
    } else {
        jb_init(&bits, strip + jh->scan_off, striplen - jh->scan_off);
    }
    for (c = 0; c < JPEG_MAX_COMP; c++) vpred[c] = 1 << (jh->prec - 1);

    for (jrow = 0; jrow < jh->high; jrow++) {
        uint16_t *rp = rowbuf;

        /*
         * Restart intervals reset both the predictors and the bit reservoir.
         * Canon does not use them, but a stray DRI would otherwise desync
         * the whole frame silently.
         */
        if (jh->restart != 0x7fffffff && jrow && (jrow % jh->restart) == 0) {
            for (c = 0; c < JPEG_MAX_COMP; c++) vpred[c] = 1 << (jh->prec - 1);
            bits.cnt = 0;
            bits.buf = 0;
            bits.marker = 0;
            while (bits.pos + 1 < bits.len &&
                   !(bits.p[bits.pos] == 0xff &&
                     bits.p[bits.pos + 1] >= 0xd0 && bits.p[bits.pos + 1] <= 0xd7))
                bits.pos++;
            bits.pos += 2;
        }

        for (jcol = 0; jcol < jh->wide; jcol++) {
            for (c = 0; c < jh->clrs; c++) {
                int diff = jhuff_diff(&bits, &jh->huff[jh->comp_td[c]]);
                int pred;
                if (jcol) {
                    pred = rp[-jh->clrs];
                } else {
                    /* start of a line predicts from the sample above */
                    pred = vpred[c];
                    vpred[c] += diff;
                }
                /*
                 * Shift as unsigned: on a corrupt file the predictor can
                 * run negative, and left-shifting a negative int is
                 * undefined behaviour even though every real compiler
                 * happens to do the arithmetic shift we want.
                 */
                *rp++ = (uint16_t)((uint32_t)(pred + diff) << jh->pt);
            }
        }

        /*
         * Checked once per row: a frame that lost its data stops within a
         * row of where it happened, instead of being completed from stale
         * state and handed back as though it were the picture.
         */
        if (bits.ioerr) return SE_E_IO;

        /* --- place this JPEG row into the raw frame --------------------- */
        if (info->slice_count == 0) {
            /* No slicing: the JPEG raster is the raw raster. */
            if (sink(ctx, jrow, 0, jwide, rowbuf) != 0) return SE_E_IO;
        } else {
            /*
             * Sliced CR2: the JPEG is a sequence of vertical strips laid out
             * one after another, so a single JPEG row spans several raw rows.
             *
             * Rather than place each sample on its own, gather maximal runs
             * of samples that land on the same row in consecutive columns and
             * hand each run to the sink in one call. The mapping below is
             * piecewise linear in jcol, so these runs are long -- a complete
             * half-row apiece for a two-slice Canon frame -- which is what
             * lets a file-backed sink write sequentially.
             */
            int run_row = -1, run_col0 = 0, run_n = 0;

            for (jcol = 0; jcol < jwide; jcol++) {
                long jidx = (long)jrow * jwide + jcol;
                int  i    = (int)(jidx / ((long)info->slice_width * jh->high));
                int  j, row, col, w;
                if ((j = (i >= info->slice_count)) != 0) i = info->slice_count;
                jidx -= (long)i * info->slice_width * jh->high;
                w   = j ? info->slice_last : info->slice_width;
                row = (int)(jidx / w);
                col = (int)(jidx % w) + i * info->slice_width;

                if (row >= info->raw_height || col >= info->raw_width) {
                    /* Out of frame: close any open run and skip. */
                    if (run_n && sink(ctx, run_row, run_col0, run_n,
                                      rowbuf + (jcol - run_n)) != 0)
                        return SE_E_IO;
                    run_n = 0; run_row = -1;
                    continue;
                }

                if (run_n && row == run_row && col == run_col0 + run_n) {
                    run_n++;                      /* extend the current run */
                    continue;
                }
                if (run_n && sink(ctx, run_row, run_col0, run_n,
                                  rowbuf + (jcol - run_n)) != 0)
                    return SE_E_IO;
                run_row = row; run_col0 = col; run_n = 1;
            }
            if (run_n && sink(ctx, run_row, run_col0, run_n,
                              rowbuf + (jwide - run_n)) != 0)
                return SE_E_IO;
        }

        if (jh->high > 0) {
            int pct = jrow * 100 / jh->high;
            if (pct != last_pct) { se_progress("decode", pct); last_pct = pct; }
        }
    }

    if (bits.marker && jrow < jh->high)
        se_log("ljpeg: entropy data ended early at row %d/%d", jrow, jh->high);

    se_progress("decode", 100);
    return SE_OK;
}

se_err se_cr2_decode_sink(const void *buf, size_t len, const se_cr2_info *info,
                          se_cr2_sink sink, void *ctx, se_arena *scratch)
{
    return cr2_decode_core(buf, len, 0, 0, info, sink, ctx, scratch);
}

se_err se_cr2_decode_stream(se_cr2_read_fn read, void *rctx,
                            const se_cr2_info *info,
                            se_cr2_sink sink, void *ctx, se_arena *scratch)
{
    if (!read) return SE_E_IO;
    return cr2_decode_core(0, 0, read, rctx, info, sink, ctx, scratch);
}

/*
 * The in-RAM decode is now just the streaming decode with a sink that
 * copies into a flat frame. Keeping it that way means the host test suite,
 * which checks every one of the 13,222,104 samples against dcraw, is also
 * testing the exact code path the camera runs.
 */
typedef struct { uint16_t *dst; int pitch; } cr2_mem_sink;

static int cr2_sink_to_mem(void *ctx, int row, int col0, int n,
                           const uint16_t *samples)
{
    cr2_mem_sink *m = (cr2_mem_sink *)ctx;
    memcpy(m->dst + (size_t)row * m->pitch + col0, samples,
           (size_t)n * sizeof(uint16_t));
    return 0;
}

se_err se_cr2_decode(const void *buf, size_t len, const se_cr2_info *info,
                     uint16_t *dst, se_arena *scratch)
{
    cr2_mem_sink m;
    m.dst   = dst;
    m.pitch = info->raw_width;
    return se_cr2_decode_sink(buf, len, info, cr2_sink_to_mem, &m, scratch);
}

se_err se_cr2_find_preview(const void *buf, size_t len,
                           uint32_t *off, uint32_t *bytes,
                           int *w, int *h)
{
    se_rd r;
    uint32_t ifd;
    int n, i;
    uint32_t so = 0, sb = 0, comp = 0, iw = 0, ih = 0;

    se_rd_init(&r, buf, len, 0);
    if (se_rd_u16(&r) != 0x4949) return SE_E_BADFILE;   /* little endian only */
    se_rd_u16(&r);
    ifd = se_rd_u32(&r);

    n = tiff_count(&r, ifd);
    if (n < 0 || n > 512) return SE_E_BADFILE;

    for (i = 0; i < n; i++) {
        tif_entry e;
        if (tiff_entry(&r, ifd, i, &e) != 0) return SE_E_BADFILE;
        switch (e.tag) {
        case 0x0100: iw   = tiff_val(&r, &e, 0); break;   /* ImageWidth   */
        case 0x0101: ih   = tiff_val(&r, &e, 0); break;   /* ImageLength  */
        case 0x0103: comp = tiff_val(&r, &e, 0); break;   /* Compression  */
        case 0x0111: so   = tiff_val(&r, &e, 0); break;   /* StripOffsets */
        case 0x0117: sb   = tiff_val(&r, &e, 0); break;   /* StripByteCnts*/
        default: break;
        }
    }

    /* Compression 6 is Canon's "old-style JPEG" tag for this preview. */
    if (comp != 6 || !so || !sb) return SE_E_UNSUPPORTED;
    if ((size_t)so + 2 > len) return SE_E_TRUNCATED;

    /* Confirm it really starts with SOI before promising a JPEG. */
    {
        const uint8_t *p = (const uint8_t *)buf + so;
        if (p[0] != 0xFF || p[1] != 0xD8) return SE_E_UNSUPPORTED;
    }

    if (off)   *off   = so;
    if (bytes) *bytes = sb;
    if (w)     *w     = (int)iw;
    if (h)     *h     = (int)ih;
    se_log("cr2: preview %ux%u at %u, %u bytes", iw, ih, so, sb);
    return SE_OK;
}

/* ===================================================================== */
/* Black level                                                            */
/* ===================================================================== */

void se_cr2_measure_black(se_cr2_info *info, const uint16_t *raw)
{
    /*
     * The columns to the left of the active area are optically masked: they
     * see no light, so their mean is the sensor's true black. Skip the first
     * few columns (they are electrically noisy) and the last couple before
     * the active area (they bleed).
     */
    int x0 = 2;
    int x1 = info->crop_left - 2;
    int y, x, k;
    double sum[4] = { 0, 0, 0, 0 };
    long   cnt[4] = { 0, 0, 0, 0 };

    if (x1 - x0 < 4) {
        /* No usable mask; keep whatever the camera database gave us. */
        info->black_measured = 0;
        for (k = 0; k < 4; k++) info->black_cfa[k] = info->black;
        return;
    }

    for (y = info->crop_top; y <= info->crop_bottom; y++) {
        const uint16_t *r = raw + (size_t)y * info->raw_width;
        for (x = x0; x < x1; x++) {
            k = ((y & 1) << 1) | (x & 1);
            sum[k] += r[x];
            cnt[k] += 1;
        }
    }

    info->black = 0;
    for (k = 0; k < 4; k++) {
        info->black_cfa[k] = cnt[k] ? (int)(sum[k] / (double)cnt[k] + 0.5) : 0;
        info->black += info->black_cfa[k];
    }
    info->black /= 4;
    info->black_measured = 1;

    se_log("black: measured %d (per-site %d %d %d %d) over %ld px/site",
           info->black, info->black_cfa[0], info->black_cfa[1],
           info->black_cfa[2], info->black_cfa[3], cnt[0]);
}
