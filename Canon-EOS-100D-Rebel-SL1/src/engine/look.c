#include "look.h"
#include "se_libc.h"

/* ------------------------------------------------------------ helpers */

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* BT.601 luma, the same weights the palette conversion uses. Integer, so
 * the camera and the desktop cannot disagree. */
static int luma8(int r, int g, int b)
{
    return (77 * r + 150 * g + 29 * b) >> 8;
}

/*
 * Film grain.
 *
 * A hash of the pixel's own coordinates: repeatable (the same frame always
 * grains the same way, so the desktop and the camera produce identical
 * bytes), needs no state, and costs a handful of integer ops. Amplitude is
 * weighted by 4v(1-v) because grain is a modulation of exposed silver --
 * it vanishes in clipped whites and in blacks where there is nothing to
 * modulate, which is what stops it looking like television noise.
 *
 * This is the same construction pipeline.c uses on the raw path, kept
 * deliberately identical so a stock grains the same from either source.
 */
/*
 * Exact division by a small variable, as a multiply.
 *
 * ARM946E-S has no divide instruction, so `n / d` with d not known at
 * compile time is a library call of some forty cycles -- and the halation
 * mask, the glow and the grain each had one or two per pixel. With
 * m = ceil(2^24 / d), (n * m) >> 24 equals n / d exactly for every
 * 0 <= n < 2^16 and 1 <= d < 256 (the error of m is below d, so n times
 * it stays below 2^24), which covers every use below. The product needs
 * 64 bits, which is one umull. Exact, so the pixels are the same bytes
 * they were, and tools/previewcheck's hashes say so.
 */
static uint32_t se_recip24(int d)
{
    if (d < 1) d = 1;
    return (uint32_t)((16777216u + (uint32_t)d - 1) / (uint32_t)d);
}
#define SE_DIV24(n, m) ((int)(((uint64_t)(uint32_t)(n) * (m)) >> 24))

