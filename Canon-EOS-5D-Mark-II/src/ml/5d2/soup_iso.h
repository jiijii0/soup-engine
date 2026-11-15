/* soup_iso.h - a film's box speed, for "Film sets ISO".
 *
 * Loading a film sets the camera to its ISO, as loading a roll would:
 * Tri-X at 400, Delta 3200 at 3200. After the 6D port (soupeng.c). The
 * speed comes from the engine's speed.h, the same one the picker card's
 * FILM SPEED band shows, so the card and the ISO always agree. A digital
 * look (5D CLASSIC) or a name with no speed leaves ISO alone.
 *
 * Clamped to the 5D Mark II's native 100..6400: Velvia 50, Kodachrome 64
 * and Vintage Kodachrome 10 get 100 (ISO 50 is the expanded "L", which
 * costs highlight range), and nothing reaches the expanded H settings.
 * No Magic Lantern here, so it is tested on the desktop (dcim_test.c).
 * GPL v2. */
#ifndef SOUP_ISO_H
#define SOUP_ISO_H

#include "speed.h"

/* Clamped to what the camera should be set to: 0, or 100..6400. */
static int soup_film_iso(const char *name)
{
    int iso = se_box_speed(name);
    if (iso <= 0) return 0;
    if (iso < 100) iso = 100;
    if (iso > 6400) iso = 6400;
    return iso;
}

/* Canon's raw ISO, in 1/8 EV: 72 is ISO 100, and the 1/3 stops are
 * 3, 5 (rounded), 8 past each stop. Nearest third, by ratio. */
static int soup_raw_iso(int iso)
{
    static const int thirds[] = { 100, 125, 160, 200, 250, 320, 400, 500, 640, 800,
                                  1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400 };
    int k, best = 0;
    if (iso <= 0) return 0;
    for (k = 1; k < (int)(sizeof thirds / sizeof thirds[0]); k++)
        if ((long)iso * iso > (long)thirds[k - 1] * thirds[k]) best = k;
    return 72 + (best * 8 + 1) / 3;
}

#endif
