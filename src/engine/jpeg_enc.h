/*
 * jpeg_enc.h - baseline JPEG encoder, streaming and integer-only.
 *
 * Writes 4:2:2 baseline sequential JPEG, the sampling the 50D playback
 * decoder accepts. Rows are pushed in 8-line strips
 * so the encoder never holds more than one MCU row, which is what lets the
 * whole pipeline run in a band rather than a frame.
 *
 * Integer throughout: the IJG "islow" DCT, and reciprocal multiplies in
 * place of divides, because the ARM946E-S has neither an FPU nor a divide
 * instruction.
 */
#ifndef SE_JPEG_ENC_H
#define SE_JPEG_ENC_H

#include "se.h"

/* Sink for encoded bytes. Returns 0 on success. */
typedef int (*se_jpeg_sink)(void *ctx, const uint8_t *data, size_t len);

#define SE_JPEG_MCU 8

/*
 * Metadata written as an EXIF APP1 block. Two reasons this matters: the
 * shot keeps its exposure data, and ColorSpace=sRGB is tagged explicitly.
 * An untagged JPEG gets colour-managed by whatever the viewer guesses,
 * which silently changes the picture.
 */
typedef struct {
    const char *make;
    const char *model;
    const char *software;    /* we put the film stock name here */
    const char *datetime;    /* "YYYY:MM:DD HH:MM:SS" */
    float exposure_time;     /* seconds; 0 to omit */
    float aperture;          /* f-number; 0 to omit */
    int   iso;               /* 0 to omit */
    const uint8_t *thumb;    /* JPEG thumbnail, or NULL */
    int   thumb_len;
} se_jpeg_exif;

typedef struct {
    se_jpeg_sink sink;
    void        *ctx;

    int width, height;
    int stride;          /* source row length; may be wider than width */
    int quality;

    uint8_t  qt[2][64];        /* natural order    */
    uint32_t qrecip[2][64];    /* Q16 reciprocals  */

    /* bit assembly */
    uint32_t bitbuf;
    int      bitcnt;

    int      dcpred[3];

    /* one MCU row of input, 8 lines of RGB8 */
    uint8_t *strip;            /* 8 * width * 3, supplied by the caller */
    int      strip_rows;

    int      rows_done;
    int      failed;

    /* small output staging buffer so the sink sees few, large writes */
    uint8_t  out[4096];
    int      outpos;

    uint8_t  exifbuf[8192];
} se_jpeg;

/*
 * This struct is ~5 KB. Allocate it from an arena, never on the stack:
 * Magic Lantern task stacks are small and an overflow there corrupts
 * Canon's memory rather than faulting cleanly.
 */

/*
 * `strip` must point at 8 * width * 3 bytes of scratch owned by the caller
 * and must stay valid until se_jpeg_end().
 */
se_err se_jpeg_begin(se_jpeg *j, int width, int height, int quality,
                     uint8_t *strip, const se_jpeg_exif *exif,
                     se_jpeg_sink sink, void *ctx);

/* Push `nrows` rows of 8-bit interleaved RGB. Any count is accepted. */
se_err se_jpeg_write_rows(se_jpeg *j, const uint8_t *rgb, int nrows);

se_err se_jpeg_end(se_jpeg *j);

#endif /* SE_JPEG_ENC_H */
