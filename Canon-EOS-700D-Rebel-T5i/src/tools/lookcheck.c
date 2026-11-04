/*
 * lookcheck - prove engine/look.c's streaming halation against a reference.
 *
 * WHY THIS EXISTS. look.c grades rows as the JPEG decoder hands them over,
 * one at a time, and halation needs the rows above and below. So it keeps a
 * delay line and emits row k only once row k+radius has arrived. That is
 * the kind of code that comes out looking almost right -- shifted by one
 * row, or short by `radius` rows at the bottom -- and "almost right" is
 * invisible in a photograph of a cat.
 *
 * So here the same look is computed a second time, the obvious way, over a
 * whole image held in memory, with no delay line and no ring buffer. The
 * two must agree byte for byte. The reference is written out longhand on
 * purpose: if it shared code with look.c it could not catch look.c's bugs.
 *
 * It also feeds the rows in at several chunk sizes, because the decoder's
 * chunking is not ours to choose -- it hands over whatever an MCU row
 * yields -- and a ring buffer that works at 16 rows a call and fails at 1
 * would fail only on real images.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "look.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int cl8(int v) { return clampi(v, 0, 255); }
static int luma(int r, int g, int b) { return (77*r + 150*g + 29*b) >> 8; }

/* The sine table, rebuilt independently. The Taylor series IS the spec --
 * see engine/look.c -- so this reproduces it rather than calling libm,
 * which would disagree in the last place on most entries. */
