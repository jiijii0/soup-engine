#include "debayer.h"

/*
 * Colour index at (x, y) for a given CFA phase: 0 = red, 1 = green, 2 = blue.
 * The phase says which site of an RGGB grid the origin sits on, so shifting
 * the coordinates by the phase and reading the RGGB pattern gives the answer.
 */
static inline int cfa_color(int phase, int x, int y)
{
    int X = x + (phase & 1);
    int Y = y + ((phase >> 1) & 1);
    if (Y & 1) return (X & 1) ? 2 : 1;      /* G B row -> B at odd X */
    return (X & 1) ? 1 : 0;                 /* R G row -> R at even X */
}

/*
 * Malvar-He-Cutler kernels, scaled by 16 so everything stays in integers.
 * The published coefficients are eighths with halves in them; doubling
 * clears the fractions without losing any precision.
 */
#define MHC_SHIFT 4   /* divide by 16 */

static inline int clampx(int x, int w)
{
    return x < 0 ? 0 : (x >= w ? w - 1 : x);
}

/* Green at a red or blue site. */
static inline int mhc_g_at_rb(const uint16_t *b, int stride, int x, int w)
{
    int c  = b[x];
    int l1 = b[clampx(x - 1, w)], r1 = b[clampx(x + 1, w)];
    int l2 = b[clampx(x - 2, w)], r2 = b[clampx(x + 2, w)];
    int u1 = b[-stride + x],      d1 = b[stride + x];
    int u2 = b[-2 * stride + x],  d2 = b[2 * stride + x];
    int v = 8 * c + 4 * (l1 + r1 + u1 + d1) - 2 * (l2 + r2 + u2 + d2);
    v >>= MHC_SHIFT;
    return SE_CLAMP(v, 0, 65535);
}

/*
 * Chroma at a green site, when the wanted colour lies along the row
 * (horizontally adjacent). Transposing the taps handles the column case.
 */
static inline int mhc_c_at_g(const uint16_t *b, int stride, int x, int w,
                             int horizontal)
{
    int c  = b[x];
    int a1, a2, p2, d_ul, d_ur, d_dl, d_dr;

    d_ul = b[-stride + clampx(x - 1, w)];
    d_ur = b[-stride + clampx(x + 1, w)];
    d_dl = b[ stride + clampx(x - 1, w)];
    d_dr = b[ stride + clampx(x + 1, w)];

    if (horizontal) {
        a1 = b[clampx(x - 1, w)] + b[clampx(x + 1, w)];   /* +8 each */
        a2 = b[clampx(x - 2, w)] + b[clampx(x + 2, w)];   /* -2 each */
        p2 = b[-2 * stride + x] + b[2 * stride + x];      /* +1 each */
    } else {
        a1 = b[-stride + x] + b[stride + x];
        a2 = b[-2 * stride + x] + b[2 * stride + x];
        p2 = b[clampx(x - 2, w)] + b[clampx(x + 2, w)];
    }

    {
        int v = 10 * c + 8 * a1 - 2 * a2
              - 2 * (d_ul + d_ur + d_dl + d_dr)
              + 1 * p2;
        v >>= MHC_SHIFT;
        return SE_CLAMP(v, 0, 65535);
    }
}

/* Red at a blue site, or blue at a red site: the diagonal kernel. */
static inline int mhc_rb_at_br(const uint16_t *b, int stride, int x, int w)
{
    int c  = b[x];
    int diag = b[-stride + clampx(x - 1, w)] + b[-stride + clampx(x + 1, w)]
             + b[ stride + clampx(x - 1, w)] + b[ stride + clampx(x + 1, w)];
    int ax = b[clampx(x - 2, w)] + b[clampx(x + 2, w)];
    int ay = b[-2 * stride + x] + b[2 * stride + x];
    int v = 12 * c + 4 * diag - 3 * (ax + ay);
    v >>= MHC_SHIFT;
    return SE_CLAMP(v, 0, 65535);
}

void se_debayer_band(const uint16_t *band, int stride, int band_w, int x0,
                     int width, int nrows, int phase, uint16_t *out)
{
    int r, x;

    for (r = 0; r < nrows; r++) {
        const uint16_t *row = band + (size_t)(r + SE_DEBAYER_HALO) * stride;
        uint16_t *o = out + (size_t)r * width * 3;

        for (x = 0; x < width; x++, o += 3) {
            int bx = x0 + x;
            int c = cfa_color(phase, x, r);
            int v = row[bx];

            if (c == 1) {
                /*
                 * Green site. Which chroma lies along the row depends on
                 * whether the horizontal neighbour is red or blue.
                 */
                /* Both horizontal neighbours of a green site are the same
                 * colour, so either one answers the question. */
                int row_is_red = (cfa_color(phase, x + 1, r) == 0);

                o[1] = (uint16_t)v;
                if (row_is_red) {
                    o[0] = (uint16_t)mhc_c_at_g(row, stride, bx, band_w, 1);
                    o[2] = (uint16_t)mhc_c_at_g(row, stride, bx, band_w, 0);
                } else {
                    o[0] = (uint16_t)mhc_c_at_g(row, stride, bx, band_w, 0);
                    o[2] = (uint16_t)mhc_c_at_g(row, stride, bx, band_w, 1);
                }
            } else if (c == 0) {
                o[0] = (uint16_t)v;
                o[1] = (uint16_t)mhc_g_at_rb(row, stride, bx, band_w);
                o[2] = (uint16_t)mhc_rb_at_br(row, stride, bx, band_w);
            } else {
                o[2] = (uint16_t)v;
                o[1] = (uint16_t)mhc_g_at_rb(row, stride, bx, band_w);
                o[0] = (uint16_t)mhc_rb_at_br(row, stride, bx, band_w);
            }
        }
    }
}

void se_bin_band(const uint16_t *band, int stride, int x0,
                 int width, int nrows, int phase, int scale, uint16_t *out)
{
    int r, x, dy, dx;
    int quads = scale / 2;              /* quads averaged per axis */
    int n = quads * quads;

    for (r = 0; r < nrows; r++) {
        uint16_t *o = out + (size_t)r * width * 3;
        for (x = 0; x < width; x++, o += 3) {
            uint32_t acc[3] = { 0, 0, 0 };
            int gcount = 0;
            for (dy = 0; dy < scale; dy++) {
                const uint16_t *row = band + (size_t)(r * scale + dy) * stride;
                for (dx = 0; dx < scale; dx++) {
                    int bx = x0 + x * scale + dx;
                    /*
                     * The phase is fixed across the frame, and both offsets
                     * walk whole quads, so the colour is just the phase
                     * evaluated at the offset within the quad.
                     */
                    int c = cfa_color(phase, dx, dy);
                    acc[c] += row[bx];
                    if (c == 1) gcount++;
                }
            }
            o[0] = (uint16_t)(acc[0] / (uint32_t)n);
            o[1] = (uint16_t)(gcount ? acc[1] / (uint32_t)gcount : 0);
            o[2] = (uint16_t)(acc[2] / (uint32_t)n);
        }
    }
}
