/*
 * stock.h - a film stock: per-channel tone response, and nothing else.
 *
 * This is deliberately the exact set of operations that can be applied to
 * MOSAICED sensor data, where every sample is one known colour and no
 * pixel has all three. That constraint is what lets DIGIC do the expensive
 * work: we rewrite the raw buffer in place, then Canon's hardware
 * demosaics, matrixes and JPEGs it at full speed.
 *
 * Expressible here:   per-channel curves, white balance, exposure,
 *                     black point, contrast, matte lift, highlight rolloff
 * Not expressible:    3D LUTs, HSL, saturation, hue rotation -- anything
 *                     that needs R, G and B at the same pixel
 *
 * That is less limiting than it sounds. Film is three emulsion layers, each
 * with its own characteristic curve; per-channel response IS the mechanism.
 *
 * A stock bakes down to three 1D tables. On the camera that is one lookup
 * per sample: ~13 M lookups, seconds rather than minutes.
 */
#ifndef SE_STOCK_H
#define SE_STOCK_H

#include "se.h"
#include "grade.h"
#include "lut.h"

/*
 * What generation of the stock format this engine speaks.
 *
 *   1  per-channel curves, WB, exposure, grain
 *   2  + SAT, SPLIT_SH/HI, HALATION, HAL_TINT, GRAIN_SIZE
 *   3  + HUE_SAT, HUE_ROT, HUE_LUM, and the [RAW] override block
 *
 * This exists because unknown keys are ignored on purpose, so a stock
 * written by something newer than the engine reading it produces no
 * error and the wrong picture. Six changes to engine/look.c in one
 * session each altered the output of an unchanged .fs file, and two of
 * them added keys. Someone tuning a stock in one place and shooting it
 * in another has no way to tell, and will assume they mis-tuned it.
 *
 * So a file may say which generation it was written for, and the engine
 * says so when it cannot keep up.
 */
#define SE_STOCK_VERSION 3

#define SE_STOCK_NAME 24
#define SE_STOCK_DESC 48

/*
 * What the raw path may do differently.
 *
 * The two develop paths do not see the same picture. The JPEG path grades
 * the preview DIGIC already made -- which has been through the camera's
 * Picture Style, so it arrives contrasty and saturated. The raw path
 * rewrites scene-linear mosaiced samples, and DIGIC applies its Picture
 * Style AFTERWARDS, on top of our work. One set of curve points cannot be
 * right for both, and until now a stock had only one set: the same points
 * evaluated in the linear domain for raw and the sRGB domain for JPEG.
 *
 * So a stock can override, for raw only, exactly the things the raw path
 * is physically able to do. Saturation, the per-hue controls, split
 * toning and halation are absent from this struct on purpose -- they need
 * all three channels at one pixel, and before demosaic no pixel has them.
 *
 * Only the raw side exists, not a matching JPEG side: the top level of
 * the file IS the JPEG look, because that is the path the camera uses
 * unless told otherwise. `[RAW]` says "and when it comes from the sensor,
 * these instead".
 */
typedef struct {
    se_curve master, r, g, b;    /* n < 2 means "no override"      */
    float exposure;   int exposure_set;
    float grain;      int grain_set;
    float grain_size; int grain_size_set;
    int   wb_preset;  int wb_set;
} se_stock_raw;

typedef struct {
    /* The format generation this file asked for; 0 when it did not say,
     * which means it predates the key and is read as generation 1. */
    int   version;

    char  name[SE_STOCK_NAME];
    char  desc[SE_STOCK_DESC];

    int   wb_preset;        /* se_wb_preset; -1 = leave as shot */
    float exposure;         /* stops */

    se_curve master;        /* applied to all three channels   */
    se_curve r, g, b;       /* then each channel on its own    */

    /*
     * Everything below is applied by engine/look.c on the JPEG path, where
     * all three channels are present at every pixel. The raw path cannot
     * use any of it: it rewrites MOSAICED samples, one colour per pixel,
     * so it has no neighbours and no cross-channel anything. A stock that
     * leans on these will look different developed from raw, and that is a
     * property of the sensor data, not a bug to fix here.
     */

    float grain;            /* 0..100, applied after DIGIC; 0 = none */
    float grain_size;       /* 0..100 -> a 1..5 pixel cell           */

    /* Saturation, 100 = unchanged. `sat_set` distinguishes "SAT 0",
     * which means monochrome, from a stock file that never said SAT. */
    float sat;
    int   sat_set;

    /* Saturation per hue sector -- R, Y, G, C, B, M -- 100 = unchanged.
     * Defaulted in se_stock_init, not by the memset, because 0 here means
     * "make that hue grey" and silence must not mean that. */
    float hue_sat[6];

    /* Hue rotation per sector, in DEGREES, signed. Positive moves toward
     * the next sector: R->Y->G->C->B->M->R. This is what makes a stock a
     * different colour rather than a stronger one. */
    float hue_rot[6];

    /* How bright each hue renders, 100 = unchanged. Film layers have
     * different speeds; this is why Velvia's blues go dense. */
    float hue_lum[6];

    /* Split tone, signed, in 0..255 output units at full weight. The
     * shadow triple is weighted by (255 - luma), the highlight triple by
     * luma, so a warm-highlight/cool-shadow stock is two lines. */
    float split_sh[3];
    float split_hi[3];

    /* Halation: light scattering off the film base and back up through the
     * emulsion. amount 0 disables it, and then it costs nothing at all --
     * no delay line is allocated. */
    float hal_amount;       /* 0..100                                 */
    float hal_thresh;       /* luma a pixel must beat to glow, 0..255 */
    float hal_radius;       /* pixels, 1..48, at full decode scale    */
    float hal_tint[3];      /* the glow's colour, 0..255              */

    /* Overrides applied only when developing from the sensor data. */
    se_stock_raw raw;
} se_stock;