static int ref_sin_tab[1536];
static void ref_tables(void)
{
    int i;
    if (ref_sin_tab[384]) return;
    for (i = 0; i < 1536; i++) {
        int q = i & 1535, sign = 1;
        float x, x2, v;
        if (q >= 768) { q -= 768; sign = -1; }
        if (q > 384) q = 768 - q;
        x  = (float)q * (3.14159265f / 768.0f);
        x2 = x * x;
        v  = x * (1.0f - x2 * (1.0f / 6.0f)
                       * (1.0f - x2 * (1.0f / 20.0f)
                                    * (1.0f - x2 * (1.0f / 42.0f))));
        v *= (float)sign * 256.0f;
        ref_sin_tab[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
}
#define RSIN(a) (ref_sin_tab[((((a) % 1536) + 1536) % 1536)])
#define RCOS(a) RSIN((a) + 384)

static unsigned ghash(unsigned x, unsigned y, unsigned seed)
{
    unsigned h = x*374761393u + y*668265263u + seed*2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* ---- the reference: whole image, no streaming, no ring ------------- */
static void reference(const se_look *lk, const uint8_t *in, int w, int h,
                      uint8_t *out)
{
    int x, y, c, k;
    ref_tables();
    uint8_t *gr   = malloc((size_t)w*h*3);
    uint8_t *mask = malloc((size_t)w*h);

    /* curves, saturation, split tone */
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        int rr = lk->map[0][in[(y*w+x)*3+0]];
        int gg = lk->map[1][in[(y*w+x)*3+1]];
        int bb = lk->map[2][in[(y*w+x)*3+2]];
        if (lk->hue_any) {
            /*
             * The reciprocal is the specified algorithm, not an
             * approximation of one: ARM946E-S has no divide instruction,
             * so 65536/d from a table and one multiply IS how hue is
             * defined here. A reference that used a real divide would
             * disagree by one in the last place on most pixels and would
             * be testing a different function. It is still written out
             * independently -- the table is rebuilt here, not borrowed.
             */
            int mx = rr>gg?(rr>bb?rr:bb):(gg>bb?gg:bb);
            int mn = rr<gg?(rr<bb?rr:bb):(gg<bb?gg:bb);
            int dd = mx-mn, hh, rc;
            if (dd > 0) {
                rc = 65536 / dd;
                if (mx==rr)      hh = ((gg-bb)*rc)>>8;
                else if (mx==gg) hh = (((bb-rr)*rc)>>8) + 512;
                else             hh = (((rr-gg)*rc)>>8) + 1024;
                if (hh<0) hh += 1536;
                if (hh>=1536) hh -= 1536;
                {
                    int sec=hh>>8, f=hh&255, nx=(sec==5?0:sec+1);
                    int ms=lk->hue_sat[sec]+(((lk->hue_sat[nx]-lk->hue_sat[sec])*f)>>8);
                    int ml=lk->hue_lum[sec]+(((lk->hue_lum[nx]-lk->hue_lum[sec])*f)>>8);
                    int mr=lk->hue_rot[sec]+(((lk->hue_rot[nx]-lk->hue_rot[sec])*f)>>8);
                    int l = luma(rr,gg,bb);
                    int u = bb-l, v = rr-l;
                    /* fade out toward neutral -- see SE_HUE_FULL */
                    if (dd < SE_HUE_FULL) {
                        int wt = (dd*256)/SE_HUE_FULL;
                        ms = 256 + (((ms-256)*wt)>>8);
                        ml = 256 + (((ml-256)*wt)>>8);
                        mr = (mr*wt)>>8;
                    }
                    if (mr) {
                        int cs=RCOS(mr), sn=RSIN(mr);
                        int u2=(u*cs - v*sn)>>8, v2=(u*sn + v*cs)>>8;
                        u=u2; v=v2;
                    }
                    if (ms != 256) { u=(u*ms)>>8; v=(v*ms)>>8; }
                    if (ml != 256) l=(l*ml)>>8;
                    rr = l + v;
                    bb = l + u;
                    gg = l - ((77*v + 29*u)/150);
                }
            }
        }
        if (lk->sat != 256) {
            int l = luma(cl8(rr),cl8(gg),cl8(bb));
            rr = l + ((rr-l)*lk->sat >> 8);
            gg = l + ((gg-l)*lk->sat >> 8);
            bb = l + ((bb-l)*lk->sat >> 8);
        }
        if (lk->sh[0]|lk->sh[1]|lk->sh[2]|lk->hi[0]|lk->hi[1]|lk->hi[2]) {
            int l  = luma(cl8(rr),cl8(gg),cl8(bb));
            int wl = 255 - l;
            rr += (lk->sh[0]*wl + lk->hi[0]*l) >> 8;
            gg += (lk->sh[1]*wl + lk->hi[1]*l) >> 8;
            bb += (lk->sh[2]*wl + lk->hi[2]*l) >> 8;
        }
        /* film's chroma rolloff toward white and toward black, rebuilt
         * here the same way engine/look.c builds its table */
        {
            int yy = (77*rr + 150*gg + 29*bb) >> 8, w;
            if (yy<0) yy=0; if (yy>255) yy=255;
            if (yy < 24)       w = 150 + (yy*106)/24;
            else if (yy <= 112) w = 256;
            else               w = 256 - ((yy-112)*140)/143;
            if (w < 0) w = 0; if (w > 255) w = 255;
            if (w != 256) {
                rr = yy + (((rr-yy)*w)>>8);
                gg = yy + (((gg-yy)*w)>>8);
                bb = yy + (((bb-yy)*w)>>8);
            }
        }
        /* gamut fit: pull chroma in rather than clipping channels.
         * Written out longhand with a real divide -- see engine/look.c,
         * where the reciprocal table is the specification; the shift-down
         * is reproduced here so the two agree exactly. */
        if (rr<0||rr>255||gg<0||gg>255||bb<0||bb>255) {
            int yy = (77*rr + 150*gg + 29*bb) >> 8, sc = 256, dd;
            if (yy<0) yy=0; if (yy>255) yy=255;
            #define FITS(num,den) ({ int N_=(num),D_=(den);                 while (D_>255){D_>>=1;N_>>=1;}                 D_<=0 ? 256 : ((N_*(65536/D_))>>8); })
            dd = rr-yy;
            if (rr>255 && dd>0) { int t=FITS(255-yy,dd);  if(t<sc)sc=t; }
            if (rr<0   && dd<0) { int t=FITS(yy,-dd);     if(t<sc)sc=t; }
            dd = gg-yy;
            if (gg>255 && dd>0) { int t=FITS(255-yy,dd);  if(t<sc)sc=t; }
            if (gg<0   && dd<0) { int t=FITS(yy,-dd);     if(t<sc)sc=t; }
            dd = bb-yy;
            if (bb>255 && dd>0) { int t=FITS(255-yy,dd);  if(t<sc)sc=t; }
            if (bb<0   && dd<0) { int t=FITS(yy,-dd);     if(t<sc)sc=t; }
            #undef FITS
            if (sc<0) sc=0; if (sc>256) sc=256;
            rr = yy + (((rr-yy)*sc)>>8);
            gg = yy + (((gg-yy)*sc)>>8);
            bb = yy + (((bb-yy)*sc)>>8);
        }
        gr[(y*w+x)*3+0] = (uint8_t)cl8(rr);
        gr[(y*w+x)*3+1] = (uint8_t)cl8(gg);
        gr[(y*w+x)*3+2] = (uint8_t)cl8(bb);
    }

    memcpy(out, gr, (size_t)w*h*3);

    if (lk->hal_amount > 0) {
        int rad = lk->hal_radius, win = 2*rad+1;
        int span = 255 - lk->hal_thresh; if (span < 1) span = 1;
        uint8_t *thr = malloc((size_t)w*h);
        /* threshold, then a horizontal box blur with the edges clamped */
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            int l = luma(gr[(y*w+x)*3], gr[(y*w+x)*3+1], gr[(y*w+x)*3+2]);
            int v = l - lk->hal_thresh;
            thr[y*w+x] = (uint8_t)(v <= 0 ? 0 : (v*255)/span);
        }
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            long s = 0;
            for (k = -rad; k <= rad; k++) s += thr[y*w + clampi(x+k,0,w-1)];
            mask[y*w+x] = (uint8_t)(s/win);
        }
        /* then a vertical box blur, edges clamped, and add the tint */
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            int s = 0, g, a;
            for (k = -rad; k <= rad; k++) s += mask[clampi(y+k,0,h-1)*w + x];
            g = s / win;
            if (g <= 0) continue;
            a = (g * lk->hal_amount) / 100;
            /* Halation only shows bleeding into something darker: where
             * the emulsion is already saturated a second exposure adds
             * nothing. Weight by the headroom left in the destination. */
            a = (a * (255 - luma8(out[(y*w+x)*3+0], out[(y*w+x)*3+1],
                                  out[(y*w+x)*3+2]))) / 255;
            if (a <= 0) continue;
            for (c = 0; c < 3; c++)
                out[(y*w+x)*3+c] =
                    (uint8_t)cl8(out[(y*w+x)*3+c] + ((a*lk->hal_tint[c]) >> 8));
        }
        free(thr);
    }

    if (lk->grain > 0) {
        int amp = (lk->grain*SE_GRAIN_AMP)/100, cell = lk->grain_cell;
        if (amp > 0) for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            int n = (int)(ghash((unsigned)(x/cell),(unsigned)(y/cell),0x5EEDu) & 0xFFFF);
            int d = ((n - 32768)*amp) >> 15;
            for (c = 0; c < 3; c++) {
                int v = out[(y*w+x)*3+c];
                int wt = (v*(255-v)) >> 6;
                out[(y*w+x)*3+c] = (uint8_t)cl8(v + ((d*wt) >> 8));
            }
        }
    }
    free(gr); free(mask);
}

