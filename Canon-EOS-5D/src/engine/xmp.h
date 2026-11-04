/*
 * xmp.h - read an Adobe Lightroom / Camera Raw sidecar into a grade.
 *
 * Written in freestanding C so the camera can read a .xmp straight off the
 * card: drop IMG_1234.xmp next to IMG_1234.CR2 and the edit travels with
 * the file. That is the whole point of the project -- the look you made in
 * Lightroom, applied in camera, without Lightroom.
 *
 * It reads the settings that are a per-pixel colour transform. Anything
 * spatial is reported in `unsupported` so the caller can say so plainly
 * rather than silently producing a different picture.
 */
#ifndef SE_XMP_H
#define SE_XMP_H

#include "se.h"
#include "grade.h"

#define SE_XMP_UNSUP_MAX 8

typedef struct {
    char  wb_name[24];        /* crs:WhiteBalance, "" if absent      */
    int   wb_preset;          /* resolved se_wb_preset, or -1        */
    float exposure;           /* already copied into the grade       */

    /* grain is spatial, so it lives outside the LUT */
    float grain_amount;       /* 0..100 */
    float grain_size;         /* 0..100 */
    float grain_frequency;    /* 0..100 */

    /* settings present in the file that a 3D LUT cannot express */
    const char *unsupported[SE_XMP_UNSUP_MAX];
    int         unsupported_n;

    int   found_settings;     /* 1 if this really looked like a sidecar */
} se_xmp_info;

se_err se_xmp_parse(const char *text, size_t len, se_grade *g, se_xmp_info *info);

#endif /* SE_XMP_H */
