/*
 * cr2.h - Canon CR2 reader: TIFF/EXIF metadata + lossless-JPEG raw decode.
 *
 * This deliberately does NOT use any Canon code. The CR2 raw payload is
 * untouched sensor data (Canon's picture style, noise reduction and
 * demosaic only ever apply to the embedded preview JPEG), so decoding it
 * ourselves is what makes the rest of the pipeline genuinely our own.
 */
#ifndef SE_CR2_H
#define SE_CR2_H

#include "wb.h"   /* the WB presets, which the stock format also needs */

#include "se.h"

/*
 * The white balance presets Canon stores in the ColorData block, in the
 * order they appear there. Lightroom's "Daylight" and friends mean these,
 * which is why a sidecar that says WhiteBalance="Daylight" cannot be
 * honoured from the as-shot value alone.
 */

/* CFA phase of the pixel at (crop_left, crop_top). */
#define SE_CFA_RGGB 0
#define SE_CFA_GRBG 1
#define SE_CFA_GBRG 2
#define SE_CFA_BGGR 3

typedef struct {
    /* --- geometry ----------------------------------------------------- */
    int raw_width;        /* width of the stored raw frame, incl. margins */
    int raw_height;
    int crop_left;        /* active area within the raw frame            */
    int crop_top;
    int crop_right;       /* inclusive, as Canon stores it               */
    int crop_bottom;
    int active_width;     /* crop_right - crop_left + 1                  */
    int active_height;
    int cfa_phase;        /* SE_CFA_* at (crop_left, crop_top)           */

    /* --- levels ------------------------------------------------------- */
    int bits;             /* bits per sample as coded (12 on the 5Dc)    */
    int black;            /* scalar black level (mean of the four)       */
    int black_cfa[4];     /* per-CFA-site black, indexed (y&1)*2 + (x&1)  */
    int white;            /* saturation point                            */
    int black_measured;   /* 1 if the levels came from masked pixels      */

    /* --- colour ------------------------------------------------------- */
    float wb[4];          /* as-shot white balance, RGGB, raw multipliers */
    int   wb_present;
    float wb_preset[SE_WB_COUNT][4];  /* the camera's stored presets       */
    int   wb_presets_present;
    float cam_xyz[9];     /* XYZ(D65) -> camera RGB, row major            */
    int   cam_xyz_present;

    /* --- exif --------------------------------------------------------- */
    char  make[32];
    char  model[64];
    float exposure_time;  /* seconds */
    float aperture;
    int   iso;
    char  datetime[24];   /* "YYYY:MM:DD HH:MM:SS", EXIF spelling */

    /* --- where the raw bits live -------------------------------------- */
    uint32_t strip_offset;
    uint32_t strip_bytes;
    int      slice_count;   /* CR2 tag 0xc640[0]: number of full slices  */
    int      slice_width;   /* [1]                                        */
    int      slice_last;    /* [2]                                        */
} se_cr2_info;

/*
 * Parse headers only. Cheap: touches a few KB of the file, so the camera
 * build can run this without pulling 13 MB off the card first.
 */
se_err se_cr2_parse(const void *buf, size_t len, se_cr2_info *out);

/*
 * Decode the full raw frame. Writes raw_width * raw_height uint16 samples
 * (still CFA-mosaiced, still at the coded bit depth) into `dst`, which must
 * hold at least that many. Needs ~64 KB of scratch from `scratch`.
 */
se_err se_cr2_decode(const void *buf, size_t len, const se_cr2_info *info,
                     uint16_t *dst, se_arena *scratch);

/*
 * Streaming decode.
 *
 * The camera cannot hold a decoded frame: 4368 x 2912 uint16 is 25 MB and
 * the 5D Classic's ML build has no allocation pool that large. So the
 * decoder hands finished samples to a callback instead of filling a buffer,
 * and the caller decides where they go -- a RAM frame when there is room,
 * or straight out to a spool file on the CF card when there is not.
 *
 * `sink` receives runs of horizontally-adjacent samples: `n` samples
 * belonging to raw row `row`, starting at column `col0`. Runs never span
 * rows. For a sliced Canon CR2 this works out to exactly two runs per
 * decoded JPEG row, each one a complete half-row, delivered in increasing
 * order within a slice -- so a sink that appends to a file per slice writes
 * purely sequentially and never has to seek.
 *
 * Return 0 from the sink to continue; any other value aborts the decode and
 * is returned from se_cr2_decode_sink as SE_E_IO.
 */
typedef int (*se_cr2_sink)(void *ctx, int row, int col0, int n,
                           const uint16_t *samples);

se_err se_cr2_decode_sink(const void *buf, size_t len, const se_cr2_info *info,
                          se_cr2_sink sink, void *ctx, se_arena *scratch);

/*
 * Decode without the coded file in memory.
 *
 * `read` must place `n` bytes from absolute file offset `off` into `dst` and
 * return 0; the decoder calls it strictly forwards, in 8 KB steps, so a
 * plain seek-and-read is all it has to be.
 *
 * This exists because of a hard limit on the 5D Classic rather than as an
 * optimisation: Magic Lantern shrinks Canon's heap to 7 MB on that body
 * (local_entry.S sets it up as 0x280000..0x980000), and a coded frame is
 * about 11 MB. Reading it whole is simply not possible there, so on the
 * camera this is the only decode path that can work.
 *
 * Restart intervals are rejected rather than mishandled: resynchronising
 * after one means scanning forward for a marker, which a window cannot do.
 * Canon does not emit them.
 */
typedef int (*se_cr2_read_fn)(void *ctx, uint32_t off, void *dst, uint32_t n);

se_err se_cr2_decode_stream(se_cr2_read_fn read, void *rctx,
                            const se_cr2_info *info,
                            se_cr2_sink sink, void *ctx, se_arena *scratch);

/*
 * Locate the full-size JPEG Canon embeds in IFD0.
 *
 * DIGIC has already demosaiced, white balanced and colour corrected that
 * image -- on a 5D Classic it is 2496x1664 and about 2 MB. Applying a film
 * stock's per-channel curves to it is the agreed architecture: the camera
 * does the hard part, we own the tone response. It also costs a fifth of the
 * reading that the raw strip does, and needs no lossless-JPEG decoding.
 *
 * Returns SE_E_UNSUPPORTED if the file has no such preview.
 */
se_err se_cr2_find_preview(const void *buf, size_t len,
                           uint32_t *off, uint32_t *bytes,
                           int *w, int *h);

/*
 * Measure the black level from the optically-masked columns to the left of
 * the active area, and refine info->black. Call after decode.
 */
void se_cr2_measure_black(se_cr2_info *info, const uint16_t *raw);

#endif /* SE_CR2_H */