/* ---- collect what the streaming path emits ------------------------- */
typedef struct { uint8_t *buf; int w, rows, cap; } sink;

static int on_row(void *ctx, const uint8_t *row, int w)
{
    sink *s = (sink *)ctx;
    if (s->rows >= s->cap) { fprintf(stderr, "emitted too many rows\n"); exit(1); }
    memcpy(s->buf + (size_t)s->rows*w*3, row, (size_t)w*3);
    s->rows++;
    return 0;
}

static int run_case(const char *what, const se_look *lk,
                    const uint8_t *in, int w, int h, int chunk)
{
    sink     sk;
    se_look_run *run = 0;
    se_arena ar;
    size_t   need = se_look_bytes(lk, w) + 4096;
    void    *mem  = malloc(need);
    uint8_t *ref  = malloc((size_t)w*h*3);
    int      y, bad = -1, i, nbad = 0;

    sk.buf = malloc((size_t)w*h*3); sk.w = w; sk.rows = 0; sk.cap = h;
    se_arena_init(&ar, mem, need);
    if (se_look_begin(&run, lk, w, &ar) != SE_OK) { printf("  %-34s begin FAILED\n", what); return 1; }

    for (y = 0; y < h; y += chunk) {
        int n = (y + chunk <= h) ? chunk : (h - y);
        if (se_look_rows(run, in + (size_t)y*w*3, n, on_row, &sk) != SE_OK) {
            printf("  %-34s rows FAILED\n", what); return 1;
        }
    }
    if (se_look_end(run, on_row, &sk) != SE_OK) { printf("  %-34s end FAILED\n", what); return 1; }

    if (sk.rows != h) {
        printf("  %-34s emitted %d rows, expected %d\n", what, sk.rows, h);
        return 1;
    }
    reference(lk, in, w, h, ref);
    for (i = 0; i < w*h*3; i++)
        if (sk.buf[i] != ref[i]) { if (bad < 0) bad = i; nbad++; }
    if (nbad) {
        printf("  %-34s %d/%d bytes differ, first at row %d col %d chan %d "
               "(got %d, want %d)\n", what, nbad, w*h*3,
               bad/(w*3), (bad%(w*3))/3, bad%3, sk.buf[bad], ref[bad]);
        return 1;
    }
    printf("  %-34s ok (%d rows, chunk %d)\n", what, h, chunk);
    free(mem); free(ref); free(sk.buf);
    return 0;
}

