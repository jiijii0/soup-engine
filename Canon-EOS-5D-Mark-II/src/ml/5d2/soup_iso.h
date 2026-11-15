/* soup_iso.h - a film's box speed, for "Film sets ISO".
 *
 * Loading a film sets the camera to its ISO, as loading a roll would:
 * Tri-X at 400, Delta 3200 at 3200. After the 6D port (soupeng.c). The
 * speed is the last number of two digits or more in the film's NAME
 * (TRI-X 400, VISION3 250D, EKTACHROME E100), or, for names without one,
 * the table below. 0 means the film leaves ISO alone (5D CLASSIC is a
 * digital look; a recipe with no film behind it has no speed).
 *
 * Clamped to the 5D Mark II's native 100..6400: Velvia 50 and Kodachrome
 * 64 get 100 (ISO 50 is the expanded "L", which costs highlight range),
 * and nothing reaches the expanded H settings. No Magic Lantern here, so
 * it is tested on the desktop (dcim_test.c). GPL v2. */
#ifndef SOUP_ISO_H
#define SOUP_ISO_H

static const struct { const char *name; int iso; } soup_box[] = {
    { "5D CLASSIC", 0 },      { "ACROS", 100 },          { "ASTIA", 100 },
    { "BLEACH BYPASS", 500 }, { "CLASSIC CHROME", 64 },  { "CLASSIC NEG", 400 },
    { "EKTACHROME", 100 },    { "ETERNA", 500 },         { "GOLD", 200 },
    { "KODACHROME", 64 },     { "NOSTALGIC NEG", 160 },  { "PORTRA", 400 },
    { "PROVIA", 100 },        { "REALA ACE", 100 },      { "VELVIA", 50 },
    /* this port's own: names without a number */
    { "HP5 PLUS", 400 },      { "VERICOLOR III", 160 },  { "REGGIES PORTRA", 400 },
    { "VINTAGE KODACHROME", 64 }, { "PACIFIC BLUES", 0 },
};

static int soup_str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* The film's ISO as named, 0 if none. Not yet clamped. */
static int soup_box_speed(const char *name)
{
    int i, n = 0, run = 0, cur = 0;
    for (i = 0; i < (int)(sizeof soup_box / sizeof soup_box[0]); i++)
        if (soup_str_eq(name, soup_box[i].name)) return soup_box[i].iso;
    for (i = 0; ; i++) {                    /* the last run of 2+ digits */
        char c = name[i];
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); run++; }
        else { if (run >= 2) n = cur; cur = 0; run = 0; }
        if (!c) break;
    }
    return n >= 25 && n <= 25600 ? n : 0;
}

/* Clamped to what the camera should be set to: 0, or 100..6400. */
static int soup_film_iso(const char *name)
{
    int iso = soup_box_speed(name);
    if (!iso) return 0;
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
