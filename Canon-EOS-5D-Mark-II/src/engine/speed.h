/* speed.h - a film's box speed, from its name.
 *
 * One table for the two places that show or use it: the FILM SPEED band
 * on the picker card (ui.c) and "Film sets ISO" (ml/5d2/soup_iso.h). They
 * used to keep a table each, and the picker's only knew the first 21 films
 * and matched a name's start, so PORTRA 160 read "FILM SPEED 400" while
 * loading it set ISO 160.
 *
 * The speed is the last number of two digits or more in the NAME (TRI-X
 * 400, VISION3 250D, EKTACHROME E100), or, for names without one, the
 * table below. The table keeps the speeds the original picker printed for
 * its films: they are the stock author's labels. SE_SPEED_DIGITAL marks a
 * look with no film behind it; 0 means the name says nothing.
 *
 * Header-only, freestanding C, so the desktop tests can use it too. */
#ifndef SE_SPEED_H
#define SE_SPEED_H

#define SE_SPEED_DIGITAL (-1)

static const struct { const char *name; int speed; } se_box[] = {
    /* the original picker's films, as it labelled them */
    { "ACROS", 100 },          { "ASTIA", 100 },          { "BLEACH BYPASS", 400 },
    { "CLASSIC CHROME", 200 }, { "CLASSIC NEG", 400 },    { "ETERNA", 250 },
    { "GOLD", 200 },           { "NOSTALGIC NEG", 400 },  { "PORTRA", 400 },
    { "PROVIA", 100 },         { "REALA ACE", 100 },      { "VELVIA", 50 },
    { "REGGIES PORTRA", 400 }, { "REGGIES SUPERIA", 400 }, { "PACIFIC BLUES", 400 },
    { "VINTAGE KODACHROME", 10 }, { "VERICOLOR III", 160 }, { "KODACHROME", 64 },
    /* the films added from the 7D and 6D ports whose names carry no speed */
    { "EKTACHROME", 100 },     { "HP5 PLUS", 400 },
    { "5D CLASSIC", SE_SPEED_DIGITAL },
};

static int se_speed_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* The film's rated speed, SE_SPEED_DIGITAL, or 0 if the name says nothing. */
static int se_box_speed(const char *name)
{
    int i, n = 0, run = 0, cur = 0;
    if (!name) return 0;
    for (i = 0; i < (int)(sizeof se_box / sizeof se_box[0]); i++)
        if (se_speed_eq(name, se_box[i].name)) return se_box[i].speed;
    for (i = 0; ; i++) {                    /* the last run of 2+ digits */
        char c = name[i];
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); run++; }
        else { if (run >= 2) n = cur; cur = 0; run = 0; }
        if (!c) break;
    }
    return n >= 25 && n <= 25600 ? n : 0;
}

#endif