int main(void)
{
    int      w = 61, h = 37, x, y, fails = 0, ci;
    uint8_t *in = malloc((size_t)w*h*3);
    int      chunks[] = { 1, 2, 3, 7, 16, 37, 64 };
    se_look  lk;

    /* a gradient with a hard bright spot: the spot is what halation acts on,
     * and putting it near an edge exercises the clamping */
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        int bright = (x > 6 && x < 14 && y > 2 && y < 9) ||
                     (x > w-6 && y > h-5);
        in[(y*w+x)*3+0] = (uint8_t)(bright ? 255 : (x*4) & 0xFF);
        in[(y*w+x)*3+1] = (uint8_t)(bright ? 250 : (y*6) & 0xFF);
        in[(y*w+x)*3+2] = (uint8_t)(bright ? 240 : ((x+y)*3) & 0xFF);
    }

    printf("lookcheck: streaming look vs a whole-image reference\n");

    for (ci = 0; ci < (int)(sizeof chunks/sizeof *chunks); ci++) {
        int c = chunks[ci];
        char nm[64];

        /* curves only: the flat path, no delay line at all */
        memset(&lk, 0, sizeof lk);
        for (x = 0; x < 256; x++) { lk.map[0][x]=lk.map[1][x]=lk.map[2][x]=(uint8_t)x; }
        lk.sat = 256;
        { int k; for (k=0;k<6;k++) { lk.hue_sat[k]=256; lk.hue_lum[k]=256;
                                     lk.hue_rot[k]=0; } }
        snprintf(nm, sizeof nm, "flat (no halation)"); 
        fails += run_case(nm, &lk, in, w, h, c);

        /* per-hue saturation on its own: the reciprocal table against a
         * real divide, which is where an off-by-one would hide */
        { int k; for (k=0;k<6;k++) lk.hue_sat[k]=256; }
        { int k2; for (k2=0;k2<6;k2++) lk.hue_lum[k2]=256; }
        lk.hue_sat[0]=330; lk.hue_sat[1]=120; lk.hue_sat[2]=64;
        lk.hue_sat[3]=200; lk.hue_sat[4]=288; lk.hue_sat[5]=176;
        lk.hue_any = 1;
        snprintf(nm, sizeof nm, "per-hue saturation");
        fails += run_case(nm, &lk, in, w, h, c);

        /* rotation: both signs, and one past a sector boundary */
        lk.hue_rot[0]=40; lk.hue_rot[1]=-64; lk.hue_rot[2]=128;
        lk.hue_rot[3]=-200; lk.hue_rot[4]=300; lk.hue_rot[5]=-12;
        snprintf(nm, sizeof nm, "per-hue rotation");
        fails += run_case(nm, &lk, in, w, h, c);

        /* and per-hue luminance on top */
        lk.hue_lum[0]=300; lk.hue_lum[1]=200; lk.hue_lum[2]=180;
        lk.hue_lum[3]=224; lk.hue_lum[4]=160; lk.hue_lum[5]=288;
        snprintf(nm, sizeof nm, "per-hue rotation + luminance");
        fails += run_case(nm, &lk, in, w, h, c);

        /* saturation + split tone, still no delay line */
        lk.sat = 150; lk.sh[0]=-8; lk.sh[2]=14; lk.hi[0]=12; lk.hi[2]=-9;
        snprintf(nm, sizeof nm, "sat + split tone");
        fails += run_case(nm, &lk, in, w, h, c);

        /* halation: radius 1 (the degenerate ring), 5, and larger than the
         * image, which is where a window that is not clamped falls apart */
        lk.hal_amount = 60; lk.hal_thresh = 180;
        lk.hal_tint[0]=255; lk.hal_tint[1]=64; lk.hal_tint[2]=24;
        lk.hal_radius = 1;
        snprintf(nm, sizeof nm, "halation r=1");
        fails += run_case(nm, &lk, in, w, h, c);

        lk.hal_radius = 5;
        snprintf(nm, sizeof nm, "halation r=5");
        fails += run_case(nm, &lk, in, w, h, c);

        lk.hal_radius = 48;
        snprintf(nm, sizeof nm, "halation r=48 (> image height)");
        fails += run_case(nm, &lk, in, w, h, c);

        /* and with grain on top, which is applied after the glow */
        lk.hal_radius = 6; lk.grain = 40; lk.grain_cell = 2;
        snprintf(nm, sizeof nm, "halation + grain");
        fails += run_case(nm, &lk, in, w, h, c);
    }

    printf(fails ? "\nFAILED (%d cases)\n" : "\nall cases agree\n", fails);
    return fails ? 1 : 0;
}