static unsigned grain_hash(unsigned x, unsigned y, unsigned seed)
{
    unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/*
 * 65536 / d, for the hue calculation.
 *
 * Finding a pixel's hue needs a divide by its chroma, and ARM946E-S has
 * no divide instruction -- the compiler would emit a call to
 * __aeabi_idiv for every pixel. Chroma is a byte difference, so 255
 * reciprocals cover every case and the per-pixel cost becomes one
 * multiply.
 */
static uint32_t s_recip[256];

/*
 * sin, as 8.8 fixed point, over a turn divided into 1536 -- the same
 * units hue is measured in, so a rotation indexes straight into it.
 *
 * Built here rather than taken from libm because this has to produce
 * byte-identical results on a camera with no libm at all, and because a
 * table built two different ways is a table that disagrees in the last
 * place. The Taylor series below IS the specification: tools/lookcheck.c
 * rebuilds it the same way, deliberately.
 */
static int16_t s_sin[1536];

/*
 * How much chroma survives at each luminance, 256 = all of it.
 *
 * This is the single most film-like thing in the file, and the most
 * obviously absent one. Real emulsion loses colour toward both ends: at
 * the top all three dye layers run out together, so a bright colour
 * desaturates on its way to white, and at the bottom there is too little
 * exposure left to hold a hue. Digital saturation does neither -- it
 * keeps a colour at full strength right up to clipping, which is exactly
 * what makes an oversaturated frame read as a slider rather than a film.
 *
 * Measured on a frame through VELVIA: foliage at luma 168 went from
 * chroma 47 to 100, a 2.1x boost held at full strength into the
 * highlights, and came back looking like neon.
 *
 * Flat through the midtones, where film is at its most colourful, and
 * rolling off above and below.
 */
static uint8_t s_chroma[256];

static void tables_init(void)
{
    int i;
    if (s_recip[255]) return;                 /* already built */
    for (i = 1; i < 256; i++) s_recip[i] = (uint32_t)(65536 / i);

    for (i = 0; i < 256; i++) {
        int w;
        if (i < 24)       w = 150 + (i * 106) / 24;        /* 150 -> 256  */
        else if (i <= 112) w = 256;                         /* the midtones */
        else              w = 256 - ((i - 112) * 140) / 143; /* 256 -> 116 */
        s_chroma[i] = (uint8_t)(w < 0 ? 0 : (w > 255 ? 255 : w));
    }

    for (i = 0; i < 1536; i++) {
        /* fold into the first quarter turn, then Taylor to x^7 */
        int   q = i & 1535, sign = 1;
        float x, x2, v;
        if (q >= 768) { q -= 768; sign = -1; }
        if (q > 384) q = 768 - q;
        x  = (float)q * (3.14159265f / 768.0f);
        x2 = x * x;
        v  = x * (1.0f - x2 * (1.0f / 6.0f)
                       * (1.0f - x2 * (1.0f / 20.0f)
                                    * (1.0f - x2 * (1.0f / 42.0f))));
        v *= (float)sign * 256.0f;
        s_sin[i] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }
}

#define SE_SIN(a) (s_sin[(((a) % 1536) + 1536) % 1536])
#define SE_COS(a) SE_SIN((a) + 384)

/*
 * Which hue a pixel is, as 0..1535 -- 256 per sector, R Y G C B M.
 * Returns -1 for a pixel with no colour in it at all.
 */
static int hue1536(int r, int g, int b, int *chroma)
{
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d  = mx - mn, h;

    *chroma = d;
    if (d <= 0) return -1;
    if (mx == r)      h = ((g - b) * (int)s_recip[d]) >> 8;
    else if (mx == g) h = (((b - r) * (int)s_recip[d]) >> 8) + 512;
    else              h = (((r - g) * (int)s_recip[d]) >> 8) + 1024;
    if (h < 0)     h += 1536;
    if (h >= 1536) h -= 1536;
    return h;
}

/*
 * 256 * num / den without a divide, for den up to about 1020.
 * s_recip only covers 1..255, so halve both until it fits -- at most two
 * shifts, and the precision lost is far below one output level.
 */
static int fit_scale(int num, int den)
{
    while (den > 255) { den >>= 1; num >>= 1; }
    if (den <= 0) return 256;
    return (num * (int)s_recip[den]) >> 8;
}

/*
 * Fit a colour back inside 0..255 by pulling its CHROMA in, not by
 * clipping its channels.
 *
 * This is the difference between a film look and a saturation slider.
 * Clipping takes a channel to 0 or 255 and leaves the others where they
 * are, which produces the purest, most electric version of that hue with
 * every trace of detail gone -- flat neon foliage, flat neon sky. Film
 * cannot do it: the three dye layers overlap, so a density of zero in one
 * layer while the others hold is not a state the material has.
 *
 * Measured on one frame through VELVIA: 57.5% of pixels had a channel at
 * the floor, against 27.8% with no look at all, and the greens came back
 * as a single flat colour.
 *
 * So find the largest scale that keeps every channel in range and apply
 * it to all three. Hue and luminance survive; only the saturation gives,
 * and only by as much as it must.
 */
static void gamut_fit(int *rr, int *gg, int *bb)
{
    int r = *rr, g = *gg, b = *bb;
    int y, sc = 256, d;

    if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255)
        return;                       /* already fits: the common case */

    y = (77 * r + 150 * g + 29 * b) >> 8;
    if (y < 0)   y = 0;
    if (y > 255) y = 255;

    d = r - y;
    if (r > 255 && d > 0) { int t = fit_scale(255 - y, d);  if (t < sc) sc = t; }
    if (r < 0   && d < 0) { int t = fit_scale(y, -d);       if (t < sc) sc = t; }
    d = g - y;
    if (g > 255 && d > 0) { int t = fit_scale(255 - y, d);  if (t < sc) sc = t; }
    if (g < 0   && d < 0) { int t = fit_scale(y, -d);       if (t < sc) sc = t; }
    d = b - y;
    if (b > 255 && d > 0) { int t = fit_scale(255 - y, d);  if (t < sc) sc = t; }
    if (b < 0   && d < 0) { int t = fit_scale(y, -d);       if (t < sc) sc = t; }

    if (sc < 0)   sc = 0;
    if (sc > 256) sc = 256;
    *rr = y + (((r - y) * sc) >> 8);
    *gg = y + (((g - y) * sc) >> 8);
    *bb = y + (((b - y) * sc) >> 8);
}