/* Which develop path is asking. */
#define SE_SRC_JPEG 0
#define SE_SRC_RAW  1

/* The grain a stock wants from a given path, honouring any [RAW]
 * override. The curves are picked inside the two bake functions, which
 * already know which path they are -- this is the one value a caller
 * outside them needs. */
float se_stock_grain(const se_stock *s, int src);
float se_stock_grain_size(const se_stock *s, int src);
int   se_stock_wb(const se_stock *s, int src);

void se_stock_init(se_stock *s);

/*
 * Parse the text format:
 *
 *   VERSION   3
 *   NAME      VELVIA
 *   DESC      SATURATED TRANSPARENCY
 *   WB        DAYLIGHT
 *   EXPOSURE  +0.10
 *   CURVE     0,0 22,16 127,127 255,255
 *   CURVE_B   0,0 52,60 255,255
 *   GRAIN     20
 *   GRAIN_SIZE 30
 *   SAT       120
 *   HUE_SAT   110,70,55,80,95,105   ; R Y G C B M, 100 = unchanged
 *   HUE_ROT   0,-8,-14,6,10,0       ; degrees, + = toward the next
 *   HUE_LUM   104,96,88,98,82,100   ; how bright each hue renders
 *   SPLIT_SH  -6,0,10
 *   SPLIT_HI  10,4,-6
 *   HALATION  40 190 14      ; amount, threshold, radius
 *   HAL_TINT  255,70,30
 *
 * Unknown keys are ignored so a newer stock file still loads on older code.
 *
 * Everything above applies to the JPEG path, which is what the camera
 * uses unless told otherwise. A `[RAW]` line starts a block that
 * overrides, for the sensor-data path only, the handful of things that
 * path can actually do:
 *
 *   [RAW]
 *   CURVE     0,0 20,14 128,132 255,255
 *   EXPOSURE  +0.15
 *   GRAIN     12
 *
 * `[JPEG]` returns to the common block. Keys that need all three colour
 * channels -- SAT, HUE_*, SPLIT_*, HALATION -- are refused inside [RAW]
 * with a warning, because the raw path cannot honour them.
 */
se_err se_stock_parse(const char *text, size_t len, se_stock *s);

/*
 * Bake to three channel tables mapping white-balanced linear 16-bit in to
 * linear 16-bit out. The curves are authored in a display-referred domain
 * (that is where anyone editing them can reason about them), so the bake
 * encodes, applies the curve, and decodes back to linear.
 */
/*
 * Bake the look into three tables.
 *
 * se_stock_bake is linear in, linear out: for the raw pipeline, which hands
 * it scene-linear sensor data.
 *
 * se_stock_bake_srgb is sRGB in, sRGB out: for the preview pipeline, which
 * works on the JPEG DIGIC already made. Using the wrong one of these is not
 * a subtle difference -- it evaluates the curve at the wrong point and
 * gamma-decodes the result, which made every curved stock dark and muddy.
 */
void se_stock_bake(const se_stock *s, se_lut1 out[3]);
void se_stock_bake_srgb(const se_stock *s, se_lut1 out[3]);

/* The sRGB bake straight to the 256-entry byte tables an 8-bit path wants.
 * Same curve, no se_lut1 in between. */
void se_stock_bake_srgb8(const se_stock *s, uint8_t out[3][256]);

#endif /* SE_STOCK_H */
