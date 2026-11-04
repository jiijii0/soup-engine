/*
 * pipeline.h - raw sensor data in, graded JPEG out.
 *
 * The whole image is processed in 8-row bands so that peak memory does not
 * scale with image height. That is not an optimisation: a 12.8 MP frame of
 * interleaved RGB16 is 76 MB, which the camera will never have.
 *
 * Stage order, and why:
 *   1. black subtract + white balance   before demosaic, so the CFA grid is
 *                                       photometrically flat
 *   2. Malvar-He-Cutler demosaic        -> camera-space linear RGB
 *   3. camera -> sRGB matrix            the sensor's own spectral response
 *   4. linear -> sRGB encode            the 3D LUT domain
 *   5. 3D LUT                           the film look
 *   6. 16 -> 8 bit with ordered dither  avoids banding in skies
 *   7. baseline JPEG
 */
#ifndef SE_PIPELINE_H
#define SE_PIPELINE_H

#include "se.h"
#include "cr2.h"
#include "lut.h"
#include "jpeg_enc.h"

#define SE_BAND_ROWS 8     /* one JPEG MCU row */

/*
 * Optional tap on the finished 8-bit rows, taken just before the JPEG
 * encoder. Set it to compare the pipeline against a reference renderer
 * without the encoder in the way, or to write an uncompressed preview.
 * When present the JPEG encoder is skipped entirely.
 */
typedef int (*se_row_sink)(void *ctx, const uint8_t *rgb8, int nrows, int width);

typedef struct {
    /*
     * 1 = full 4368x2912 with the Malvar-He-Cutler demosaic.
     * 2 = half size by Bayer-quad binning, no interpolation at all.
     * 4 = quarter size.
     * On the camera, 2 is roughly an order of magnitude less work: a quarter
     * of the pixels AND no 5x5 filter on any of them.
     */
    int   scale;
    float exposure_ev;     /* digital push/pull before the look           */
    int   jpeg_quality;    /* 1..100                                      */
    int   dither;          /* 1 = ordered dither on the 16->8 step        */
    const float *wb_override;  /* NULL = use as-shot                      */
    /*
     * Three linear-in/linear-out tables from se_stock_bake(), applied to
     * the mosaiced data. This is the path the camera will use; set it and
     * leave `lut` NULL to preview exactly what DIGIC would produce.
     */
    const se_lut1 *tone;
    /* written into the output JPEG's Software tag, so a finished frame
     * records which stock produced it */
    const char *stock_name;
    se_row_sink rows;      /* NULL = encode JPEG                          */
    void       *rows_ctx;

    /*
     * Grain. Spatial, so it cannot live in the 3D LUT and is applied after
     * it. Scales match Lightroom's panel (0..100).
     */
    float grain_amount;
    float grain_size;
    float grain_frequency;
} se_dev_opts;

void se_dev_opts_default(se_dev_opts *o);

/* Output dimensions for the chosen scale. */
void se_develop_out_size(const se_cr2_info *info, int scale, int *w, int *h);

/*
 * How much arena the developer needs for a given frame, excluding the raw
 * buffer itself. Call this before committing memory on the camera.
 */
size_t se_develop_scratch_size(const se_cr2_info *info);

/*
 * Where the mosaiced sensor rows come from.
 *
 * A 12.8 MP frame is 25 MB of uint16 and the 5D Classic's Magic Lantern
 * build has no large-allocation pool (exmem is excluded on that port), so
 * holding the frame in RAM may simply not be possible. Making the source
 * a callback means the camera can spool the decoded frame to a file on the
 * CF card and stream bands back from it, which costs a few hundred KB
 * instead of 25 MB. The host just points `mem` at the whole frame.
 *
 * read_rows() must fill `n` consecutive raw rows starting at raw row `y`,
 * each raw_width samples, and return 0 on success.
 */
typedef struct {
    const uint16_t *mem;    /* whole frame in RAM, or NULL to use the cb */
    int (*read_rows)(void *ctx, int y, int n, uint16_t *dst);
    void *ctx;
} se_raw_src;

/*
 * Develop a decoded raw frame. `lut` is the look; pass NULL for a straight
 * sRGB render with no grade.
 */
se_err se_develop(const se_cr2_info *info, const se_raw_src *src,
                  const se_lut3 *lut, const se_dev_opts *opts,
                  se_arena *arena, se_jpeg_sink sink, void *sink_ctx);

#endif /* SE_PIPELINE_H */