/* ------------------------------------------------- stock -> look ----- */

void se_look_from_stock(const se_stock *s, int scale, se_look *lk)
{
    int c;

    memset(lk, 0, sizeof *lk);
    tables_init();
    se_stock_bake_srgb8(s, lk->map);

    for (c = 0; c < 6; c++) {
        lk->hue_sat[c] = (int)(s->hue_sat[c] * 2.56f + 0.5f);
        if (lk->hue_sat[c] < 0)    lk->hue_sat[c] = 0;
        if (lk->hue_sat[c] > 1024) lk->hue_sat[c] = 1024;

        lk->hue_lum[c] = (int)(s->hue_lum[c] * 2.56f + 0.5f);
        if (lk->hue_lum[c] < 0)    lk->hue_lum[c] = 0;
        if (lk->hue_lum[c] > 1024) lk->hue_lum[c] = 1024;

        /* authored in degrees; 1536 units to the turn */
        lk->hue_rot[c] = (int)(s->hue_rot[c] * (1536.0f / 360.0f)
                               + (s->hue_rot[c] < 0 ? -0.5f : 0.5f));
        if (lk->hue_rot[c] < -512) lk->hue_rot[c] = -512;
        if (lk->hue_rot[c] >  512) lk->hue_rot[c] =  512;

        if (lk->hue_sat[c] != 256 || lk->hue_lum[c] != 256 || lk->hue_rot[c])
            lk->hue_any = 1;
    }

    lk->sat = s->sat_set ? (int)(s->sat * 2.56f + 0.5f) : 256;
    if (lk->sat < 0)    lk->sat = 0;
    if (lk->sat > 1024) lk->sat = 1024;

    for (c = 0; c < 3; c++) {
        lk->sh[c] = (int)s->split_sh[c];
        lk->hi[c] = (int)s->split_hi[c];
    }

    lk->grain = (int)(s->grain + 0.5f);
    if (lk->grain < 0)   lk->grain = 0;
    if (lk->grain > 100) lk->grain = 100;
    /*
     * size 0..100 -> a 1..9 pixel cell at full size, shrinking with the
     * decode scale so a half-size frame is not twice as grainy.
     *
     * The divisor was 25, giving five steps, and then the half-size divide
     * collapsed four of them onto 1: every stock from Velvia's fine grain
     * to a coarse 800-speed stock's clumps came out with a one-pixel
     * cell on the path the camera actually uses. Nine steps survive the
     * halving with a real spread -- 1 at the fine end, 4 or 5 at the
     * coarse end.
     */
    lk->grain_cell = 1 + (int)(s->grain_size / 12.0f);
    if (scale > 1) lk->grain_cell = (lk->grain_cell + scale - 1) / scale;
    if (lk->grain_cell < 1) lk->grain_cell = 1;

    /*
     * HALATION above 100 is a gain, and it has to be allowed.
     *
     * The glow is a box blur, so what reaches add_glow is the MEAN of the
     * mask over a (2r+1)^2 window, not its sum. A street light occupying
     * a hundred pixels inside a 53x53 window averages down to almost
     * nothing, and with the old cap of 100 there was no way to ask for it
     * back: the halo could never be brighter than the window mean. The
     * practical effect was that widening the radius made the bloom fainter
     * rather than larger, which is the opposite of what the control reads
     * as and the reason the halation stock's glow would not bloom however it was
     * tuned.
     *
     * 400 lets a stock say "this scatters hard" and still clip sanely --
     * add_glow runs every channel through clamp8. The reference in
     * tools/lookcheck.c takes hal_amount straight from the look, so it
     * needs no matching change; this converts a stock, and nothing else.
     */
    lk->hal_amount = (int)(s->hal_amount + 0.5f);
    if (lk->hal_amount < 0)   lk->hal_amount = 0;
    if (lk->hal_amount > 400) lk->hal_amount = 400;
    lk->hal_thresh = (int)(s->hal_thresh + 0.5f);
    if (lk->hal_thresh < 0)   lk->hal_thresh = 0;
    if (lk->hal_thresh > 255) lk->hal_thresh = 255;
    lk->hal_radius = (int)(s->hal_radius + 0.5f) / (scale > 0 ? scale : 1);
    if (lk->hal_radius < 1)  lk->hal_radius = 1;
    if (lk->hal_radius > 48) lk->hal_radius = 48;
    for (c = 0; c < 3; c++) lk->hal_tint[c] = clamp8((int)s->hal_tint[c]);
}

