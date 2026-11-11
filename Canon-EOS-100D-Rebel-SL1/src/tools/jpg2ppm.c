/*
 * jpg2ppm - decode a JPEG with the engine's own decoder and write a PPM.
 *
 * For looking at what the camera produced. Using our decoder rather than the
 * system one means a file the system decoder would quietly repair still
 * comes out here the way the engine sees it.
 *
 * usage: jpg2ppm <in.jpg> <out.ppm>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "jpeg_dec.h"

static FILE *g_out;

static int sink(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    (void)ctx;
    fwrite(rgb, 1, (size_t)nrows * (size_t)width * 3, g_out);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    uint8_t *buf;
    se_arena a;
    se_jdec_info info;
    static uint8_t arena[2 * 1024 * 1024];
    se_err rc;

    if (argc < 3) { fprintf(stderr, "usage: jpg2ppm in.jpg out.ppm\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);

    if (se_jpeg_dec_info(buf, (size_t)n, &info) != SE_OK) {
        fprintf(stderr, "not a baseline JPEG this decoder accepts\n");
        return 1;
    }
    g_out = fopen(argv[2], "wb");
    if (!g_out) { perror(argv[2]); return 1; }
    fprintf(g_out, "P6\n%d %d\n255\n", info.width, info.height);

    se_arena_init(&a, arena, sizeof arena);
    rc = se_jpeg_decode(buf, (size_t)n, &a, sink, NULL, &info);
    fclose(g_out);
    printf("%s  %dx%d ncomp=%d rc=%d\n", argv[1], info.width, info.height,
           info.ncomp, rc);
    return rc == SE_OK ? 0 : 1;
}
