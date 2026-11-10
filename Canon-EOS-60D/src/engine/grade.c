#include "grade.h"
#include "se_libc.h"

/* ------------------------------------------------------- small helpers */

static float se_powf_g(float x, float e)
{
    float lx, y, t, acc;
    int   n = 0, i;
    if (x <= 0.0f) return 0.0f;
    while (x < 1.0f)  { x *= 2.0f; n--; }
    while (x >= 2.0f) { x *= 0.5f; n++; }
    t = (x - 1.0f) / (x + 1.0f);
    acc = 0.0f;
    { float tp = t; for (i = 1; i <= 15; i += 2) { acc += tp / (float)i; tp *= t * t; } }
    lx = (float)n + acc * 2.0f * 1.44269504f;
    y = lx * e;
    { int   m = (int)(y < 0 ? y - 0.5f : y + 0.5f);
      float f = y - (float)m, r = 1.0f, p = 1.0f;
      for (i = 1; i <= 8; i++) { p *= f * 0.69314718f / (float)i; r += p; }
      while (m > 0) { r *= 2.0f; m--; }
      while (m < 0) { r *= 0.5f; m++; }
      return r; }
}

static float srgb_enc(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x <= 0.0031308f ? x * 12.92f
                           : 1.055f * se_powf_g(x, 1.0f / 2.4f) - 0.055f;
}

static float srgb_dec(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x <= 0.04045f ? x / 12.92f
                         : se_powf_g((x + 0.055f) / 1.055f, 2.4f);
}

void se_grade_init(se_grade *g)
{
    int i;
    memset(g, 0, sizeof *g);
    for (i = 0; i < SE_HSL_BANDS; i++) g->mono_mix[i] = 0.0f;
}

/* ------------------------------------------------------------- curves */
/*
 * Monotone cubic Hermite (Fritsch-Carlson). Lightroom's point curve is a
 * spline through the control points; using a plain Catmull-Rom here would
 * overshoot and invert the curve between close points, which shows up as
 * banding. Monotonicity is the property that matters.
 */
float se_curve_eval(const se_curve *c, float x)
{
    float xs[SE_CURVE_MAX_PTS], ys[SE_CURVE_MAX_PTS];
    float d[SE_CURVE_MAX_PTS], m[SE_CURVE_MAX_PTS];
    int   n = c->n, i, k;
    float h, t, t2, t3, h00, h10, h01, h11;

    if (n < 2) return x;
    if (n > SE_CURVE_MAX_PTS) n = SE_CURVE_MAX_PTS;

    for (i = 0; i < n; i++) {
        xs[i] = (float)c->x[i] / 255.0f;
        ys[i] = (float)c->y[i] / 255.0f;
    }

    if (x <= xs[0])     return ys[0];
    if (x >= xs[n - 1]) return ys[n - 1];

    for (i = 0; i < n - 1; i++) {
        float dx = xs[i + 1] - xs[i];
        d[i] = dx > 1e-6f ? (ys[i + 1] - ys[i]) / dx : 0.0f;
    }

    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (i = 1; i < n - 1; i++) {
        if (d[i - 1] * d[i] <= 0.0f) m[i] = 0.0f;
        else                         m[i] = (d[i - 1] + d[i]) * 0.5f;
    }
    /* clamp the tangents so no segment can overshoot */
    for (i = 0; i < n - 1; i++) {
        if (d[i] == 0.0f) { m[i] = m[i + 1] = 0.0f; continue; }
        { float a = m[i] / d[i], b = m[i + 1] / d[i];
          float s = a * a + b * b;
          if (s > 9.0f) {
              float f = 3.0f / se_powf_g(s, 0.5f);
              m[i]     = f * a * d[i];
              m[i + 1] = f * b * d[i];
          } }
    }

    k = 0;
    for (i = 0; i < n - 1; i++) if (x >= xs[i] && x <= xs[i + 1]) { k = i; break; }

    h  = xs[k + 1] - xs[k];
    t  = h > 1e-6f ? (x - xs[k]) / h : 0.0f;
    t2 = t * t; t3 = t2 * t;
    h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
    h10 =         t3 - 2.0f * t2 + t;
    h01 = -2.0f * t3 + 3.0f * t2;
    h11 =         t3 -        t2;
    return h00 * ys[k] + h10 * h * m[k] + h01 * ys[k + 1] + h11 * h * m[k + 1];
}

/* ------------------------------------------------------------ HSL ---- */
/*
 * Lightroom's eight bands are not evenly spaced in hue; these are the
 * centres it uses, and a colour is split between its two adjacent bands.
 */
