/*
 * lut.h - 1D tone curves and 3D colour LUTs.
 *
 * The 3D LUT domain is sRGB-encoded (gamma) RGB, not linear. That matches
 * how .cube files exported from Lightroom, Capture One and Resolve are
 * actually authored, and it keeps shadow detail resolvable at practical
 * LUT sizes -- a linear-domain LUT would need to be enormous to avoid
 * banding in the toe.
 */
#ifndef SE_LUT_H
#define SE_LUT_H

#include "se.h"

/* ------------------------------------------------------------ 1D curve */
#define SE_LUT1_SIZE 4096          /* 16-bit in, interpolated, 8 KB */

typedef struct {
    uint16_t v[SE_LUT1_SIZE];
} se_lut1;

/* Linear light -> sRGB encoded. */
void se_lut1_srgb_encode(se_lut1 *t);
/* sRGB encoded -> linear light. */
void se_lut1_srgb_decode(se_lut1 *t);

static inline uint16_t se_lut1_eval(const se_lut1 *t, uint16_t x)
{
    /* 4096 entries over 65536 inputs: 16 input steps per entry. */
    unsigned i = x >> 4;
    unsigned f = x & 15;
    unsigned a = t->v[i];
    unsigned b = t->v[i < SE_LUT1_SIZE - 1 ? i + 1 : SE_LUT1_SIZE - 1];
    return (uint16_t)(a + (((int)b - (int)a) * (int)f >> 4));
}

/*
 * Collapse a curve to the 256 values an 8-bit path can actually reach.
 *
 * The capture path grades the JPEG Canon already made: 8-bit in, 8-bit out.
 * Evaluating the 16-bit curve per channel per pixel means a shift, two
 * loads, a subtract, a multiply and a shift, three and a bit million times
 * a frame -- to produce one of 256 answers. Building the 256 answers once
 * turns the whole look into a byte load.
 */
void se_lut1_bytes(const se_lut1 *t, uint8_t *out256);

/* ------------------------------------------------------------- 3D LUT */
#define SE_LUT3_MAX_SIZE 65

typedef struct {
    int       size;        /* N, so N*N*N entries           */
    uint16_t *data;        /* N^3 * 3, red index varies fastest */
} se_lut3;

/* Allocate an N^3 LUT from the arena and fill it with the identity. */
se_err se_lut3_identity(se_lut3 *lut, int size, se_arena *a);

/*
 * Parse an Adobe/Resolve .cube file. Handles LUT_3D_SIZE, DOMAIN_MIN and
 * DOMAIN_MAX, TITLE, comments and blank lines. Values outside [0,1] are
 * clamped. The text does not need to be NUL-terminated.
 */
se_err se_lut3_parse_cube(const char *text, size_t len, se_lut3 *lut, se_arena *a);

/* Tetrahedral interpolation, applied in place to an interleaved RGB16 row. */
void se_lut3_apply_row(const se_lut3 *lut, uint16_t *rgb, int width);

#endif /* SE_LUT_H */
