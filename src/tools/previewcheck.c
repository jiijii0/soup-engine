/*
 * previewcheck - the desktop twin of the camera's develop.
 *
 * Does exactly what ml/payload/film.c does: find the JPEG Canon embedded in
 * the CR2, decode it at the requested scale, run the stock's per-channel
 * curves over it, and re-encode. Prints the same checksum the camera logs,
 * so the two can be compared directly, and writes the JPEG so the look can
 * be judged by eye.
 *
 * "Exactly" is load-bearing: this is what tools/simtest.sh compares the
 * simulated camera against byte for byte. The downscale used to live here
 * as a 2x2 box filter over the decoder's full-size output; it now lives
 * inside the decoder as a 4-point IDCT, and it had to move in both files at
 * once or the comparison would have failed for the wrong reason.
 *
 * usage: previewcheck <in.CR2> <stock.fs> <scale> [out.jpg] [420]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "cr2.h"
#include "lut.h"
#include "stock.h"
#include "jpeg_enc.h"
#include "jpeg_dec.h"
#include "look.h"

#define FNV_INIT 2166136261u
static uint32_t hb(uint32_t h, const void *p, int n)
{
    const unsigned char *b = p; int i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/*
 * The look itself is engine/look.c, the same translation unit the camera
 * runs. This file used to carry its own copy of the grade -- a per-channel
 * LUT applied inline -- and the camera carried another. Two copies of the
 * thing whose whole job is to be identical on both sides.
 */
typedef struct {
    se_jpeg *enc; se_look_run *run; int ow;
    uint32_t hash; int err;
} look_ctx;

static int look_emit(void *ctx, const uint8_t *row, int width)
{
    look_ctx *L = ctx;
    L->hash = hb(L->hash, row, width * 3);
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) { L->err = 1; return -1; }
    return 0;
}

static int look_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    look_ctx *L = ctx;
    if (width != L->ow) { L->err = 1; return -1; }
    if (se_look_rows(L->run, rgb, nrows, look_emit, L) != SE_OK) {
        L->err = 1; return -1;
    }
    return 0;
}

static int wr(void *ctx, const uint8_t *d, size_t n)
{ return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1; }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long L; void *buf; uint32_t poff, plen; int pw, ph;
    se_cr2_info info; se_jdec_info ji; se_stock st; static se_lut1 tone[3];
    se_arena ar; void *am; look_ctx lc; se_jpeg *enc; se_jpeg_exif ex;
    se_look lk;
    FILE *sf, *of; char *text; long tl;
    int scale = (argc > 3) ? atoi(argv[3]) : 2, ow, oh;
    const char *outp = (argc > 4) ? argv[4] : "/dev/null";
    /* "420" as a fifth argument encodes 4:2:0, as the 700D does */
    int c420 = (argc > 5) && strcmp(argv[5], "420") == 0;

    if (!f) { puts("no cr2"); return 2; }
    fseek(f, 0, SEEK_END); L = ftell(f); rewind(f);
    buf = malloc(L); if (fread(buf, 1, L, f) != (size_t)L) return 2; fclose(f);

    if (se_cr2_find_preview(buf, L, &poff, &plen, &pw, &ph) != SE_OK) {
        puts("no preview"); return 2;
    }
    if (se_cr2_parse(buf, L, &info) != SE_OK) memset(&info, 0, sizeof info);
    if (se_jpeg_dec_info((char *)buf + poff, plen, &ji) != SE_OK) {
        puts("bad preview"); return 2;
    }

    sf = fopen(argv[2], "rb");
    fseek(sf, 0, SEEK_END); tl = ftell(sf); rewind(sf);
    text = malloc(tl); if (fread(text, 1, tl, sf) != (size_t)tl) return 2; fclose(sf);
    se_stock_init(&st);
    if (se_stock_parse(text, tl, &st) != SE_OK) { puts("bad stock"); return 2; }
    se_jpeg_scaled_size(ji.width, ji.height, scale, &ow, &oh);
    am = malloc(32u << 20); se_arena_init(&ar, am, 32u << 20);
    memset(&lc, 0, sizeof lc);
    enc = SE_NEW(&ar, se_jpeg, 1);

    /*
     * se_look_fit with the same budget the camera uses. This desktop has 32
     * MB of arena and needs no cap at all, but a radius the camera would
     * have reduced must be reduced here too or the two grade differently
     * and the hash this program prints proves nothing.
     */
    se_look_from_stock(&st, scale, &lk);
    se_look_fit(&lk, ow, SE_LOOK_BUDGET);
    if (se_look_begin(&lc.run, &lk, ow, &ar) != SE_OK) {
        puts("look fail"); return 2;
    }
    lc.ow = ow; lc.hash = FNV_INIT; lc.enc = enc;

    of = fopen(outp, "wb");
    memset(&ex, 0, sizeof ex);
    ex.make = info.make[0] ? info.make : "Canon";
    ex.model = info.model[0] ? info.model : "Canon EOS 5D";
    ex.software = st.name; ex.datetime = info.datetime;
    ex.exposure_time = info.exposure_time; ex.aperture = info.aperture;
    ex.iso = info.iso;
    {
        uint8_t *strip = SE_NEW(&ar, uint8_t, SE_JPEG_STRIP_BYTES(ow, c420));
        if (se_jpeg_begin2(enc, ow, oh, 88, strip, &ex, wr, of, c420) != SE_OK) {
            puts("enc fail"); return 2;
        }
    }
    if (se_jpeg_decode_scaled((char *)buf + poff, plen, scale, &ar,
                              look_rows, &lc, &ji) != SE_OK) {
        puts("decode fail"); return 2;
    }
    if (se_look_end(lc.run, look_emit, &lc) != SE_OK) {
        puts("look flush fail"); return 2;
    }
    se_jpeg_end(enc); fclose(of);

    printf("%s + %s scale %d -> %dx%d\n", argv[1], st.name, scale, ow, oh);
    printf("halation amount %d radius %d, grain %d cell %d\n",
           lk.hal_amount, lk.hal_radius, lk.grain, lk.grain_cell);
    printf("look hash %08X\n", lc.hash);
    return 0;
}
