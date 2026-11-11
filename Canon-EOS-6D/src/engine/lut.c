#include "lut.h"
#include "se_libc.h"

/* ------------------------------------------------------------ 1D curve */

static float se_powf_approx(float x, float e)
{
    /* log2/exp2 pair, enough for building a 4096-entry table once. */
    float lx, y, t, acc;
    int   n = 0, i;
    if (x <= 0.0f) return 0.0f;
    while (x < 1.0f)  { x *= 2.0f; n--; }
    while (x >= 2.0f) { x *= 0.5f; n++; }
    /* log2(x) for x in [1,2) via ln((1+u)/(1-u)) series */
    t = (x - 1.0f) / (x + 1.0f);
    acc = 0.0f;
    { float tp = t; for (i = 1; i <= 15; i += 2) { acc += tp / (float)i; tp *= t * t; } }
    lx = (float)n + acc * 2.0f * 1.44269504f;

    y = lx * e;
    /* 2^y */
    { int   m = (int)(y < 0 ? y - 0.5f : y + 0.5f);
      float f = y - (float)m, r = 1.0f, p = 1.0f;
      for (i = 1; i <= 8; i++) { p *= f * 0.69314718f / (float)i; r += p; }
      while (m > 0) { r *= 2.0f; m--; }
      while (m < 0) { r *= 0.5f; m++; }
      return r; }
}

void se_lut1_srgb_encode(se_lut1 *t)
{
    int i;
    for (i = 0; i < SE_LUT1_SIZE; i++) {
        float x = (float)i / (float)(SE_LUT1_SIZE - 1);
        float y = (x <= 0.0031308f)
                ? x * 12.92f
                : 1.055f * se_powf_approx(x, 1.0f / 2.4f) - 0.055f;
        t->v[i] = (uint16_t)SE_CLAMP(y * 65535.0f + 0.5f, 0.0f, 65535.0f);
    }
}


void se_lut1_bytes(const se_lut1 *t, uint8_t *out256)
{
    int i;
    for (i = 0; i < 256; i++) {
        /* 257 = 65535/255, so 0 maps to 0 and 255 to 65535 exactly */
        unsigned v = se_lut1_eval(t, (uint16_t)(i * 257)) >> 8;
        out256[i] = (uint8_t)(v > 255 ? 255 : v);
    }
}

void se_lut1_srgb_decode(se_lut1 *t)
{
    int i;
    for (i = 0; i < SE_LUT1_SIZE; i++) {
        float x = (float)i / (float)(SE_LUT1_SIZE - 1);
        float y = (x <= 0.04045f)
                ? x / 12.92f
                : se_powf_approx((x + 0.055f) / 1.055f, 2.4f);
        t->v[i] = (uint16_t)SE_CLAMP(y * 65535.0f + 0.5f, 0.0f, 65535.0f);
    }
}

/* ------------------------------------------------------------- 3D LUT */

se_err se_lut3_identity(se_lut3 *lut, int size, se_arena *a)
{
    int r, g, b;
    if (size < 2 || size > SE_LUT3_MAX_SIZE) return SE_E_UNSUPPORTED;
    lut->size = size;
    lut->data = SE_NEW(a, uint16_t, (size_t)size * size * size * 3);
    if (!lut->data) return SE_E_NOMEM;
    for (b = 0; b < size; b++)
        for (g = 0; g < size; g++)
            for (r = 0; r < size; r++) {
                size_t i = ((size_t)b * size * size + (size_t)g * size + r) * 3;
                lut->data[i + 0] = (uint16_t)(r * 65535 / (size - 1));
                lut->data[i + 1] = (uint16_t)(g * 65535 / (size - 1));
                lut->data[i + 2] = (uint16_t)(b * 65535 / (size - 1));
            }
    return SE_OK;
}

/* --- a tiny text scanner, because the engine has no stdio ------------- */

typedef struct { const char *p, *end; } txt;

static int txt_eof(txt *t) { return t->p >= t->end; }

static void txt_skip_space(txt *t)
{
    while (t->p < t->end && (*t->p == ' ' || *t->p == '\t' || *t->p == '\r'))
        t->p++;
}

static void txt_skip_line(txt *t)
{
    while (t->p < t->end && *t->p != '\n') t->p++;
    if (t->p < t->end) t->p++;
}

