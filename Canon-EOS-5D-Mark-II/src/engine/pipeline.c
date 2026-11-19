#include "pipeline.h"
#include "color.h"
#include "debayer.h"
#include "se_libc.h"

/* Halo columns taken from outside the active area when the frame allows. */
#define XHALO 2

void se_dev_opts_default(se_dev_opts *o)
{
    memset(o, 0, sizeof *o);
    o->scale        = 1;
    o->exposure_ev  = 0.0f;
    o->jpeg_quality = 92;
    o->dither       = 1;
    o->wb_override  = NULL;
}

void se_develop_out_size(const se_cr2_info *info, int scale, int *w, int *h)
{
    if (scale < 1) scale = 1;
    *w = info->active_width  / scale;
    *h = info->active_height / scale;
    /* JPEG wants at least one MCU */
    if (*w < 8) *w = 8;
    if (*h < 8) *h = 8;
}

size_t se_develop_scratch_size(const se_cr2_info *info)
{
    /*
     * Buffers scale differently with the output scale: the RGB band shrinks
     * with the output, but the CFA band and the raw staging buffer GROW,
     * because a binned output row consumes `scale` raw rows. Quarter size
     * is therefore the worst case, not full size. Report the maximum over
     * all supported scales so a caller can size one arena and use any mode.
     */
    static const int scales[3] = { 1, 2, 4 };
    size_t worst = 0;
    int    k;

    for (k = 0; k < 3; k++) {
        int    scale = scales[k];
        int    halo  = (scale == 1) ? SE_DEBAYER_HALO : 0;
        int    aw    = info->active_width / scale;
        int    bw    = (scale == 1) ? aw * scale + 2 * XHALO : aw * scale;
        int    rows  = SE_BAND_ROWS * scale + 2 * halo;
        size_t need  = (size_t)rows * bw * sizeof(uint16_t)              /* band  */
                     + (size_t)rows * info->raw_width * sizeof(uint16_t) /* stage */
                     + (size_t)SE_BAND_ROWS * aw * 3 * sizeof(uint16_t)  /* rgb   */
                     + (size_t)SE_BAND_ROWS * aw * 3                     /* rgb8  */
                     + (size_t)8 * aw * 3                                /* strip */
                     + sizeof(se_lut1);                                  /* curve */
        if (need > worst) worst = need;
    }
    return worst + 4096;   /* alignment slack */
}

/*
 * Film grain.
 *
 * A hash of the pixel coordinates, so it is deterministic (the same frame
 * always grains the same way), needs no state, and costs a few integer ops.
 * Amplitude is weighted by 4v(1-v): grain is a modulation of exposed
 * silver density, so it vanishes in clipped whites and empty blacks and
 * peaks in the midtones, which is what it does on real film.
 */
