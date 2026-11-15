/* soup_dev.h - develop one frame into a film JPEG. GPL v2. */
#ifndef SOUP_DEV_H
#define SOUP_DEV_H

#include "se.h"
#include "stock.h"

#define FILM_HDR       (256 * 1024)   /* front of the CR2: IFDs + MakerNote */
#define FILM_SCRATCH   (512 * 1024)   /* decoder + encoder working memory   */
#define FILM_LOOK_MAX  (512 * 1024)   /* look stage (grain, halation)       */
#define FILM_OUTCHUNK  (128 * 1024)   /* output written in chunks this big  */
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
