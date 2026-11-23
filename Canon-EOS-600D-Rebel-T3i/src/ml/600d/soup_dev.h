/* soup_dev.h - develop one frame into a film JPEG. GPL v2. */
#ifndef SOUP_DEV_H
#define SOUP_DEV_H

#include "se.h"
#include "stock.h"

/*
 * Working-memory budget, EOS 600D. Cut from the 60D values to leave the
 * 600D's heap room (60D in brackets). Peak for one develop is about 1 MB
 * (60D: about 1.8 MB).
 *
 *   FILM_HDR      cacheable copy of the front of the file      (256 KB)
 *   FILM_DMA      uncacheable card-read bounce buffer          (256 KB; it
 *                 used to be sized like FILM_HDR, but reads loop, so it can
 *                 be small)
 *   FILM_SCRATCH  decoder + encoder arena                      (512 KB; kept:
 *                 the 600D has the same 5184x3456 sensor, so the decode
 *                 needs what the 60D's did)
 *   FILM_LOOK_MAX ceiling for the look stage (grain, halation) (512 KB; the
 *                 engine fits the look under this, so a smaller ceiling can
 *                 simplify heavy grain/halation. Set back to 512 KB for
 *                 output identical to the 60D module)
 *   FILM_OUTCHUNK output is written in two buffers this big    (128 KB)
 *
 * If a CR2 fails with "preview ... does not fit" in ML/LOGS/SOUP.LOG, the
 * JPEG preview starts beyond FILM_HDR: raise FILM_HDR (192 or 256 KB).
 */
#define FILM_HDR       (128 * 1024)
#define FILM_DMA       (32 * 1024)
#define FILM_SCRATCH   (512 * 1024)
#define FILM_LOOK_MAX  (256 * 1024)
#define FILM_OUTCHUNK  (64 * 1024)
#define FILM_SCALE     2              /* half size                          */
#define FILM_QUALITY   88

/* `src` is a CR2 (is_cr2 = 1, its embedded full-size JPEG is used) or a
 * camera JPEG. Writes the film JPEG to `out`. Returns SE_OK or an se_err;
 * on failure `out` may be partial and the caller removes it. */
int soup_develop(const char *src, int is_cr2, const char *out,
                 const se_stock *stock, se_progress_fn progress);

/* Paint a film JPEG, fitted and centred, into a W x H 4:2:2 (UYVY,
 * signed chroma) buffer with `pitch` bytes per row. Stops early if
 * *abort turns non-zero (returns SOUP_VIEW_CANCELLED). */
#define SOUP_VIEW_CANCELLED (-50)
int soup_view_flm(const char *path, uint8_t *buf, int pitch, int W, int H,
                  volatile int *abort);

/* One line to the module's log. Defined by the caller (soup.c on camera). */
void soup_log(const char *fmt, ...);

#endif