static const float g_band_hue[SE_HSL_BANDS + 1] = {
    0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 300.0f, 360.0f
};

static void band_weights(float hue_deg, float *w)
{
    int i;
    for (i = 0; i < SE_HSL_BANDS; i++) w[i] = 0.0f;
    while (hue_deg < 0.0f)    hue_deg += 360.0f;
    while (hue_deg >= 360.0f) hue_deg -= 360.0f;
    for (i = 0; i < SE_HSL_BANDS; i++) {
        float a = g_band_hue[i], b = g_band_hue[i + 1];
        if (hue_deg >= a && hue_deg <= b) {
            float t = (b - a) > 1e-6f ? (hue_deg - a) / (b - a) : 0.0f;
            w[i] += 1.0f - t;
            w[(i + 1) % SE_HSL_BANDS] += t;   /* magenta wraps to red */
            return;
        }
    }
}

static void rgb_to_hsl(const float *rgb, float *h, float *s, float *l)
{
    float mx = SE_MAX(rgb[0], SE_MAX(rgb[1], rgb[2]));
    float mn = SE_MIN(rgb[0], SE_MIN(rgb[1], rgb[2]));
    float c  = mx - mn;
    *l = (mx + mn) * 0.5f;
    if (c < 1e-6f) { *h = 0.0f; *s = 0.0f; return; }
    *s = (*l > 0.5f) ? c / (2.0f - mx - mn) : c / (mx + mn);
    if      (mx == rgb[0]) *h = 60.0f * (((rgb[1] - rgb[2]) / c) + (rgb[1] < rgb[2] ? 6.0f : 0.0f));
    else if (mx == rgb[1]) *h = 60.0f * (((rgb[2] - rgb[0]) / c) + 2.0f);
    else                   *h = 60.0f * (((rgb[0] - rgb[1]) / c) + 4.0f);
}