static int txt_match(txt *t, const char *kw)
{
    size_t n = strlen(kw);
    if ((size_t)(t->end - t->p) < n) return 0;
    if (strncmp(t->p, kw, n) != 0) return 0;
    t->p += n;
    return 1;
}

/* Parse a decimal float. Handles sign, fraction and exponent. */
static int txt_float(txt *t, float *out)
{
    int   neg = 0, any = 0;
    float v = 0.0f, scale;
    txt_skip_space(t);
    if (txt_eof(t)) return 0;
    if (*t->p == '+' || *t->p == '-') { neg = (*t->p == '-'); t->p++; }
    while (t->p < t->end && *t->p >= '0' && *t->p <= '9') {
        v = v * 10.0f + (float)(*t->p - '0');
        t->p++; any = 1;
    }
    if (t->p < t->end && *t->p == '.') {
        t->p++;
        scale = 0.1f;
        while (t->p < t->end && *t->p >= '0' && *t->p <= '9') {
            v += (float)(*t->p - '0') * scale;
            scale *= 0.1f;
            t->p++; any = 1;
        }
    }
    if (!any) return 0;
    if (t->p < t->end && (*t->p == 'e' || *t->p == 'E')) {
        int eneg = 0, e = 0;
        t->p++;
        if (t->p < t->end && (*t->p == '+' || *t->p == '-')) {
            eneg = (*t->p == '-'); t->p++;
        }
        while (t->p < t->end && *t->p >= '0' && *t->p <= '9') {
            e = e * 10 + (*t->p - '0'); t->p++;
        }
        while (e-- > 0) v = eneg ? v * 0.1f : v * 10.0f;
    }
    *out = neg ? -v : v;
    return 1;
}

static int txt_int(txt *t, int *out)
{
    float f;
    if (!txt_float(t, &f)) return 0;
    *out = (int)(f + 0.5f);
    return 1;
}

se_err se_lut3_parse_cube(const char *text, size_t len, se_lut3 *lut, se_arena *a)
{
    txt   t;
    int   size = 0;
    float dmin[3] = { 0.0f, 0.0f, 0.0f };
    float dmax[3] = { 1.0f, 1.0f, 1.0f };
    long  want, got = 0;
    uint16_t *out;

    t.p = text; t.end = text + len;

    /* --- header ---------------------------------------------------- */
    while (!txt_eof(&t)) {
        const char *line_start = t.p;
        txt_skip_space(&t);
        if (txt_eof(&t)) break;

        if (*t.p == '#' || *t.p == '\n') { txt_skip_line(&t); continue; }

        if (txt_match(&t, "TITLE"))          { txt_skip_line(&t); continue; }
        if (txt_match(&t, "LUT_1D_SIZE")) {
            se_log("lut: 1D .cube files are not a colour grade, ignoring");
            return SE_E_UNSUPPORTED;
        }
        if (txt_match(&t, "LUT_3D_SIZE")) {
            if (!txt_int(&t, &size)) return SE_E_BADFILE;
            txt_skip_line(&t);
            continue;
        }
        if (txt_match(&t, "DOMAIN_MIN")) {
            txt_float(&t, &dmin[0]); txt_float(&t, &dmin[1]); txt_float(&t, &dmin[2]);
            txt_skip_line(&t);
            continue;
        }
        if (txt_match(&t, "DOMAIN_MAX")) {
            txt_float(&t, &dmax[0]); txt_float(&t, &dmax[1]); txt_float(&t, &dmax[2]);
            txt_skip_line(&t);
            continue;
        }
        /* Not a keyword: this is the first data line. */
        t.p = line_start;
        break;
    }

    if (size < 2 || size > SE_LUT3_MAX_SIZE) {
        se_log("lut: LUT_3D_SIZE %d out of range (2..%d)", size, SE_LUT3_MAX_SIZE);
        return SE_E_UNSUPPORTED;
    }
    if (dmin[0] != 0.0f || dmax[0] != 1.0f)
        se_log("lut: domain [%d/1000..%d/1000] will be renormalised",
               (int)(dmin[0] * 1000), (int)(dmax[0] * 1000));

    lut->size = size;
    want = (long)size * size * size;
    lut->data = SE_NEW(a, uint16_t, (size_t)want * 3);
    if (!lut->data) return SE_E_NOMEM;
    out = lut->data;

    /* --- table ------------------------------------------------------ */
    while (got < want && !txt_eof(&t)) {
        float v[3];
        int   c;
        txt_skip_space(&t);
        if (txt_eof(&t)) break;
        if (*t.p == '#' || *t.p == '\n') { txt_skip_line(&t); continue; }
        if (!txt_float(&t, &v[0])) { txt_skip_line(&t); continue; }
        if (!txt_float(&t, &v[1]) || !txt_float(&t, &v[2])) {
            se_log("lut: short data row at entry %ld", got);
            return SE_E_BADFILE;
        }
        for (c = 0; c < 3; c++) {
            float span = dmax[c] - dmin[c];
            float x = span > 1e-6f ? (v[c] - dmin[c]) / span : v[c];
            out[got * 3 + c] = (uint16_t)SE_CLAMP(x * 65535.0f + 0.5f, 0.0f, 65535.0f);
        }
        got++;
        txt_skip_line(&t);
    }

    if (got != want) {
        se_log("lut: expected %ld entries, found %ld", want, got);
        return SE_E_BADFILE;
    }
    se_log("lut: loaded %dx%dx%d cube", size, size, size);
    return SE_OK;
}