/* ------------------------------------------------------- the run ----- */

struct se_look_run {
    se_look  lk;
    int      w;
    int      y_out;           /* rows emitted                            */

    int      on;              /* halation enabled: a delay line is live  */
    int      r;               /* halation radius, in rows                */
    int      nring;           /* 2r + 1: the mask window                 */
    int      ndelay;          /* r + 1: see se_look_begin                 */
    uint8_t *ring;            /* nring rows of horizontally blurred mask */
    uint8_t *delay;           /* ndelay rows of graded RGB               */
    int     *colsum;          /* w running column sums over the ring     */
    int      pushed;          /* mask rows pushed, priming included      */
    uint8_t *scratch;         /* one row of mask, pre-blur               */

    uint8_t *flat;            /* !on only: the one row we grade into     */
};

size_t se_look_bytes(const se_look *lk, int width)
{
    size_t n;
    if (lk->hal_amount <= 0)
        return sizeof(struct se_look_run) + (size_t)width * 3 + 32;
    n  = sizeof(struct se_look_run) + 16;
    n += (size_t)(2 * lk->hal_radius + 1) * (size_t)width;          /* ring   */
    n += (size_t)(lk->hal_radius + 1) * (size_t)width * 3;          /* delay  */
    n += (size_t)width * sizeof(int);                               /* colsum */
    n += (size_t)width;                                             /* scratch*/
    return n + 64;
}

size_t se_look_fit(se_look *lk, int width, size_t budget)
{
    size_t n = se_look_bytes(lk, width);

    while (lk->hal_amount > 0 && lk->hal_radius > 1 && n > budget) {
        lk->hal_radius--;
        n = se_look_bytes(lk, width);
    }
    if (n > budget && lk->hal_amount > 0) {
        /* A radius of 1 is not a glow anyway, and with amount 0 the run
         * allocates no delay line at all -- so this always terminates at
         * something that fits. */
        lk->hal_amount = 0;
        n = se_look_bytes(lk, width);
    }
    return n;
}

