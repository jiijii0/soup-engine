/* soup_dev.h - develop one frame into a film JPEG. GPL v2. */
#ifndef SOUP_DEV_H
#define SOUP_DEV_H

#include "se.h"
#include "stock.h"

#define FILM_HDR       (256 * 1024)   /* front of the CR2: IFDs + MakerNote */
#define FILM_SCRATCH   (512 * 1024)   /* decoder + encoder working memory   */
#define FILM_LOOK_MAX  (512 * 1024)   /* look stage (grain, halation), HALF */
#define FILM_OUTCHUNK  (128 * 1024)   /* output written in chunks this big  */

/*
 * Two sizes. HALF is 2800x1872 from a 5D Mark II frame, about 45 s.
 * FULL is the frame's own 5616x3744, the size of Canon's JPEG L, for
 * several times as long.
 *
 * FULL gets a bigger look budget: at full width the glow is twice as wide,
 * and 512 KB would shorten it on six stocks (Soup 800 needs 970 KB). With
 * 1 MB no stock is cut, so FULL is the same look as HALF, larger. And a
 * higher quality, since the point of FULL is a file to set beside Canon's
 * own JPEG L.
 */
#define FILM_SCALE_HALF   2
#define FILM_SCALE_FULL   1
#define FILM_QUALITY_HALF 88
#define FILM_QUALITY_FULL 95
#define FILM_LOOK_MAX_FULL (1024 * 1024)
/* Decoder + encoder at full width: ~381 KB for a 4:2:2 JPEG (Canon's
 * camera JPEGs), ~650 KB for 4:2:0, whose row buffers are twice as tall.
 * 512 KB ran out on a 4:2:0 CR2 preview. */
#define FILM_SCRATCH_FULL  (768 * 1024)

/* `src` is a CR2 (is_cr2 = 1, its embedded full-size JPEG is used) or a
 * camera JPEG. Writes the film JPEG to `out` at `scale` (FILM_SCALE_HALF
 * or FILM_SCALE_FULL) and JPEG `quality`. Returns SE_OK or an se_err; on
 * failure `out` may be partial and the caller removes it. */
int soup_develop(const char *src, int is_cr2, const char *out,
                 const se_stock *stock, int scale, int quality,
                 se_progress_fn progress);

/* Paint a film JPEG, fitted and centred, into a W x H 4:2:2 (UYVY,
 * signed chroma) buffer with `pitch` bytes per row. Stops early if
 * *abort turns non-zero (returns SOUP_VIEW_CANCELLED). */
#define SOUP_VIEW_CANCELLED (-50)
int soup_view_flm(const char *path, uint8_t *buf, int pitch, int W, int H,
                  volatile int *abort);

/* One line to the module's log. Defined by the caller (soup.c on camera). */
void soup_log(const char *fmt, ...);

#endif