/*
 * Tetrahedral interpolation. Preferred over trilinear because it keeps the
 * neutral axis exactly neutral: along r == g == b the four-point blend
 * degenerates to a straight line between two diagonal vertices, so grey
 * stays grey instead of picking up the colour cast trilinear introduces.
 */
void se_lut3_apply_row(const se_lut3 *lut, uint16_t *rgb, int width)
{
    const int      N  = lut->size;
    const int      S  = N - 1;
    const uint16_t *D = lut->data;
    const int      sg = N * 3;
    const int      sb = N * N * 3;
    int x;

    for (x = 0; x < width; x++) {
        uint32_t tr = (uint32_t)rgb[x * 3 + 0] * (uint32_t)S;
        uint32_t tg = (uint32_t)rgb[x * 3 + 1] * (uint32_t)S;
        uint32_t tb = (uint32_t)rgb[x * 3 + 2] * (uint32_t)S;
        int ir = (int)(tr / 65535u), ig = (int)(tg / 65535u), ib = (int)(tb / 65535u);
        int fr, fg, fb;
        const uint16_t *c000, *c111;
        const uint16_t *ca, *cb2;
        int wa, wb, wc;
        int c;

        if (ir >= S) { ir = S - 1; fr = 65535; } else fr = (int)(tr - (uint32_t)ir * 65535u);
        if (ig >= S) { ig = S - 1; fg = 65535; } else fg = (int)(tg - (uint32_t)ig * 65535u);
        if (ib >= S) { ib = S - 1; fb = 65535; } else fb = (int)(tb - (uint32_t)ib * 65535u);

        c000 = D + ((size_t)ib * N * N + (size_t)ig * N + ir) * 3;
        c111 = c000 + 3 + sg + sb;

        /*
         * The cube splits into six tetrahedra; the ordering of the three
         * fractions picks which one the sample falls in, and with it the
         * two intermediate vertices.
         */
        if (fr >= fg) {
            if (fg >= fb)      { ca = c000 + 3;           cb2 = c000 + 3 + sg;      wa = fr - fg; wb = fg - fb; wc = fb; }
            else if (fr >= fb) { ca = c000 + 3;           cb2 = c000 + 3 + sb;      wa = fr - fb; wb = fb - fg; wc = fg; }
            else               { ca = c000 + sb;          cb2 = c000 + 3 + sb;      wa = fb - fr; wb = fr - fg; wc = fg; }
        } else {
            if (fb >= fg)      { ca = c000 + sb;          cb2 = c000 + sg + sb;     wa = fb - fg; wb = fg - fr; wc = fr; }
            else if (fb >= fr) { ca = c000 + sg;          cb2 = c000 + sg + sb;     wa = fg - fb; wb = fb - fr; wc = fr; }
            else               { ca = c000 + sg;          cb2 = c000 + 3 + sg;      wa = fg - fr; wb = fr - fb; wc = fb; }
        }

        for (c = 0; c < 3; c++) {
            int64_t v = (int64_t)c000[c] * (65535 - wa - wb - wc)
                      + (int64_t)ca[c]   * wa
                      + (int64_t)cb2[c]  * wb
                      + (int64_t)c111[c] * wc;
            v /= 65535;
            rgb[x * 3 + c] = (uint16_t)SE_CLAMP(v, 0, 65535);
        }
    }
}
