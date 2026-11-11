/*
 * hashcheck - the desktop half of the camera's bracketing checksums.
 *
 * ml/payload/film.c hashes every sample the decoder hands to the spool, and
 * then hashes four rows read back out of the spool afterwards, and logs
 * both. This computes the same two numbers from the same raw on a machine
 * where the answer is known to be right, so a mismatch says which stage on
 * the camera is at fault rather than only that something is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "se.h"
#include "cr2.h"

#define FNV_INIT 2166136261u
static uint32_t hb(uint32_t h, const void *p, int n)
{
    const unsigned char *b = p;
    int i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static uint32_t dh[2];
static int      half;
static uint16_t *frame;
static int       pitch;

static int sink(void *ctx, int row, int col0, int n, const uint16_t *s)
{
    int w = (col0 >= half) ? 1 : 0;
    dh[w] = hb(dh[w], s, n * 2);
    memcpy(frame + (size_t)row * pitch + col0, s, (size_t)n * 2);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long L; void *buf; se_cr2_info info; se_arena ar; void *am;
    static const int pr[4] = { 0, 1000, 2000, 2900 };
    uint32_t rh = FNV_INIT;
    int q, k;

    if (!f) { puts("no file"); return 2; }
    fseek(f, 0, SEEK_END); L = ftell(f); rewind(f);
    buf = malloc(L);
    if (fread(buf, 1, L, f) != (size_t)L) return 2;
    fclose(f);

    if (se_cr2_parse(buf, L, &info) != SE_OK) { puts("parse failed"); return 2; }
    half  = info.raw_width / 2;
    pitch = info.raw_width;
    frame = malloc((size_t)pitch * info.raw_height * 2);
    am = malloc(4u << 20);
    se_arena_init(&ar, am, 4u << 20);
    dh[0] = dh[1] = FNV_INIT;
    if (se_cr2_decode_sink(buf, L, &info, sink, 0, &ar) != SE_OK) {
        puts("decode failed"); return 2;
    }

    /* Same traversal order as the camera: row outer, half inner. */
    for (q = 0; q < 4; q++)
        for (k = 0; k < 2; k++)
            rh = hb(rh, frame + (size_t)pr[q] * pitch + (size_t)k * half, half * 2);

    printf("%s\n", argv[1]);
    printf("  decode hash L=%08X R=%08X\n", dh[0], dh[1]);
    printf("  readback hash %08X\n", rh);
    return 0;
}
