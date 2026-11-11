/*
 * looktest - what a film stock does to DIGIC's own output.
 *
 * Every CR2 embeds a full-size JPEG that the camera has already demosaiced,
 * white balanced and colour corrected. Applying a stock's per-channel curves
 * to that is the architecture originally agreed -- let DIGIC do the hard
 * part, and own the tone response -- and it needs none of the lossless-JPEG
 * decoding that the raw path spends two minutes on.
 *
 * This takes a 24-bit BMP (standing in for the decoded preview), applies the
 * baked curves, and writes the JPEG through the engine's own encoder. It
 * exists to answer the look question on the desktop before any of it is
 * written for the camera.
 *
 * usage: looktest <in.bmp> <stock.fs> <out.jpg> [scale]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "lut.h"
#include "stock.h"
#include "jpeg_enc.h"

static int wr(void *ctx, const uint8_t *d, size_t n)
{
    return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    unsigned char hdr[54];
    int w, h, scale = (argc > 4) ? atoi(argv[4]) : 1;
    long dataoff;
    unsigned char *row, *out;
    se_stock st;
    se_lut1 tone[3];
    char *text;
    long tl;
    FILE *sf, *of;
    se_jpeg *je;
    se_jpeg_exif ex;
    uint8_t *strip;
    se_arena ar;
    void *am;
    int y, ow, oh;

    if (!f) { puts("no bmp"); return 2; }
    if (fread(hdr, 1, 54, f) != 54) return 2;
    dataoff = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16);
    w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16);
    h = (int)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
    if (h < 0) h = -h;                     /* top-down, which sips writes */

    sf = fopen(argv[2], "rb");
    if (!sf) { puts("no stock"); return 2; }
    fseek(sf, 0, SEEK_END); tl = ftell(sf); rewind(sf);
    text = malloc(tl + 1);
    if (fread(text, 1, tl, sf) != (size_t)tl) return 2;
    fclose(sf);
    se_stock_init(&st);
    if (se_stock_parse(text, tl, &st) != SE_OK) { puts("bad stock"); return 2; }
    se_stock_bake(&st, tone);

    ow = w / scale; oh = h / scale;
    of = fopen(argv[3], "wb");
    am = malloc(4 << 20);
    se_arena_init(&ar, am, 4 << 20);
    je    = SE_NEW(&ar, se_jpeg, 1);
    strip = SE_NEW(&ar, uint8_t, 8 * ow * 3);
    memset(&ex, 0, sizeof ex);
    ex.software = st.name;
    ex.make = "Canon"; ex.model = "Canon EOS 5D";
    if (se_jpeg_begin(je, ow, oh, 88, strip, &ex, wr, of) != SE_OK) {
        puts("enc fail"); return 2;
    }

    row = malloc((size_t)w * 3 + 4);
    out = malloc((size_t)ow * 3);
    fseek(f, dataoff, SEEK_SET);
    for (y = 0; y < oh; y++) {
        int x, c, k;
        long acc[3];
        /* `scale` source rows per output row, box filtered */
        memset(out, 0, (size_t)ow * 3);
        for (k = 0; k < scale; k++) {
            size_t pad = ((size_t)w * 3 + 3) & ~3u;
            if (fread(row, 1, pad, f) != pad) break;
            for (x = 0; x < ow; x++) {
                int sx;
                acc[0] = acc[1] = acc[2] = 0;
                for (sx = 0; sx < scale; sx++) {
                    const unsigned char *p = row + (size_t)(x * scale + sx) * 3;
                    acc[0] += p[2]; acc[1] += p[1]; acc[2] += p[0];  /* BGR */
                }
                for (c = 0; c < 3; c++)
                    out[x * 3 + c] = (unsigned char)
                        (out[x * 3 + c] + acc[c] / scale / scale);
            }
        }
        /* the look: one curve per channel, exactly as the camera would */
        for (x = 0; x < ow; x++)
            for (c = 0; c < 3; c++) {
                uint16_t v = (uint16_t)(out[x * 3 + c] * 257);
                out[x * 3 + c] = (unsigned char)(se_lut1_eval(&tone[c], v) >> 8);
            }
        se_jpeg_write_rows(je, out, 1);
    }
    se_jpeg_end(je);
    fclose(of);
    printf("%s: %dx%d -> %dx%d through %s\n", argv[3], w, h, ow, oh, st.name);
    return 0;
}
