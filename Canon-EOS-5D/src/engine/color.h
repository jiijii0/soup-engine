/*
 * color.h - raw levels, white balance, and camera -> working space.
 *
 * Nothing here comes from Canon's firmware. The camera's spectral response
 * (cam_xyz) plus the as-shot white balance is all we need to get from
 * sensor counts to a real colour space.
 */
#ifndef SE_COLOR_H
#define SE_COLOR_H

#include "se.h"
#include "cr2.h"
#include "lut.h"

/*
 * The DIGIC II's ARM946E-S has no floating point unit, so every per-pixel
 * path here is fixed point. Floats appear only in setup code, which runs
 * once per frame and can afford the software emulation.
 */
#define SE_Q 14
#define SE_QONE (1 << SE_Q)

typedef struct {
    /* --- raw conditioning, applied before demosaic ------------------- */
    int   black_cfa[4];     /* per CFA site, raw counts                 */
    int32_t cfa_scale_q[4]; /* per CFA site: wb * headroom, Q14         */
    int   white;            /* saturation point, raw counts            */
    int   clip_cfa[4];      /* (white - black) * scale, clamped 16-bit  */

    /* --- camera RGB -> linear sRGB (D65) ------------------------------ */
    float   cam_to_srgb[9];   /* kept for diagnostics / host printing   */
    int32_t cam_to_srgb_q[9]; /* the one the pixel loop actually uses   */

    /* --- per-channel film response ------------------------------------ */
    /*
     * The film stock's tone curves, as three linear-in/linear-out tables.
     * Applied here, to MOSAICED data, because that is the one place the
     * camera can also apply them -- straight into the raw buffer, before
     * DIGIC demosaics it. Keeping the desktop and the camera on the same
     * code means what we preview is what the camera will produce.
     */
    const se_lut1 *tone;    /* three entries, or NULL for no stock       */
    int   cfa_chan[4];      /* CFA site -> 0 = R, 1 = G, 2 = B           */

    /* --- user controls ------------------------------------------------ */
    float exposure_ev;      /* digital exposure push/pull               */
    int   highlight_mode;   /* 0 = clip, 1 = desaturate towards white   */
} se_color;

/*
 * Build the transform from the parsed file plus a requested exposure.
 * `wb_override` may be NULL to use the as-shot white balance.
 */
se_err se_color_setup(se_color *c, const se_cr2_info *info,
                      const float *wb_override, float exposure_ev);

/*
 * Condition one raw row: subtract black, apply white balance, and scale to
 * full 16-bit range. Done before demosaic so the CFA grid is photometrically
 * flat, which is what Malvar-He-Cutler assumes.
 *
 * `x0` and `y` are the absolute position of src[0] in the raw frame; they
 * select the CFA parity, which is why they cannot be assumed to be even.
 */
void se_color_scale_row(const se_color *c, const uint16_t *src, uint16_t *dst,
                        int width, int x0, int y);

/* Apply the camera->sRGB matrix to an interleaved RGB16 row, in place. */
void se_color_matrix_row(const se_color *c, uint16_t *rgb, int width);

#endif /* SE_COLOR_H */