se_err se_look_begin(se_look_run **out, const se_look *lk,
                     int width, se_arena *ar)
{
    se_look_run *r;

    if (!out || !lk || width <= 0 || !ar) return SE_E_CORRUPT;

    /*
     * Also here, not only in se_look_from_stock.
     *
     * A caller may fill se_look itself rather than resolving it from a
     * stock -- tools/lookcheck.c does exactly that -- and with the table
     * still zero every pixel reports hue 0 and the whole per-hue stage
     * silently collapses onto the red sector. It cost an afternoon
     * looking for an off-by-one in the reciprocal. Every consumer has to
     * come through here, so this is where it belongs.
     */
    tables_init();

    r = SE_NEW(ar, se_look_run, 1);
    if (!r) return SE_E_NOMEM;
    memset(r, 0, sizeof *r);
    r->lk = *lk;
    r->w  = width;

    if (lk->hal_amount > 0) {
        r->r      = lk->hal_radius;
        r->nring  = 2 * r->r + 1;
        /*
         * The mask window spans 2r+1 rows, but the graded RGB does not.
         * A row is written at push index i and read back at push index
         * i+r, so it only has to survive r+1 pushes -- it is overwritten
         * at i+r+1, one push after it was needed. Sizing this ring like
         * the mask ring held r extra rows of RGB for nothing: 120 KB at
         * full width and radius 16, which is the difference between the
         * full-size develop getting radius 8 and radius 13.
         */
        r->ndelay = r->r + 1;
        r->ring    = SE_NEW(ar, uint8_t, (size_t)r->nring * width);
        r->delay   = SE_NEW(ar, uint8_t, (size_t)r->ndelay * width * 3);
        r->colsum  = SE_NEW(ar, int, width);
        r->scratch = SE_NEW(ar, uint8_t, width);
        if (!r->ring || !r->delay || !r->colsum || !r->scratch)
            return SE_E_NOMEM;
        memset(r->ring,   0, (size_t)r->nring * width);
        memset(r->colsum, 0, (size_t)width * sizeof(int));
        r->on = 1;
    } else {
        r->flat = SE_NEW(ar, uint8_t, (size_t)width * 3);
        if (!r->flat) return SE_E_NOMEM;
    }
    *out = r;
    return SE_OK;
}

/* Curves, saturation and split tone: everything that needs only the pixel
 * itself. Writes in place. */
static void grade_row(const se_look *lk, uint8_t *row, int w)
{
    int x;
    for (x = 0; x < w; x++) {
        int rr = lk->map[0][row[x * 3 + 0]];
        int gg = lk->map[1][row[x * 3 + 1]];
        int bb = lk->map[2][row[x * 3 + 2]];

        /*
         * The per-hue stage, on the colour the curves produced.
         *
         * Saturation, rotation and luminance all act on the same two
         * numbers, so this works in luma + chroma rather than in RGB:
         * u and v ARE the colour, rotating them IS a hue shift, scaling
         * them IS saturation, and y on its own IS how bright that hue
         * renders. Doing it in RGB would need three separate passes and
         * could not express a rotation at all.
         *
         * Everything is derived from luma8's own weights, so the inverse
         * below is exact up to luma8's truncation -- which is why the
         * whole block is skipped when a stock asks for nothing per-hue,
         * rather than relying on it round-tripping to the same bytes.
         */
        if (lk->hue_any) {
            int chroma;
            int h = hue1536(rr, gg, bb, &chroma);
            if (h >= 0) {
                int sec = h >> 8, f = h & 255, nx = (sec == 5 ? 0 : sec + 1);
                int msat = lk->hue_sat[sec]
                         + (((lk->hue_sat[nx] - lk->hue_sat[sec]) * f) >> 8);
                int mlum = lk->hue_lum[sec]
                         + (((lk->hue_lum[nx] - lk->hue_lum[sec]) * f) >> 8);
                int mrot = lk->hue_rot[sec]
                         + (((lk->hue_rot[nx] - lk->hue_rot[sec]) * f) >> 8);
                int y = luma8(rr, gg, bb);
                int u = bb - y, v = rr - y;

                /*
                 * Fade the whole stage out as the pixel approaches
                 * neutral. See SE_HUE_FULL: the hue of a near-grey pixel
                 * is meaningless, and acting on it tears flat areas into
                 * blotches.
                 */
                if (chroma < SE_HUE_FULL) {
                    int wt = (chroma * 256) / SE_HUE_FULL;
                    msat = 256 + (((msat - 256) * wt) >> 8);
                    mlum = 256 + (((mlum - 256) * wt) >> 8);
                    mrot = (mrot * wt) >> 8;
                }

                if (mrot) {
                    int cs = SE_COS(mrot), sn = SE_SIN(mrot);
                    int u2 = (u * cs - v * sn) >> 8;
                    int v2 = (u * sn + v * cs) >> 8;
                    u = u2; v = v2;
                }
                if (msat != 256) {
                    u = (u * msat) >> 8;
                    v = (v * msat) >> 8;
                }
                if (mlum != 256) y = (y * mlum) >> 8;

                rr = y + v;
                bb = y + u;
                gg = y - ((77 * v + 29 * u) / 150);
            }
        }

        if (lk->sat != 256) {
            int y = luma8(clamp8(rr), clamp8(gg), clamp8(bb));
            rr = y + (((rr - y) * lk->sat) >> 8);
            gg = y + (((gg - y) * lk->sat) >> 8);
            bb = y + (((bb - y) * lk->sat) >> 8);
        }

        if (lk->sh[0] | lk->sh[1] | lk->sh[2] |
            lk->hi[0] | lk->hi[1] | lk->hi[2]) {
            int y  = luma8(clamp8(rr), clamp8(gg), clamp8(bb));
            int wl = 255 - y;                  /* shadow weight    */
            rr += (lk->sh[0] * wl + lk->hi[0] * y) >> 8;
            gg += (lk->sh[1] * wl + lk->hi[1] * y) >> 8;
            bb += (lk->sh[2] * wl + lk->hi[2] * y) >> 8;
        }

        /*
         * Film's own chroma rolloff, applied once to the finished
         * colour -- see s_chroma. Before the gamut fit, because it
         * removes most of what would otherwise need fitting.
         */
        {
            int y = (77 * rr + 150 * gg + 29 * bb) >> 8;
            int w;
            if (y < 0)   y = 0;
            if (y > 255) y = 255;
            w = s_chroma[y];
            if (w != 256) {
                rr = y + (((rr - y) * w) >> 8);
                gg = y + (((gg - y) * w) >> 8);
                bb = y + (((bb - y) * w) >> 8);
            }
        }

        gamut_fit(&rr, &gg, &bb);
        row[x * 3 + 0] = (uint8_t)clamp8(rr);
        row[x * 3 + 1] = (uint8_t)clamp8(gg);
        row[x * 3 + 2] = (uint8_t)clamp8(bb);
    }
}

