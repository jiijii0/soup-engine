#include "stock.h"
#include "wb.h"
#include "se_libc.h"

void se_stock_init(se_stock *s)
{
    int i;
    memset(s, 0, sizeof *s);
    s->wb_preset = -1;
    for (i = 0; i < 6; i++) { s->hue_sat[i] = 100.0f;
                              s->hue_lum[i] = 100.0f;
                              s->hue_rot[i] = 0.0f; }
}

/* --------------------------------------------- which path is asking --- */

float se_stock_grain(const se_stock *s, int src)
{
    if (src == SE_SRC_RAW && s->raw.grain_set) return s->raw.grain;
    return s->grain;
}

float se_stock_grain_size(const se_stock *s, int src)
{
    if (src == SE_SRC_RAW && s->raw.grain_size_set) return s->raw.grain_size;
    return s->grain_size;
}

int se_stock_wb(const se_stock *s, int src)
{
    if (src == SE_SRC_RAW && s->raw.wb_set) return s->raw.wb_preset;
    return s->wb_preset;
}

/* The curves a path should use: the [RAW] override when there is one and
 * raw is asking, otherwise the common set. A curve with fewer than two
 * points is not an override -- parse_curve zeroes n for those. */
static void pick_curves(const se_stock *s, int src, const se_curve *out[4])
{
    const se_stock_raw *v = &s->raw;
    int raw = (src == SE_SRC_RAW);
    out[0] = (raw && v->master.n >= 2) ? &v->master : &s->master;
    out[1] = (raw && v->r.n      >= 2) ? &v->r      : &s->r;
    out[2] = (raw && v->g.n      >= 2) ? &v->g      : &s->g;
    out[3] = (raw && v->b.n      >= 2) ? &v->b      : &s->b;
}

static float pick_exposure(const se_stock *s, int src)
{
    if (src == SE_SRC_RAW && s->raw.exposure_set) return s->raw.exposure;
    return s->exposure;
}

/* ------------------------------------------------------------ parsing */

typedef struct { const char *p, *end; } sc;

static void sc_sp(sc *t)   { while (t->p < t->end && (*t->p == ' ' || *t->p == '\t')) t->p++; }
static void sc_line(sc *t) { while (t->p < t->end && *t->p != '\n') t->p++; if (t->p < t->end) t->p++; }

static int sc_word(sc *t, char *dst, size_t n)
{
    size_t i = 0;
    sc_sp(t);
    while (t->p < t->end && *t->p != ' ' && *t->p != '\t' &&
           *t->p != '\n' && *t->p != '\r' && i + 1 < n)
        dst[i++] = *t->p++;
    dst[i] = 0;
    return i > 0;
}

/* Rest of the line, trimmed. Used for free-text fields. */
static void sc_rest(sc *t, char *dst, size_t n)
{
    size_t i = 0;
    sc_sp(t);
    while (t->p < t->end && *t->p != '\n' && *t->p != '\r' && i + 1 < n)
        dst[i++] = *t->p++;
    while (i && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) i--;
    dst[i] = 0;
}

