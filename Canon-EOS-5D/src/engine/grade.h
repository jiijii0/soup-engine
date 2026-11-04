/*
 * grade.h - a Lightroom-shaped colour grade, baked into a 3D LUT.
 *
 * This models the parts of an Adobe edit that are a pure per-pixel colour
 * transform: basic tone, point curves, and the HSL bands. Those bake into a
 * cube exactly, so the camera pays the same cost it would for any .cube.
 *
 * What it deliberately does NOT model, because no 3D LUT can:
 *   Texture, Clarity, Dehaze  - local-contrast operators, they depend on a
 *                               pixel's neighbours, not its colour
 *   Sharpness, noise reduction - spatial
 *   Lens corrections           - geometric
 * Grain is also spatial, but it is cheap and a real part of the look, so it
 * lives in the pipeline as a separate post-LUT stage (see se_dev_opts).
 */
#ifndef SE_GRADE_H
#define SE_GRADE_H

#include "se.h"
#include "lut.h"

#define SE_CURVE_MAX_PTS 24
#define SE_HSL_BANDS     8     /* red orange yellow green aqua blue purple magenta */

typedef struct {
    int     n;                       /* 0 = identity */
    uint8_t x[SE_CURVE_MAX_PTS];     /* Lightroom's 0..255 display domain */
    uint8_t y[SE_CURVE_MAX_PTS];
} se_curve;

typedef struct {
    /* --- basic panel, on Lightroom's own scales --------------------- */
    float exposure;      /* stops                       */
    float contrast;      /* -100 .. +100                */
    float highlights;
    float shadows;
    float whites;
    float blacks;
    float vibrance;
    float saturation;

    /* --- curves ------------------------------------------------------ */
    se_curve profile;    /* the camera profile's tone curve, applied first */
    se_curve master;
    se_curve red, green, blue;

    /* --- HSL --------------------------------------------------------- */
    float hue[SE_HSL_BANDS];   /* -100 .. +100 */
    float sat[SE_HSL_BANDS];
    float lum[SE_HSL_BANDS];

    int   monochrome;
    float mono_mix[SE_HSL_BANDS];  /* B&W panel, per-band luminance weights */
} se_grade;

void   se_grade_init(se_grade *g);

/* Apply the grade to one sRGB-encoded RGB triple, in place. */
void   se_grade_apply(const se_grade *g, float *v);
se_err se_grade_bake(const se_grade *g, int size, se_lut3 *out, se_arena *a);

/* Evaluate a point curve at x in [0,1]; monotone cubic through the points. */
float  se_curve_eval(const se_curve *c, float x);

#endif /* SE_GRADE_H */
