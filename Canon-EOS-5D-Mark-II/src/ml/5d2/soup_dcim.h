/* soup_dcim.h - which DCIM folder is the camera really writing to?
 *
 * Magic Lantern's get_dcim_dir() does not read the folder name from the
 * card: it rebuilds it as the folder number plus Canon's own suffix, e.g.
 * A:/DCIM/108EOS5D. But a DCF camera reuses any existing folder with the
 * right number, whoever made it. A card that was in a Fujifilm first has
 * DCIM/108_FUJI, the 5D Mark II writes IMG_8659.JPG into that, and
 * get_dcim_dir() names a folder that does not exist -- so every frame
 * looked missing and was silently skipped.
 *
 * So soup.c lists DCIM (rarely: at start and when the folder number
 * changes) and picks with soup_dcim_pick() below. No Magic Lantern here,
 * so it builds on the desktop too (dcim_test.c). GPL v2. */
#ifndef SOUP_DCIM_H
#define SOUP_DCIM_H

/* "108EOS5D", "108_FUJI", "108CANON" for folder 108: DCF names are three
 * digits and five more characters. Case is as the card has it. */
static int soup_dcim_match(const char *name, int folder)
{
    int i, n = 0;
    if (folder < 100 || folder > 999) return 0;
    for (i = 0; i < 3; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    n = (name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0');
    if (n != folder) return 0;
    for (i = 3; name[i]; i++) ;
    return i == 8;
}

/* Pick among the entries of DCIM. `has_frame(ctx, name)` says whether the
 * folder holds the camera's current frame (IMG_nnnn.CR2 / .JPG).
 * Returns the index into `names`, or -1 if no folder has this number.
 *   1. a folder with this number that holds the current frame
 *   2. else Canon's own name (number + `canon_suffix`), if it is there
 *   3. else the first folder with this number */
static int soup_dcim_pick(const char *const *names, int count, int folder,
                          const char *canon_suffix,
                          int (*has_frame)(void *ctx, const char *name),
                          void *ctx)
{
    int i, first = -1, canon = -1;
    for (i = 0; i < count; i++) {
        const char *s, *t;
        if (!soup_dcim_match(names[i], folder)) continue;
        if (first < 0) first = i;
        if (has_frame && has_frame(ctx, names[i])) return i;
        for (s = names[i] + 3, t = canon_suffix ? canon_suffix : ""; *s && *s == *t; s++, t++) ;
        if (canon < 0 && *s == 0 && *t == 0) canon = i;
    }
    return canon >= 0 ? canon : first;
}

#endif