static inline uint32_t grain_hash(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void apply_grain(uint16_t *row, int width, int y,
                        int cell, int amp_q, uint32_t seed)
{
    int x;
    for (x = 0; x < width; x++) {
        int v = row[x * 3 + 1];                 /* weight on green/luma */
        int w = (v * (65535 - v)) >> 14;        /* 4v(1-v), 0..65535-ish */
        int n, d, c;
        if (w <= 0) continue;
        n = (int)(grain_hash((uint32_t)(x / cell), (uint32_t)(y / cell), seed) & 0xffff);
        d = ((n - 32768) * amp_q >> 15) * w >> 16;
        for (c = 0; c < 3; c++) {
            int t = (int)row[x * 3 + c] + d;
            row[x * 3 + c] = (uint16_t)SE_CLAMP(t, 0, 65535);
        }
    }
}

/* Bayer 8x8 ordered dither, scaled to one 8-bit step. */
static const uint8_t g_dither[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

se_err se_develop(const se_cr2_info *info, const se_raw_src *src,
                  const se_lut3 *lut, const se_dev_opts *opts,
                  se_arena *arena, se_jpeg_sink sink, void *sink_ctx)
{
    se_color  col;
    se_jpeg  *jpg;
    se_lut1  *encode;
    uint16_t *band, *rgb, *stage;
    uint8_t  *rgb8, *strip;
    se_err    rc;
    int       scale = opts->scale < 1 ? 1 : opts->scale;
    int       aw, ah;
    int       halo, bstride, band_rows_max, stage_rows;
    int       xh_l, xh_r, xoff, bvalid;
    int       y0, last_pct = -1;

    if (scale != 1 && scale != 2 && scale != 4) return SE_E_UNSUPPORTED;

    se_develop_out_size(info, scale, &aw, &ah);

    /*
     * Halo columns only exist if the active area is inset from the raw
     * frame. It always is on a Canon (the masked border), but clamp anyway.
     * Binned modes need no halo at all: a 2x2 quad is self-contained, which
     * is most of why they are so much cheaper.
     */
    halo = (scale == 1) ? SE_DEBAYER_HALO : 0;
    xh_l = (scale == 1) ? SE_MIN(XHALO, info->crop_left) : 0;
    xh_r = (scale == 1) ? SE_MIN(XHALO, info->raw_width - 1 - info->crop_right) : 0;
    xoff = xh_l;
    bvalid  = xh_l + aw * scale + xh_r;
    bstride = bvalid;
    band_rows_max = SE_BAND_ROWS * scale + 2 * halo;
    stage_rows    = band_rows_max;

    rc = se_color_setup(&col, info, opts->wb_override, opts->exposure_ev);
    if (rc != SE_OK) return rc;
    col.tone = opts->tone;

    band   = SE_NEW(arena, uint16_t, (size_t)band_rows_max * bstride);
    rgb    = SE_NEW(arena, uint16_t, (size_t)SE_BAND_ROWS * aw * 3);
    rgb8   = SE_NEW(arena, uint8_t,  (size_t)SE_BAND_ROWS * aw * 3);
    strip  = SE_NEW(arena, uint8_t,  (size_t)8 * aw * 3);
    encode = SE_NEW(arena, se_lut1,  1);
    /* ~5 KB; an ML task stack will not take it */
    jpg    = opts->rows ? NULL : SE_NEW(arena, se_jpeg, 1);
    stage  = src->mem ? NULL
                      : SE_NEW(arena, uint16_t, (size_t)stage_rows * info->raw_width);
    if (!band || !rgb || !rgb8 || !strip || !encode) return SE_E_NOMEM;
    if (!opts->rows && !jpg) return SE_E_NOMEM;
    if (!src->mem && (!stage || !src->read_rows)) return SE_E_NOMEM;

    memset(band, 0, (size_t)band_rows_max * bstride * sizeof(uint16_t));
    se_lut1_srgb_encode(encode);

    if (!opts->rows) {
        se_jpeg_exif ex;
        char sw[64];
        int  k = 0;
        const char *tag = "Soup Engine";
        while (tag[k] && k < 40) { sw[k] = tag[k]; k++; }
        if (opts->stock_name && opts->stock_name[0]) {
            int m = 0;
            sw[k++] = ' ';
            while (opts->stock_name[m] && k < 60) sw[k++] = opts->stock_name[m++];
        }
        sw[k] = 0;

        memset(&ex, 0, sizeof ex);
        ex.make          = info->make[0]  ? info->make  : NULL;
        ex.model         = info->model[0] ? info->model : NULL;
        ex.software      = sw;
        ex.datetime      = info->datetime[0] ? info->datetime : NULL;
        ex.exposure_time = info->exposure_time;
        ex.aperture      = info->aperture;
        ex.iso           = info->iso;

        rc = se_jpeg_begin(jpg, aw, ah, opts->jpeg_quality, strip, &ex,
                           sink, sink_ctx);
        if (rc != SE_OK) return rc;
    }

    for (y0 = 0; y0 < ah; y0 += SE_BAND_ROWS) {
        int nrows = SE_MIN(SE_BAND_ROWS, ah - y0);
        int i, r, x;
        int phase;
        int band_rows = nrows * scale + 2 * halo;
        int ay_lo = y0 * scale;                 /* first active row needed */
        int ah_raw = info->active_height;

        /* --- 1. condition the CFA rows this band needs ----------------- */
        /*
         * Reflection at the frame edges never reaches outside the clamped
         * contiguous span below, so one sequential read per band is enough
         * even when the source is a file.
         */
        int s0 = SE_MAX(0, ay_lo - halo);
        int s1 = SE_MIN(ah_raw, ay_lo + nrows * scale + halo);

        if (!src->mem) {
            if (src->read_rows(src->ctx, info->crop_top + s0, s1 - s0, stage) != 0)
                return SE_E_IO;
        }

        for (i = 0; i < band_rows; i++) {
            int ay = ay_lo + i - halo;                  /* active row     */
            int ry;
            const uint16_t *srow;
            /* reflect at the frame edges rather than smearing a constant */
            if (ay < 0)       ay = -ay;
            if (ay >= ah_raw) ay = 2 * ah_raw - ay - 2;
            ay = SE_CLAMP(ay, 0, ah_raw - 1);
            ry = info->crop_top + ay;

            srow = src->mem
                 ? src->mem + (size_t)ry * info->raw_width
                 : stage + (size_t)(ay - s0) * info->raw_width;

            se_color_scale_row(&col,
                               srow + info->crop_left - xh_l,
                               band + (size_t)i * bstride,
                               bvalid,
                               info->crop_left - xh_l,
                               ry);
        }

        /*
         * The CFA phase of output row 0 of this band. Reflection above can
         * flip the parity of the halo rows, but never of the output rows,
         * so this stays exact.
         */
        phase = (((info->crop_top + ay_lo) & 1) << 1) | (info->crop_left & 1);

        /* --- 2. demosaic, or bin if we are rendering reduced ------------ */
        if (scale == 1)
            se_debayer_band(band, bstride, bvalid, xoff, aw, nrows, phase, rgb);
        else
            se_bin_band(band, bstride, xoff, aw, nrows, phase, scale, rgb);

        /* --- 3..6. colour, encode, look, dither ------------------------- */
        for (r = 0; r < nrows; r++) {
            uint16_t *row  = rgb + (size_t)r * aw * 3;
            uint8_t  *out8 = rgb8 + (size_t)r * aw * 3;
            const uint8_t *dr = g_dither[(y0 + r) & 7];

            se_color_matrix_row(&col, row, aw);

            for (x = 0; x < aw * 3; x++)
                row[x] = se_lut1_eval(encode, row[x]);

            if (lut) se_lut3_apply_row(lut, row, aw);

            if (opts->grain_amount > 0.0f) {
                /* size 0..100 -> a 1..5 pixel grain cell */
                int cell  = 1 + (int)(opts->grain_size / 25.0f);
                /* frequency raises the fine-grain contrast a little */
                float amp = (opts->grain_amount / 100.0f) * 0.22f
                          * (0.7f + 0.6f * opts->grain_frequency / 100.0f);
                apply_grain(row, aw, y0 + r, cell,
                            (int)(amp * 32768.0f), 0x5eed1234u);
            }

            if (opts->dither) {
                /*
                 * Ordered dither: replace the fixed half-step rounding with
                 * a threshold that varies across an 8x8 cell, so a value
                 * between two 8-bit codes resolves to a stable mix instead
                 * of a hard contour. Costs nothing and saves skies.
                 */
                for (x = 0; x < aw; x++) {
                    int thr = ((int)dr[x & 7] * 65535) >> 6;
                    int c;
                    for (c = 0; c < 3; c++) {
                        int v = ((int)row[x * 3 + c] * 255 + thr) / 65535;
                        out8[x * 3 + c] = (uint8_t)SE_CLAMP(v, 0, 255);
                    }
                }
            } else {
                for (x = 0; x < aw * 3; x++)
                    out8[x] = (uint8_t)(((int)row[x] * 255 + 32767) / 65535);
            }
        }

        if (opts->rows) {
            if (opts->rows(opts->rows_ctx, rgb8, nrows, aw) != 0) return SE_E_IO;
        } else {
            rc = se_jpeg_write_rows(jpg, rgb8, nrows);
            if (rc != SE_OK) return rc;
        }

        {
            int pct = (y0 + nrows) * 100 / ah;
            if (pct != last_pct) { se_progress("develop", pct); last_pct = pct; }
        }
    }

    rc = opts->rows ? SE_OK : se_jpeg_end(jpg);
    se_progress("develop", 100);
    return rc;
}
