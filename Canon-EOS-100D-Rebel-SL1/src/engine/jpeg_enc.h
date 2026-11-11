/*
 * jpeg_enc.h - baseline JPEG encoder, streaming and integer-only.
 *
 * Writes 4:4:4 baseline sequential JPEG. Rows are pushed in 8-line strips
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
} se_jpeg_exif;

typedef struct {
    se_jpeg_sink sink;
    void        *ctx;

    int width, height;
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

    uint8_t  exifbuf[1024];

    int      chroma420;        /* 4:2:0 rather than 4:4:4 */
    int      mcu_rows;         /* 16 or 8: rows per strip  */
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

/*
 * The same with a choice of chroma sampling. 4:2:0 halves the encoder's
 * block work and the file size for the same pixels, at the usual cost of
 * chroma at half resolution -- what every camera JPEG does. The strip is
 * sixteen rows then, not eight: size it with SE_JPEG_STRIP_BYTES.
 */
se_err se_jpeg_begin2(se_jpeg *j, int width, int height, int quality,
                      uint8_t *strip, const se_jpeg_exif *exif,
                      se_jpeg_sink sink, void *ctx, int chroma420);
#define SE_JPEG_STRIP_BYTES(width, chroma420) \
    ((size_t)((chroma420) ? 16 : 8) * (size_t)(width) * 3)

/* Push `nrows` rows of 8-bit interleaved RGB. Any count is accepted. */
se_err se_jpeg_write_rows(se_jpeg *j, const uint8_t *rgb, int nrows);

se_err se_jpeg_end(se_jpeg *j);

#endif /* SE_JPEG_ENC_H */
