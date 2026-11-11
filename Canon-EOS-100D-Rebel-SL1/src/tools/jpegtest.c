/*
 * jpegtest - check the baseline decoder against a reference decode.
 *
 * usage: jpegtest <in.jpg> <reference.bmp>
 *
 * The reference is produced by the system decoder (sips), so a close match
 * means our Huffman, dequantisation, IDCT, upsampling and colour conversion
 * all agree with an implementation that is known good. An exact match is not
 * expected -- IDCT rounding differs between implementations -- so this
 * reports the worst and mean absolute error and fails only on a real
 * divergence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "jpeg_dec.h"

static uint8_t *g_ref;
static int g_w, g_h, g_row;
static long g_sum, g_n;
static int g_worst;

static int sink(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    int y, i;
    for (y = 0; y < nrows && g_row < g_h; y++, g_row++) {
        const uint8_t *mine = rgb + (size_t)y * width * 3;
        const uint8_t *ref  = g_ref + (size_t)g_row * g_w * 3;
        for (i = 0; i < width * 3 && i < g_w * 3; i++) {
            int d = mine[i] - ref[i];
            if (d < 0) d = -d;
            if (d > g_worst) g_worst = d;
            g_sum += d; g_n++;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long L; void *buf; se_arena ar; void *am;
    se_jdec_info info;
    FILE *r; unsigned char hdr[54]; long dataoff; int y;
    se_err rc;

    if (!f) { puts("no jpeg"); return 2; }
    fseek(f, 0, SEEK_END); L = ftell(f); rewind(f);
    buf = malloc(L);
    if (fread(buf, 1, L, f) != (size_t)L) return 2;
    fclose(f);

    r = fopen(argv[2], "rb");
    if (!r) { puts("no reference bmp"); return 2; }
    if (fread(hdr, 1, 54, r) != 54) return 2;
    dataoff = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16);
    g_w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16);
    g_h = (int)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
    if (g_h < 0) g_h = -g_h;
    g_ref = malloc((size_t)g_w * g_h * 3);
    fseek(r, dataoff, SEEK_SET);
    {
        size_t pad = ((size_t)g_w * 3 + 3) & ~3u;
        unsigned char *line = malloc(pad);
        for (y = 0; y < g_h; y++) {
            int x;
            if (fread(line, 1, pad, r) != pad) break;
            for (x = 0; x < g_w; x++) {      /* BMP is BGR */
                g_ref[(size_t)y * g_w * 3 + x * 3 + 0] = line[x * 3 + 2];
                g_ref[(size_t)y * g_w * 3 + x * 3 + 1] = line[x * 3 + 1];
                g_ref[(size_t)y * g_w * 3 + x * 3 + 2] = line[x * 3 + 0];
            }
        }
        free(line);
    }
    fclose(r);

    am = malloc(16u << 20);
    se_arena_init(&ar, am, 16u << 20);
    if (se_jpeg_dec_info(buf, L, &info) != SE_OK) { puts("header parse FAILED"); return 1; }
    printf("decoded header: %dx%d, %d components (reference %dx%d)\n",
           info.width, info.height, info.ncomp, g_w, g_h);
    if (info.width != g_w || info.height != g_h) { puts("SIZE MISMATCH"); return 1; }

    rc = se_jpeg_decode(buf, L, &ar, sink, NULL, &info);
    if (rc != SE_OK) { printf("decode FAILED rc=%d\n", rc); return 1; }

    printf("rows produced: %d / %d\n", g_row, g_h);
    printf("worst channel error: %d   mean: %.3f   over %ld samples\n",
           g_worst, g_n ? (double)g_sum / g_n : 0.0, g_n);
    if (g_row != g_h) { puts("RESULT: wrong number of rows"); return 1; }
    if (g_worst > 8 || (g_n && (double)g_sum / g_n > 1.0)) {
        puts("RESULT: FAIL - diverges from the reference");
        return 1;
    }
    puts("RESULT: matches the reference decoder");
    return 0;
}