static float hue_chan(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static void hsl_to_rgb(float h, float s, float l, float *rgb)
{
    float q, p, hn;
    if (s < 1e-6f) { rgb[0] = rgb[1] = rgb[2] = l; return; }
    q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
    p = 2.0f * l - q;
    while (h < 0.0f)    h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    hn = h / 360.0f;
    rgb[0] = hue_chan(p, q, hn + 1.0f / 3.0f);
    rgb[1] = hue_chan(p, q, hn);
    rgb[2] = hue_chan(p, q, hn - 1.0f / 3.0f);
}

/* ------------------------------------------------------- basic panel - */
/*
 * Approximations of Adobe's PV2012 tone controls. The weighting functions
 * are chosen so Shadows and Highlights hold both endpoints fixed (they
 * reshape the middle) while Blacks and Whites move the endpoints, which is
 * how the real controls behave. Not bit-accurate to Adobe -- nothing short
 * of their source could be -- but the right shape and range.
 */
static float basic_tone(const se_grade *g, float v)
{
    float w;

    /* shadows / highlights: zero at both ends, peaked in their own region */
    if (g->shadows != 0.0f) {
        w = 4.0f * v * (1.0f - v) * (1.0f - v) * (1.0f - v) * 1.7f;
        v += 0.42f * (g->shadows / 100.0f) * w;
    }
    if (g->highlights != 0.0f) {
        w = 4.0f * v * v * v * (1.0f - v) * 1.7f;
        v += 0.42f * (g->highlights / 100.0f) * w;
    }

    /* blacks / whites: move the endpoints */
    if (g->blacks != 0.0f) {
        w = (1.0f - v); w = w * w * w * w;
        v += 0.18f * (g->blacks / 100.0f) * w;
    }
    if (g->whites != 0.0f) {
        w = v * v * v * v;
        v += 0.18f * (g->whites / 100.0f) * w;
    }

    v = SE_CLAMP(v, 0.0f, 1.0f);

    /* contrast: an S around middle grey */
    if (g->contrast != 0.0f) {
        float s = v * v * (3.0f - 2.0f * v);
        v = v + (s - v) * (g->contrast / 100.0f);
        v = SE_CLAMP(v, 0.0f, 1.0f);
    }
    return v;
}

/* ------------------------------------------------------------- bake -- */

/*
 * Apply the grade to one RGB triple in the sRGB-encoded domain, in place.
 * Factored out of the bake so a neutral ramp can be pushed through it --
 * that is how a film stock's per-channel curves are extracted from a full
 * Lightroom edit (see se_stock / the mkstock command).
 */
void se_grade_apply(const se_grade *g, float *v)
{
    int c;

    /* 1. the camera profile's own tone curve */
    if (g->profile.n >= 2)
        for (c = 0; c < 3; c++) v[c] = se_curve_eval(&g->profile, v[c]);

    /* 2. exposure, in linear light where it belongs */
    if (g->exposure != 0.0f) {
        float gain = se_powf_g(2.0f, g->exposure);
        for (c = 0; c < 3; c++)
            v[c] = srgb_enc(SE_CLAMP(srgb_dec(v[c]) * gain, 0.0f, 1.0f));
    }

    /* 3. the basic panel */
    for (c = 0; c < 3; c++) v[c] = basic_tone(g, v[c]);

    /* 4. point curves: master then per channel */
    if (g->master.n >= 2)
        for (c = 0; c < 3; c++) v[c] = se_curve_eval(&g->master, v[c]);
    if (g->red.n   >= 2) v[0] = se_curve_eval(&g->red,   v[0]);
    if (g->green.n >= 2) v[1] = se_curve_eval(&g->green, v[1]);
    if (g->blue.n  >= 2) v[2] = se_curve_eval(&g->blue,  v[2]);

    for (c = 0; c < 3; c++) v[c] = SE_CLAMP(v[c], 0.0f, 1.0f);

    /* 5. HSL bands */
    {
        float h, s, l, w[SE_HSL_BANDS];
        float dh = 0.0f, ds = 0.0f, dl = 0.0f;
        int   i, any = 0;
        for (i = 0; i < SE_HSL_BANDS; i++)
            if (g->hue[i] != 0.0f || g->sat[i] != 0.0f || g->lum[i] != 0.0f) { any = 1; break; }
        if (any) {
            rgb_to_hsl(v, &h, &s, &l);
            if (s > 1e-4f) {
                band_weights(h, w);
                for (i = 0; i < SE_HSL_BANDS; i++) {
                    dh += w[i] * g->hue[i];
                    ds += w[i] * g->sat[i];
                    dl += w[i] * g->lum[i];
                }
                h += dh * 0.30f;                      /* +/-100 -> +/-30 deg */
                s *= 1.0f + SE_CLAMP(ds / 100.0f, -1.0f, 1.0f);
                l += dl / 100.0f * 0.22f * (1.0f - (2.0f * l - 1.0f) * (2.0f * l - 1.0f));
                s = SE_CLAMP(s, 0.0f, 1.0f);
                l = SE_CLAMP(l, 0.0f, 1.0f);
                hsl_to_rgb(h, s, l, v);
            }
        }
    }

    /* 6. monochrome conversion, using the B&W panel's band weights */
    if (g->monochrome) {
        float h, s, l, w[SE_HSL_BANDS];
        float y = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        int   i;
        rgb_to_hsl(v, &h, &s, &l);
        if (s > 1e-4f) {
            band_weights(h, w);
            for (i = 0; i < SE_HSL_BANDS; i++)
                y += w[i] * (g->mono_mix[i] / 100.0f) * 0.30f * s;
        }
        v[0] = v[1] = v[2] = SE_CLAMP(y, 0.0f, 1.0f);
    }

    /* 7. saturation and vibrance */
    if (!g->monochrome && (g->saturation != 0.0f || g->vibrance != 0.0f)) {
        float y  = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        float mx = SE_MAX(v[0], SE_MAX(v[1], v[2]));
        float mn = SE_MIN(v[0], SE_MIN(v[1], v[2]));
        float sn = mx > 0.001f ? (mx - mn) / mx : 0.0f;
        float k  = 1.0f + g->saturation / 100.0f
                        + (g->vibrance / 100.0f) * (1.0f - sn);
        for (c = 0; c < 3; c++) v[c] = y + (v[c] - y) * k;
    }

}

se_err se_grade_bake(const se_grade *g, int size, se_lut3 *out, se_arena *a)
{
    int ri, gi, bi, c;
    se_err rc = se_lut3_identity(out, size, a);
    if (rc != SE_OK) return rc;

    for (bi = 0; bi < size; bi++)
    for (gi = 0; gi < size; gi++)
    for (ri = 0; ri < size; ri++) {
        size_t idx = ((size_t)bi * size * size + (size_t)gi * size + ri) * 3;
        float v[3];

        v[0] = (float)ri / (float)(size - 1);
        v[1] = (float)gi / (float)(size - 1);
        v[2] = (float)bi / (float)(size - 1);

        se_grade_apply(g, v);

        for (c = 0; c < 3; c++)
            out->data[idx + c] =
                (uint16_t)SE_CLAMP(v[c] * 65535.0f + 0.5f, 0.0f, 65535.0f);
    }
    return SE_OK;
}
