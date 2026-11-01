/*
 * jpeg_dec.h - baseline JPEG decoder.
 *
 * Exists so the film look can be applied to the full-size JPEG that Canon
 * already embedded in every CR2. DIGIC has demosaiced, white balanced and
 * colour corrected that image; decoding it and running the stock's
 * per-channel curves over it is the whole job, and it costs 2 MB of reading
 * instead of eleven, with no lossless-JPEG path and no spool.
 *
 * Baseline sequential only -- which is all Canon writes. Progressive and
 * arithmetic coding are rejected rather than half-handled.
 *
 * Rows come out through a callback so the caller can curve and re-encode
 * them as they appear; nothing ever holds the whole picture.
 */
#ifndef SE_JPEG_DEC_H
#define SE_JPEG_DEC_H

#include "se.h"

typedef struct {
    int width, height;
    int ncomp;
} se_jdec_info;

/* Receives `nrows` rows of interleaved 8-bit RGB, top to bottom. */
typedef int (*se_jdec_sink)(void *ctx, const uint8_t *rgb, int nrows, int width);

/*
 * Read just the frame header. Cheap; touches only the first few KB.
 *
 * Reports the CODED size. The decode entry points below report the
 * DELIVERED size in their own `info`, which differs when scale is 2 --
 * se_jpeg_scaled_size() is the conversion between the two.
 */
se_err se_jpeg_dec_info(const void *buf, size_t len, se_jdec_info *out);

/*
 * Half-size decoding.
 *
 * scale 1 is the whole image. scale 2 asks the decoder to run a 4-point
 * inverse DCT over the top-left 4x4 coefficients of each block instead of
 * the full 8-point transform over all 64, which delivers the image at half
 * its linear size for an eighth of the transform work -- and a quarter of
 * the colour conversion, and a quarter of the pixels for whatever comes
 * next. The high frequencies are never reconstructed rather than being
 * reconstructed and then thrown away by a resize.
 *
 * This is why it matters here: a film frame off this camera took about 41
 * seconds, nearly all of it the decode and re-encode of a 2496x1664
 * preview, and the result was being box-filtered down to 1248x832 anyway.
 * Asking the decoder for 1248x832 in the first place is the same picture
 * for a fraction of the arithmetic.
 *
 * Dimensions round up, so an odd width keeps its last column.
 *
 * scale 4 is the same again, stopped at two points: the top-left 2x2
 * coefficients, a quarter-size image, 8 multiply-accumulates a block.
 * For the 700D, whose 18 MP preview and 150 MIPS processor make even
 * half size a 42-second frame.
 */
void se_jpeg_scaled_size(int w, int h, int scale, int *ow, int *oh);

/*
 * Decode the whole image. Needs roughly width * 16 * 6 bytes of arena for a
 * 2x2-subsampled frame, which is about 240 KB at 2496 wide -- and a quarter
 * of that at scale 2, since every buffer it sizes is an output buffer.
 */
se_err se_jpeg_decode(const void *buf, size_t len, se_arena *a,
                      se_jdec_sink sink, void *ctx, se_jdec_info *info);

se_err se_jpeg_decode_scaled(const void *buf, size_t len, int scale,
                             se_arena *a, se_jdec_sink sink, void *ctx,
                             se_jdec_info *info);

/*
 * Decode without the file in memory.
 *
 * `hdr` must hold at least the markers up to and including the scan header
 * -- a few hundred bytes for a Canon preview, so a few KB is ample --
 * and `read` supplies the rest, called strictly forwards in 8 KB steps with
 * offsets relative to the start of the JPEG.
 *
 * This is not an optimisation. The 5D Classic refused a 2.2 MB allocation
 * with 2264 KB reported free: the total exists but no single run that large
 * does. Streaming keeps the largest request down to the window.
 */
typedef int (*se_jdec_read_fn)(void *ctx, uint32_t off, void *dst, uint32_t n);

se_err se_jpeg_decode_stream(const void *hdr, size_t hdrlen, uint32_t total,
                             se_jdec_read_fn read, void *rctx, se_arena *a,
                             se_jdec_sink sink, void *ctx, se_jdec_info *info);

se_err se_jpeg_decode_stream_scaled(const void *hdr, size_t hdrlen,
                                    uint32_t total, se_jdec_read_fn read,
                                    void *rctx, int scale, se_arena *a,
                                    se_jdec_sink sink, void *ctx,
                                    se_jdec_info *info);

#endif /* SE_JPEG_DEC_H */
