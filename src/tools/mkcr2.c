/*
 * mkcr2 - a raw file anyone may have, for the simulator.
 *
 * tools/sim700d needs a CR2 to develop, and the ones in samples/ are the
 * owner's own photos, which are not committed. So this makes one from
 * doc/stocks.jpg, which is public already: it takes the PROVIA frame out of
 * the contact sheet, scales it up to the 700D's 5184x3456 and wraps it the way
 * Canon does, a JPEG in IFD0 and just enough TIFF around it. There is no sensor
 * data in it, only the embedded preview, which is the one part of a raw the
 * 700D's develop path reads.
 *
 * usage: mkcr2 <stocks.jpg> <out.CR2>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "jpeg_enc.h"
#include "jpeg_dec.h"

#define OUT_W 5184
#define OUT_H 3456
/* The sheet's PROVIA frame. The label sits below it. */
#define TILE_X 1360
#define TILE_Y 245
#define TILE_W 339
#define TILE_H 226

static uint8_t *g_sheet;
static int      g_row, g_sw;

static int sheet_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    (void)ctx;
    memcpy(g_sheet + (size_t)g_row * width * 3, rgb, (size_t)nrows * width * 3);
    g_row += nrows;
    g_sw = width;
    return 0;
}

static uint8_t *g_jpg;
static size_t   g_jlen, g_jcap;

static int jpg_out(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (g_jlen + n > g_jcap) {
        g_jcap = (g_jlen + n) * 2;
        g_jpg = realloc(g_jpg, g_jcap);
        if (!g_jpg) return -1;
    }
    memcpy(g_jpg + g_jlen, d, n);
    g_jlen += n;
    return 0;
}

/* ------------------------------------------------------------ the TIFF */
static uint8_t g_hdr[1024];

static void put16(int at, unsigned v) { g_hdr[at] = v & 255; g_hdr[at + 1] = v >> 8; }
static void put32(int at, uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); }
static void entry(int at, int tag, int type, uint32_t count, uint32_t val)
{
    put16(at, tag); put16(at + 2, type); put32(at + 4, count); put32(at + 8, val);
}

int main(int argc, char **argv)
{
    static uint8_t arena[8 * 1024 * 1024];
    static const char make[] = "Canon", model[] = "Canon EOS 700D", when[] = "2026:07:11 12:30:02";
    se_arena a;
    se_jdec_info info;
    se_jpeg *enc;
    uint8_t *in, *strip, *row;
    size_t ilen;
    uint32_t off;
    FILE *f;
    long n;
    int x, y, c, p;

    if (argc < 3) { fprintf(stderr, "usage: mkcr2 stocks.jpg out.CR2\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    ilen = (size_t)n;
    in = malloc(ilen);
    if (!in || fread(in, 1, ilen, f) != ilen) return 1;
    fclose(f);

    if (se_jpeg_dec_info(in, ilen, &info) != SE_OK) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    g_sheet = malloc((size_t)info.width * info.height * 3);
    se_arena_init(&a, arena, sizeof arena);
    if (!g_sheet || se_jpeg_decode(in, ilen, &a, sheet_rows, 0, &info) != SE_OK) return 1;

    se_arena_init(&a, arena, sizeof arena);
    enc = SE_NEW(&a, se_jpeg, 1);
    strip = SE_NEW(&a, uint8_t, SE_JPEG_STRIP_BYTES(OUT_W, 1));
    row = malloc(OUT_W * 3);
    if (!enc || !strip || !row) return 1;
    if (se_jpeg_begin2(enc, OUT_W, OUT_H, 92, strip, 0, jpg_out, 0, 1) != SE_OK) return 1;

    /* Bilinear, in 1/256ths of a source pixel. */
    for (y = 0; y < OUT_H; y++) {
        int sy = y * (TILE_H - 1) * 256 / OUT_H, y0 = sy >> 8, fy = sy & 255;
        for (x = 0; x < OUT_W; x++) {
            int sx = x * (TILE_W - 1) * 256 / OUT_W, x0 = sx >> 8, fx = sx & 255;
            const uint8_t *t = g_sheet + ((size_t)(TILE_Y + y0) * g_sw + TILE_X + x0) * 3, *b = t + (size_t)g_sw * 3;
            for (c = 0; c < 3; c++) {
                int top = t[c] * (256 - fx) + t[c + 3] * fx, bot = b[c] * (256 - fx) + b[c + 3] * fx;
                row[x * 3 + c] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
            }
        }
        if (se_jpeg_write_rows(enc, row, 1) != SE_OK) return 1;
    }
    if (se_jpeg_end(enc) != SE_OK) return 1;

    /* II*, IFD0 at 16, "CR" 2.0, the raw IFD after it, strings and the Exif
     * IFD after that, then the JPEG. The raw IFD names the JPEG too: nothing
     * here decodes sensor data, and the parser wants a strip to exist. */
    off = 512;
    memcpy(g_hdr, "II*\0", 4); put32(4, 16);
    memcpy(g_hdr + 8, "CR\2\0", 4);

    p = 16; put16(p, 8);
    entry(p + 2,      0x0100, 3, 1, OUT_W);
    entry(p + 2 + 12, 0x0101, 3, 1, OUT_H);
    entry(p + 2 + 24, 0x0103, 3, 1, 6);
    entry(p + 2 + 36, 0x010f, 2, sizeof make, 300);
    entry(p + 2 + 48, 0x0110, 2, sizeof model, 308);
    entry(p + 2 + 60, 0x0111, 4, 1, off);
    entry(p + 2 + 72, 0x0117, 4, 1, (uint32_t)g_jlen);
    entry(p + 2 + 84, 0x8769, 4, 1, 200);
    put32(p + 2 + 96, 0);
    put32(12, 120);

    p = 120; put16(p, 5);
    entry(p + 2,      0x0100, 3, 1, OUT_W);
    entry(p + 2 + 12, 0x0101, 3, 1, OUT_H);
    entry(p + 2 + 24, 0x0103, 3, 1, 6);
    entry(p + 2 + 36, 0x0111, 4, 1, off);
    entry(p + 2 + 48, 0x0117, 4, 1, (uint32_t)g_jlen);
    put32(p + 2 + 60, 0);

    p = 200; put16(p, 4);
    entry(p + 2,      0x829a, 5, 1, 340);          /* 1/250 */
    entry(p + 2 + 12, 0x829d, 5, 1, 348);          /* f/8 */
    entry(p + 2 + 24, 0x8827, 3, 1, 200);
    entry(p + 2 + 36, 0x9003, 2, sizeof when, 320);
    put32(p + 2 + 48, 0);

    memcpy(g_hdr + 300, make, sizeof make);
    memcpy(g_hdr + 308, model, sizeof model);
    memcpy(g_hdr + 320, when, sizeof when);
    put32(340, 1); put32(344, 250);
    put32(348, 8); put32(352, 1);

    f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite(g_hdr, 1, 512, f);
    fwrite(g_jpg, 1, g_jlen, f);
    /* The watcher takes anything under a megabyte for a raw still being
     * written. A real one has its sensor data here. */
    for (n = (long)g_jlen + 512; n < 2L * 1024 * 1024; n++) fputc(0, f);
    fclose(f);
    printf("%s: %dx%d, a %ld KB JPEG from one frame of %s\n", argv[2], OUT_W, OUT_H, (long)g_jlen / 1024, argv[1]);
    return 0;
}