/*
 * The highlight mask for one graded row, blurred horizontally.
 *
 * A running sum, so the cost per pixel does not depend on the radius --
 * one add and one subtract however wide the glow is. The mask is how far
 * the pixel is above the threshold, scaled so that a pixel at 255 is 255:
 * a soft knee would be prettier, but this is what a threshold means and it
 * is one comparison.
 */
static void mask_row(const se_look *lk, const uint8_t *row, int w,
                     uint8_t *scratch, uint8_t *dst)
{
    int x, rad = lk->hal_radius, win = 2 * rad + 1;
    long sum = 0;
    int  span = 255 - lk->hal_thresh;
    if (span < 1) span = 1;
    {
    uint32_t rspan = se_recip24(span), rwin = se_recip24(win);

    for (x = 0; x < w; x++) {
        int y = luma8(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
        int v = y - lk->hal_thresh;
        scratch[x] = (uint8_t)(v <= 0 ? 0 : SE_DIV24(v * 255, rspan));
    }

    /* prime the window with the left edge clamped */
    for (x = -rad; x <= rad; x++)
        sum += scratch[x < 0 ? 0 : (x >= w ? w - 1 : x)];
    for (x = 0; x < w; x++) {
        int add = x + rad + 1, sub = x - rad;
        dst[x] = (uint8_t)SE_DIV24(sum, rwin);
        sum += scratch[add >= w ? w - 1 : add];
        sum -= scratch[sub < 0 ? 0 : sub];
    }
    }
}

/* Add the glow to one row, given its vertical column sums. */
static void add_glow(const se_look *lk, uint8_t *row, int w,
                     const int *colsum, int nrows)
{
    int x;
    if (nrows < 1) nrows = 1;
    uint32_t rrows = se_recip24(nrows);
    for (x = 0; x < w; x++) {
        int g = SE_DIV24(colsum[x], rrows);     /* 0..255 */
        int a, room;
        if (g <= 0) continue;
        a = (g * lk->hal_amount) / 100;

        /*
         * Fade the glow out where the frame is already bright.
         *
         * Halation is light that scattered off the base and exposed the
         * emulsion a second time. Where the emulsion is already near
         * saturation that second exposure changes nothing, so the halo is
         * only ever visible bleeding into something darker.
         *
         * Adding it everywhere is what made this unusable in daylight: a
         * white building lit the whole mask, the glow landed on top of
         * the building as well as beside it, and the frame came out
         * outlined in red and washed pink. Weighting by the headroom left
         * in the destination fixes both ends at once -- a street light
         * against a dark wall still blooms hard, and a white wall in
         * sunlight gets almost nothing, which is what film does.
         */
        room = 255 - luma8(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
        a = (a * room) / 255;
        if (a <= 0) continue;

        row[x * 3 + 0] = (uint8_t)clamp8(row[x * 3 + 0] + ((a * lk->hal_tint[0]) >> 8));
        row[x * 3 + 1] = (uint8_t)clamp8(row[x * 3 + 1] + ((a * lk->hal_tint[1]) >> 8));
        row[x * 3 + 2] = (uint8_t)clamp8(row[x * 3 + 2] + ((a * lk->hal_tint[2]) >> 8));
    }
}

static void grain_row(const se_look *lk, uint8_t *row, int w, int y)
{
    int x, cell = lk->grain_cell;
    /* 0..100 -> about +-15 at full strength. This was 72/100, which put
     * +-28 on a midtone: at that amplitude a coarse stock stopped reading
     * as emulsion and started reading as sensor noise, which is the one
     * thing grain must not look like. */
    int amp = (lk->grain * SE_GRAIN_AMP) / 100;
    unsigned yc = (unsigned)(y / cell), xc = 0;
    int cnt = 0;
    if (amp <= 0) return;
    for (x = 0; x < w; x++) {
        int n = (int)(grain_hash(xc, yc, 0x5EEDu) & 0xFFFF);
        if (++cnt == cell) { cnt = 0; xc++; }   /* x / cell, without dividing */
        int d = ((n - 32768) * amp) >> 15;
        int c;
        for (c = 0; c < 3; c++) {
            int v = row[x * 3 + c];
            /* 4v(1-v), in eighths, so grain fades out at both ends */
            int wgt = (v * (255 - v)) >> 6;     /* 0..254 */
            row[x * 3 + c] = (uint8_t)clamp8(v + ((d * wgt) >> 8));
        }
    }
}

/* One row, fully graded and already in `dst`, ready to go out. */
static se_err emit_one(se_look_run *r, uint8_t *dst,
                       se_look_emit emit, void *ctx)
{
    if (r->lk.grain > 0) grain_row(&r->lk, dst, r->w, r->y_out);
    r->y_out++;
    return emit(ctx, dst, r->w) == 0 ? SE_OK : SE_E_IO;
}

/*
 * Push one source row into the delay line, and emit whatever row that
 * completes.
 *
 * THE INDEXING, because it is the one thing here that is easy to get wrong
 * and impossible to see wrong in a photograph:
 *
 *   The ring is primed with `r` extra copies of row 0, so source row j
 *   lands at push index j + r. After push index p, the ring holds push
 *   indices p-2r .. p, which is exactly the vertical window centred on
 *   output row p - 2r. That row's RGB is at push index p - r, still in the
 *   ring because nring = 2r+1 > r.
 *
 *   Top and bottom edges are handled by replication -- the priming copies
 *   at the top, and `r` more copies of the last mask row in se_look_end at
 *   the bottom. Clamping the window instead would need a per-row divisor
 *   and would still be a guess about what is off the edge of the frame.
 *
 *   Total pushes are H + 2r and emits start at p = 2r, so exactly H rows
 *   come out however short the image is -- including H < r, where nothing
 *   at all is emitted until the flush.
 */
static se_err emit_delayed(se_look_run *r, int p,
                           se_look_emit emit, void *ctx);

static se_err push_row(se_look_run *r, const uint8_t *src,
                       se_look_emit emit, void *ctx)
{
    int      w    = r->w;
    int      slot = r->pushed % r->nring;
    int      dslot = r->pushed % r->ndelay;
    uint8_t *dly  = r->delay + (size_t)dslot * w * 3;
    uint8_t *msk  = r->ring  + (size_t)slot * w;
    int      x, p;

    /* The row leaving the window stops contributing to the column sums.
     * Only once the ring has been round at least once, or we would be
     * subtracting the zeros se_look_begin put there. */
    if (r->pushed >= r->nring)
        for (x = 0; x < w; x++) r->colsum[x] -= msk[x];

    memcpy(dly, src, (size_t)w * 3);
    grade_row(&r->lk, dly, w);
    mask_row(&r->lk, dly, w, r->scratch, msk);

    for (x = 0; x < w; x++) r->colsum[x] += msk[x];

    p = r->pushed++;
    if (p < 2 * r->r) return SE_OK;          /* window not centred yet */
    return emit_delayed(r, p, emit, ctx);
}

/* Emit the output row whose window has just closed at push index p. */
static se_err emit_delayed(se_look_run *r, int p,
                           se_look_emit emit, void *ctx)
{
    int      slot = (p - r->r) % r->ndelay;
    uint8_t *dly  = r->delay + (size_t)slot * r->w * 3;

    add_glow(&r->lk, dly, r->w, r->colsum, r->nring);
    return emit_one(r, dly, emit, ctx);
}

se_err se_look_rows(se_look_run *r, const uint8_t *rgb, int nrows,
                    se_look_emit emit, void *ctx)
{
    int    i;
    se_err e;

    if (!r || !rgb || nrows < 0 || !emit) return SE_E_CORRUPT;

    for (i = 0; i < nrows; i++) {
        const uint8_t *src = rgb + (size_t)i * r->w * 3;

        if (!r->on) {
            /* No halation, so no neighbourhood and no delay: grade and go.
             * The caller's rows are const and may be the decoder's own
             * scratch, so the grade lands in our row, never in theirs. */
            memcpy(r->flat, src, (size_t)r->w * 3);
            grade_row(&r->lk, r->flat, r->w);
            e = emit_one(r, r->flat, emit, ctx);
            if (e != SE_OK) return e;
            continue;
        }

        /* First row of the frame: prime the top edge with r copies of it,
         * so the window is full from the first emit onwards. */
        if (r->pushed == 0) {
            int k;
            for (k = 0; k < r->r; k++) {
                e = push_row(r, src, emit, ctx);
                if (e != SE_OK) return e;
            }
        }

        e = push_row(r, src, emit, ctx);
        if (e != SE_OK) return e;
    }
    return SE_OK;
}

se_err se_look_end(se_look_run *r, se_look_emit emit, void *ctx)
{
    int    k, w, last;
    se_err e;

    if (!r || !emit) return SE_E_CORRUPT;
    if (!r->on || r->pushed == 0) return SE_OK;

    w = r->w;

    /* The bottom edge, replicated: copy the last mask row down `r` times.
     * The mask is copied rather than re-derived because the graded RGB it
     * came from has since had glow and grain written into it -- masking
     * that again would feed the glow back through itself. */
    for (k = 0; k < r->r; k++) {
        int slot, x;
        last = (r->pushed - 1) % r->nring;
        slot = r->pushed % r->nring;

        if (r->pushed >= r->nring)
            for (x = 0; x < w; x++) r->colsum[x] -= r->ring[(size_t)slot * w + x];
        memcpy(r->ring + (size_t)slot * w,
                  r->ring + (size_t)last * w, (size_t)w);
        for (x = 0; x < w; x++) r->colsum[x] += r->ring[(size_t)slot * w + x];

        r->pushed++;
        if (r->pushed - 1 < 2 * r->r) continue;
        e = emit_delayed(r, r->pushed - 1, emit, ctx);
        if (e != SE_OK) return e;
    }
    return SE_OK;
}