static int sc_float(sc *t, float *out)
{
    int   neg = 0, any = 0;
    float v = 0.0f, scale;
    sc_sp(t);
    if (t->p >= t->end) return 0;
    if (*t->p == '+' || *t->p == '-') { neg = (*t->p == '-'); t->p++; }
    while (t->p < t->end && *t->p >= '0' && *t->p <= '9') {
        v = v * 10.0f + (float)(*t->p - '0'); t->p++; any = 1;
    }
    if (t->p < t->end && *t->p == '.') {
        t->p++; scale = 0.1f;
        while (t->p < t->end && *t->p >= '0' && *t->p <= '9') {
            v += (float)(*t->p - '0') * scale; scale *= 0.1f; t->p++; any = 1;
        }
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* "0,0 51,32 255,238" up to the end of the line */
static void parse_curve(sc *t, se_curve *c)
{
    c->n = 0;
    while (c->n < SE_CURVE_MAX_PTS) {
        float x, y;
        sc_sp(t);
        if (t->p >= t->end || *t->p == '\n' || *t->p == '\r' || *t->p == '#') break;
        if (!sc_float(t, &x)) break;
        sc_sp(t);
        if (t->p < t->end && *t->p == ',') t->p++;
        if (!sc_float(t, &y)) break;
        c->x[c->n] = (uint8_t)SE_CLAMP(x, 0.0f, 255.0f);
        c->y[c->n] = (uint8_t)SE_CLAMP(y, 0.0f, 255.0f);
        c->n++;
    }
    if (c->n < 2) c->n = 0;    /* fewer than two points is the identity */
}

/* "a,b,c" or "a b c" -- n floats, comma optional. Used for the split
 * tone triples and the halation tint. Missing components stay as they are,
 * so a short line is a partial edit rather than a reset to zero. */
static void parse_vec(sc *t, float *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        float v;
        sc_sp(t);
        if (t->p < t->end && *t->p == ',') t->p++;
        if (!sc_float(t, &v)) break;
        out[i] = v;
    }
}

static int key_is(const char *w, const char *k)
{
    return strcmp(w, k) == 0;
}

se_err se_stock_parse(const char *text, size_t len, se_stock *s)
{
    sc   t;
    char w[32];
    int  got_name = 0, got_tint = 0;
    int  in_raw = 0;        /* inside a [RAW] block */
    char unknown[120];      /* the keys we had to skip, named together */
    int  nunknown = 0;
    unknown[0] = 0;

    se_stock_init(s);
    t.p = text; t.end = text + len;

    while (t.p < t.end) {
        sc_sp(&t);
        if (t.p >= t.end) break;
        if (*t.p == '#' || *t.p == '\n' || *t.p == '\r') { sc_line(&t); continue; }
        if (!sc_word(&t, w, sizeof w)) { sc_line(&t); continue; }

        /* A section header switches where the keys below it land. */
        if (w[0] == '[') {
            if      (key_is(w, "[RAW]"))    in_raw = 1;
            else if (key_is(w, "[JPEG]"))   in_raw = 0;
            else if (key_is(w, "[COMMON]")) in_raw = 0;
            else se_log("stock: unknown section '%s', ignoring it", w);
            sc_line(&t); continue;
        }

        /*
         * Inside [RAW], only what the raw path can actually do.
         *
         * The others need all three colour channels at one pixel and the
         * raw path works on mosaiced samples, where no pixel has them.
         * Silently dropping them would leave someone tuning numbers that
         * cannot take effect, so say so.
         */
        if (in_raw) {
            if (key_is(w, "SAT") || key_is(w, "HUE_SAT") ||
                key_is(w, "HUE_ROT") || key_is(w, "HUE_LUM") ||
                key_is(w, "SPLIT_SH") || key_is(w, "SPLIT_HI") ||
                key_is(w, "HALATION") || key_is(w, "HAL_TINT")) {
                se_log("stock: '%s' cannot work from raw -- it needs all "
                       "three channels at one pixel, and the sensor data "
                       "is mosaiced. Ignoring it inside [RAW].", w);
                sc_line(&t); continue;
            }
            if      (key_is(w, "CURVE"))   { parse_curve(&t, &s->raw.master); sc_line(&t); continue; }
            else if (key_is(w, "CURVE_R")) { parse_curve(&t, &s->raw.r);      sc_line(&t); continue; }
            else if (key_is(w, "CURVE_G")) { parse_curve(&t, &s->raw.g);      sc_line(&t); continue; }
            else if (key_is(w, "CURVE_B")) { parse_curve(&t, &s->raw.b);      sc_line(&t); continue; }
            else if (key_is(w, "EXPOSURE")) {
                if (sc_float(&t, &s->raw.exposure)) s->raw.exposure_set = 1;
                sc_line(&t); continue;
            }
            else if (key_is(w, "GRAIN")) {
                if (sc_float(&t, &s->raw.grain)) s->raw.grain_set = 1;
                sc_line(&t); continue;
            }
            else if (key_is(w, "GRAIN_SIZE")) {
                if (sc_float(&t, &s->raw.grain_size)) s->raw.grain_size_set = 1;
                sc_line(&t); continue;
            }
            /* NAME/DESC/WB and anything else fall through to the common
             * handling below, which is right for WB -- it is a raw-only
             * operation anyway -- and harmless for the rest. */
        }

        if      (key_is(w, "NAME"))     { sc_rest(&t, s->name, sizeof s->name); got_name = 1; }
        else if (key_is(w, "DESC"))     { sc_rest(&t, s->desc, sizeof s->desc); }
        else if (key_is(w, "VERSION")) {
            float v = 0.0f;
            if (sc_float(&t, &v)) s->version = (int)(v + 0.5f);
        }
        else if (key_is(w, "EXPOSURE")) { sc_float(&t, &s->exposure); }
        else if (key_is(w, "GRAIN"))    { sc_float(&t, &s->grain); }
        else if (key_is(w, "GRAIN_SIZE")) { sc_float(&t, &s->grain_size); }
        else if (key_is(w, "SAT"))      { if (sc_float(&t, &s->sat)) s->sat_set = 1; }
        else if (key_is(w, "HUE_SAT"))  parse_vec(&t, s->hue_sat, 6);
        else if (key_is(w, "HUE_ROT"))  parse_vec(&t, s->hue_rot, 6);
        else if (key_is(w, "HUE_LUM"))  parse_vec(&t, s->hue_lum, 6);
        else if (key_is(w, "SPLIT_SH")) parse_vec(&t, s->split_sh, 3);
        else if (key_is(w, "SPLIT_HI")) parse_vec(&t, s->split_hi, 3);
        else if (key_is(w, "HAL_TINT")) { parse_vec(&t, s->hal_tint, 3); got_tint = 1; }
        else if (key_is(w, "HALATION")) {
            sc_float(&t, &s->hal_amount);
            sc_float(&t, &s->hal_thresh);
            sc_float(&t, &s->hal_radius);
        }
        else if (key_is(w, "WB")) {
            char v[24];
            int  p_wb = -1;
            sc_rest(&t, v, sizeof v);
            if (key_is(v, "AS_SHOT") || key_is(v, "AS SHOT")) p_wb = -1;
            else {
                int p = se_wb_from_name(v);
                /* the file uses underscores and caps; map the common ones */
                if (p < 0) {
                    if      (key_is(v, "DAYLIGHT"))    p = SE_WB_DAYLIGHT;
                    else if (key_is(v, "SHADE"))       p = SE_WB_SHADE;
                    else if (key_is(v, "CLOUDY"))      p = SE_WB_CLOUDY;
                    else if (key_is(v, "TUNGSTEN"))    p = SE_WB_TUNGSTEN;
                    else if (key_is(v, "FLUORESCENT")) p = SE_WB_FLUORESCENT;
                    else if (key_is(v, "FLASH"))       p = SE_WB_FLASH;
                }
                if (p < 0) se_log("stock: unknown white balance '%s', using as shot", v);
                p_wb = p;
            }
            if (in_raw) { s->raw.wb_preset = p_wb; s->raw.wb_set = 1; }
            else          s->wb_preset = p_wb;
        }
        else if (key_is(w, "CURVE"))   parse_curve(&t, &s->master);
        else if (key_is(w, "CURVE_R")) parse_curve(&t, &s->r);
        else if (key_is(w, "CURVE_G")) parse_curve(&t, &s->g);
        else if (key_is(w, "CURVE_B")) parse_curve(&t, &s->b);
        else {
            /* Collected rather than logged one at a time, so the report
             * below can name them beside the version that explains them. */
            int k = 0, j = 0;
            while (unknown[k]) k++;
            if (k + 12 < (int)sizeof unknown) {
                if (k) { unknown[k++] = ','; unknown[k++] = ' '; }
                while (w[j] && k + 1 < (int)sizeof unknown) unknown[k++] = w[j++];
                unknown[k] = 0;
            }
            nunknown++;
        }

        sc_line(&t);
    }

    if (!got_name) {
        se_log("stock: file has no NAME");
        return SE_E_BADFILE;
    }

    /*
     * Say when a file was written for a newer engine than this one.
     *
     * Unknown keys are skipped on purpose so an old build still loads a
     * new file, but that means the difference shows up as a wrong
     * picture and nothing else. Naming the version and the keys turns a
     * mystery into a sentence.
     */
    if (s->version > SE_STOCK_VERSION)
        se_log("stock '%s': written for format v%d, this engine speaks v%d"
               "%s%s -- it will not look as intended",
               s->name, s->version, SE_STOCK_VERSION,
               nunknown ? "; ignored: " : "", nunknown ? unknown : "");
    else if (nunknown)
        se_log("stock '%s': ignored %d unknown key%s (%s)",
               s->name, nunknown, nunknown == 1 ? "" : "s", unknown);

    return SE_OK;

    /* A stock that asked for halation and not for a colour gets the colour
     * halation actually is: the red-orange of light coming back up through
     * the emulsion. Defaulting to zero would silently mean "no glow". */
    if (s->hal_amount > 0.0f && !got_tint) {
        s->hal_tint[0] = 255.0f; s->hal_tint[1] = 64.0f; s->hal_tint[2] = 24.0f;
    }
    if (s->hal_amount > 0.0f && s->hal_radius < 1.0f) s->hal_radius = 8.0f;
    if (s->hal_amount > 0.0f && s->hal_thresh <= 0.0f) s->hal_thresh = 200.0f;

    return SE_OK;
}

/* -------------------------------------------------------------- bake */

static float st_pow(float x, float e)
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

static float enc(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x <= 0.0031308f ? x * 12.92f : 1.055f * st_pow(x, 1.0f / 2.4f) - 0.055f;
}

static float dec(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x <= 0.04045f ? x / 12.92f : st_pow((x + 0.055f) / 1.055f, 2.4f);
}

void se_stock_bake(const se_stock *s, se_lut1 out[3])
{
    /* This function IS the raw path, so it takes the [RAW] overrides.
     * No caller has to say which path it is -- the two bakes already
     * encode that, which is why neither signature changed. */
    const se_curve *cv[4];
    float gain;
    int   c, i;

    pick_curves(s, SE_SRC_RAW, cv);
    gain = pick_exposure(s, SE_SRC_RAW) != 0.0f
         ? st_pow(2.0f, pick_exposure(s, SE_SRC_RAW)) : 1.0f;

    for (c = 0; c < 3; c++) {
        for (i = 0; i < SE_LUT1_SIZE; i++) {
            float lin = (float)i / (float)(SE_LUT1_SIZE - 1);
            float v;
            lin *= gain;
            if (lin > 1.0f) lin = 1.0f;
            v = enc(lin);                                   /* to the curve domain */
            if (cv[0]->n >= 2)     v = se_curve_eval(cv[0], v);
            if (cv[c + 1]->n >= 2) v = se_curve_eval(cv[c + 1], v);
            v = SE_CLAMP(v, 0.0f, 1.0f);
            v = dec(v);                                      /* back to linear */
            out[c].v[i] = (uint16_t)SE_CLAMP(v * 65535.0f + 0.5f, 0.0f, 65535.0f);
        }
    }
}

/*
 * The same look, for data that is already sRGB-encoded.
 *
 * se_stock_bake above is linear in and linear out, because the raw pipeline
 * hands it scene-linear sensor data. The preview path does not: it decodes
 * the JPEG DIGIC already made, whose samples are sRGB-encoded and
 * tone-mapped. Feeding those to the linear table evaluates the curve at the
 * wrong place on itself and then gamma-decodes the result before writing it
 * back as though it were sRGB -- so every stock with a curve came out dark
 * and muddy, and `standard`, which has no curves, was the only one that
 * looked right, because dec(enc(s)) == s.
 *
 * That is why the camera's pictures read as an edit rather than a look: the
 * looks were never actually being applied as written.
 *
 * Here the index is the sRGB value and the output stays sRGB. Exposure is
 * the one thing that must still happen in linear, because that is what a
 * stop means, so it is decoded and re-encoded around the multiply.
 */
/*
 * The same curve, evaluated only at the 256 values an 8-bit path can hold.
 *
 * The capture path grades the JPEG Canon already made: 8-bit in, 8-bit out,
 * so 4096 interpolated entries per channel describe 255 steps sixteen times
 * over. Baking straight to bytes costs 768 curve evaluations instead of
 * 12,288, needs no 24 KB se_lut1 at all -- which is what the payload's bss
 * could no longer afford -- and makes applying the look one byte load.
 *
 * It is also slightly more accurate than collapsing the big table
 * afterwards, because nothing is interpolated between entries.
 */
void se_stock_bake_srgb8(const se_stock *s, uint8_t out[3][256])
{
    /* And this one IS the JPEG path, so it takes the common fields and
     * never the [RAW] block. That is not an oversight: the top level of
     * a stock file is the JPEG look. */
    const se_curve *per[3];
    float gain = s->exposure != 0.0f ? st_pow(2.0f, s->exposure) : 1.0f;
    int   c, i;

    per[0] = &s->r; per[1] = &s->g; per[2] = &s->b;

    for (c = 0; c < 3; c++) {
        for (i = 0; i < 256; i++) {
            float v = (float)i / 255.0f;
            if (gain != 1.0f) {
                float lin = dec(v) * gain;
                if (lin > 1.0f) lin = 1.0f;
                v = enc(lin);
            }
            if (s->master.n >= 2) v = se_curve_eval(&s->master, v);
            if (per[c]->n  >= 2) v = se_curve_eval(per[c], v);
            v = SE_CLAMP(v, 0.0f, 1.0f);
            out[c][i] = (uint8_t)SE_CLAMP(v * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }
}

void se_stock_bake_srgb(const se_stock *s, se_lut1 out[3])
{
    const se_curve *per[3];
    float gain = s->exposure != 0.0f ? st_pow(2.0f, s->exposure) : 1.0f;
    int   c, i;

    per[0] = &s->r; per[1] = &s->g; per[2] = &s->b;

    for (c = 0; c < 3; c++) {
        for (i = 0; i < SE_LUT1_SIZE; i++) {
            float v = (float)i / (float)(SE_LUT1_SIZE - 1);
            if (gain != 1.0f) {
                float lin = dec(v) * gain;
                if (lin > 1.0f) lin = 1.0f;
                v = enc(lin);
            }
            if (s->master.n >= 2) v = se_curve_eval(&s->master, v);
            if (per[c]->n  >= 2) v = se_curve_eval(per[c], v);
            v = SE_CLAMP(v, 0.0f, 1.0f);
            out[c].v[i] = (uint16_t)SE_CLAMP(v * 65535.0f + 0.5f, 0.0f, 65535.0f);
        }
    }
}
