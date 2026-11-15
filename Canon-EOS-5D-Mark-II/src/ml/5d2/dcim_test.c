/*
 * dcim_test.c - check soup_dcim.h's folder picking on the desktop.
 *
 *   cc -std=gnu99 -o dcim_test dcim_test.c && ./dcim_test
 *
 * The first case is the card the 5D Mark II was first tried with: a
 * Fujifilm had made DCIM/100_FUJI and DCIM/108_FUJI, the camera wrote
 * IMG_8659.JPG into 108_FUJI, and Magic Lantern said A:/DCIM/108EOS5D.
 * GPL v2.
 */
#include <stdio.h>
#include <string.h>
#include "soup_dcim.h"

static int fails, checks;

/* has_frame: the folder whose name is in ctx holds the current frame */
static int frame_in(void *ctx, const char *name)
{
    return ctx && strcmp((const char *)ctx, name) == 0;
}

static void pick(const char *what, const char *const *names, int n, int folder,
                 const char *frame_folder, const char *want)
{
    int i = soup_dcim_pick(names, n, folder, "EOS5D", frame_in, (void *)frame_folder);
    const char *got = i >= 0 ? names[i] : "(none)";
    checks++;
    if (strcmp(got, want)) { fails++; printf("FAIL %s: got %s, want %s\n", what, got, want); }
    else printf("ok   %s: %s\n", what, got);
}

static void match(const char *name, int folder, int want)
{
    checks++;
    if (soup_dcim_match(name, folder) != want) {
        fails++;
        printf("FAIL match(%s, %d) != %d\n", name, folder, want);
    }
}

int main(void)
{
    const char *fuji[]  = { "100_FUJI", "108_FUJI" };
    const char *canon[] = { "100CANON", "108EOS5D" };
    const char *both[]  = { "108_FUJI", "108EOS5D" };
    const char *other[] = { "108_FUJI", "109_FUJI" };
    const char *junk[]  = { "MISC", "108", "108EOS5DX", "10A_FUJI" };

    pick("Fujifilm card, frame in 108_FUJI", fuji, 2, 108, "108_FUJI", "108_FUJI");
    pick("Fujifilm card, frame not found yet", fuji, 2, 108, 0, "108_FUJI");
    pick("Canon's own folder", canon, 2, 108, "108EOS5D", "108EOS5D");
    pick("both, frame in 108EOS5D", both, 2, 108, "108EOS5D", "108EOS5D");
    pick("both, frame in 108_FUJI", both, 2, 108, "108_FUJI", "108_FUJI");
    pick("both, frame in neither: Canon's name", both, 2, 108, 0, "108EOS5D");
    pick("number matters", other, 2, 109, 0, "109_FUJI");
    pick("no folder with this number", fuji, 2, 101, 0, "(none)");
    pick("only non-DCF names", junk, 4, 108, 0, "(none)");

    match("108_FUJI", 108, 1);
    match("108EOS5D", 108, 1);
    match("108EOS5D", 109, 0);
    match("108", 108, 0);
    match("108EOS5DX", 108, 0);
    match("10A_FUJI", 108, 0);
    match("099CANON", 99, 0);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
