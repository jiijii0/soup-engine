/*
 * debayer.h - Malvar-He-Cutler gradient-corrected linear demosaic.
 *
 * "High-Quality Linear Interpolation for Demosaicing of Bayer-Patterned
 * Colour Images", Malvar, He & Cutler, Microsoft Research, ICASSP 2004.
 *
 * Chosen over adaptive methods (AHD, VNG, AMaZE) deliberately: it is a
 * fixed 5x5 linear filter, which means a predictable instruction count per
 * pixel and no data-dependent branching -- the only kind of demosaic that
 * has any business running on a 2005 ARM core.
 */
#ifndef SE_DEBAYER_H
#define SE_DEBAYER_H

#include "se.h"

/* Rows of halo the band buffer must carry above and below the output rows. */
#define SE_DEBAYER_HALO 2

/*
 * Demosaic one horizontal band.
 *
 *   band     CFA samples, already black-subtracted and white-balanced.
 *            Must contain nrows + 2*SE_DEBAYER_HALO rows; band row
 *            (r + SE_DEBAYER_HALO) is output row r. At the top and bottom of
 *            the frame the caller duplicates edge rows into the halo.
 *   stride   samples per band row.
 *   band_w   valid samples per band row (the clamping limit).
 *   x0       band column that becomes output column 0. Giving this a
 *            non-zero value lets the caller supply real halo columns from
 *            outside the active area instead of clamping at the frame edge.
 *   width    pixels per output row.
 *   nrows    output rows to produce.
 *   phase    SE_CFA_* of the pixel at output row 0, output column 0.
 *   out      nrows * width * 3 interleaved uint16, camera-space linear RGB.
 */
void se_debayer_band(const uint16_t *band, int stride, int band_w, int x0,
                     int width, int nrows, int phase, uint16_t *out);

/*
 * Binned readout: collapse each 2x2 Bayer quad straight into one RGB pixel
 * (R, (G1+G2)/2, B), optionally averaging scale/2 squared quads.
 *
 * This is not "demosaic then downscale". The quad already carries one
 * sample of every colour, so there is nothing to interpolate: the output is
 * both faster AND cleaner than interpolating and then throwing pixels away.
 * It is the right mode for a half- or quarter-size render.
 *
 *   scale   2 or 4. Output is active/scale on each axis.
 *   nrows   output rows; the band must hold nrows * scale raw rows and
 *           needs no halo, since no filter reaches outside the quad.
 */
void se_bin_band(const uint16_t *band, int stride, int x0,
                 int width, int nrows, int phase, int scale, uint16_t *out);

#endif /* SE_DEBAYER_H */
