/*
 * dcim_test.c - check soup_dcim.h's folder picking on the desktop.
 *
 *   cc -std=gnu99 -I../../engine -o dcim_test dcim_test.c && ./dcim_test
 *
 * The first case is the card the 5D Mark II was first tried with: a
 * Fujifilm had made DCIM/100_FUJI and DCIM/108_FUJI, the camera wrote
 * IMG_8659.JPG into 108_FUJI, and Magic Lantern said A:/DCIM/108EOS5D.
 * GPL v2.
 */
#include <stdio.h>
#include <string.h>
#include "soup_dcim.h"
#include "soup_iso.h"

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

    {
        static const struct { const char *name; int want; } fn[] = {
            { "IMG_8661.JPG", 8661 }, { "IMG_0001.CR2", 1 }, { "_MG_1234.JPG", 1234 },
            { "img_0042.jpg", 42 },   { "IMG_9999.cr2", 9999 },
            { "IMG_0000.JPG", 0 },    { "IMG_866.JPG", 0 },  { "IMG_8661.THM", 0 },
            { "IMG_86a1.JPG", 0 },    { "FLM_8661.TMP", 0 }, { "IMG_8661.JPEG", 0 },
            { "IMG_8661JPG.", 0 },    { "MVI_1234.MOV", 0 },
        };
        unsigned i;
        for (i = 0; i < sizeof fn / sizeof fn[0]; i++) {
            int got = soup_frame_number(fn[i].name);
            checks++;
            if (got != fn[i].want) { fails++; printf("FAIL frame_number(%s) = %d, want %d\n", fn[i].name, got, fn[i].want); }
        }
        printf("ok   frame numbers from file names (%u cases)\n", (unsigned)(sizeof fn / sizeof fn[0]));
    }

    {
        static const struct { const char *name; int iso; int raw; } fi[] = {
            { "TRI-X 400", 400, 88 },      { "DELTA 3200", 3200, 112 },  { "VISION3 250D", 250, 83 },
            { "EKTACHROME E100", 100, 72 }, { "PORTRA 160", 160, 77 },    { "PORTRA 800", 800, 96 },
            { "VELVIA", 100, 72 },         { "KODACHROME 64", 100, 72 }, { "CLASSIC CHROME", 200, 80 },
            { "HP5 PLUS", 400, 88 },       { "VERICOLOR III", 160, 77 }, { "PORTRA 400 V2", 400, 88 },
            { "5D CLASSIC", 0, 0 },        { "PACIFIC BLUES", 400, 88 }, { "STANDARD", 0, 0 },
            { "SOUP 800", 800, 96 },       { "PRO 400H", 400, 88 },      { "T-MAX 400", 400, 88 },
            { "ETERNA", 250, 83 },         { "NOSTALGIC NEG", 400, 88 }, { "BLEACH BYPASS", 400, 88 },
            { "VINTAGE KODACHROME", 100, 72 }, { "REGGIES SUPERIA", 400, 88 }, { "EKTAR 100", 100, 72 },
        };
        unsigned i;
        for (i = 0; i < sizeof fi / sizeof fi[0]; i++) {
            int iso = soup_film_iso(fi[i].name), raw = soup_raw_iso(iso);
            checks++;
            if (iso != fi[i].iso || raw != fi[i].raw) {
                fails++;
                printf("FAIL film ISO %s: %d (raw %d), want %d (raw %d)\n", fi[i].name, iso, raw, fi[i].iso, fi[i].raw);
            }
        }
        printf("ok   film box speeds (%u films)\n", (unsigned)(sizeof fi / sizeof fi[0]));
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
