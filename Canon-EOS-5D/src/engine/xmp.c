#include "xmp.h"
#include "cr2.h"
#include "se_libc.h"

/* --------------------------------------------------------- scanning -- */

static const char *mem_find(const char *hay, size_t n, const char *needle,
                            const char *from)
{
    size_t m = strlen(needle);
    const char *p   = from ? from : hay;
    const char *end = hay + n;
    if (m == 0 || m > (size_t)(end - p)) return NULL;
    for (; p <= end - m; p++)
        if (p[0] == needle[0] && strncmp(p, needle, m) == 0) return p;
    return NULL;
}

/* Read a quoted attribute value: key="value". */
static int attr_str(const char *t, size_t n, const char *key,
                    char *dst, size_t dstsz)
{
    char pat[64];
    const char *p;
    size_t i = 0, kl = strlen(key);
    if (kl + 2 >= sizeof pat) return 0;
    memcpy(pat, key, kl);
    pat[kl] = '=';
    pat[kl + 1] = '"';
    pat[kl + 2] = 0;
    p = mem_find(t, n, pat, NULL);
    if (!p) return 0;
    p += kl + 2;
    while (p < t + n && *p != '"' && i + 1 < dstsz) dst[i++] = *p++;
    dst[i] = 0;
    return 1;
}

static int str_to_float(const char *s, float *out)
{
    int   neg = 0, any = 0;
    float v = 0.0f, scale;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10.0f + (float)(*s - '0'); s++; any = 1; }
    if (*s == '.') {
        s++; scale = 0.1f;
        while (*s >= '0' && *s <= '9') { v += (float)(*s - '0') * scale; scale *= 0.1f; s++; any = 1; }
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

static int attr_float(const char *t, size_t n, const char *key, float *out)
{
    char buf[32];
    if (!attr_str(t, n, key, buf, sizeof buf)) return 0;
    return str_to_float(buf, out);
}

/*
 * Read a <crs:TagName> ... <rdf:li>x, y</rdf:li> ... </crs:TagName> block
 * into a curve. `region_end` bounds the search so the user's curve and the
 * profile's curve (which share a tag name) do not get confused.
 */
static int read_curve(const char *t, size_t n, const char *tag,
                      const char *from, const char *region_end, se_curve *c)
{
    char open_tag[48], close_tag[48];
    const char *p, *end, *li;
    size_t kl = strlen(tag);

    c->n = 0;
    if (kl + 10 >= sizeof open_tag) return 0;

    open_tag[0] = '<'; memcpy(open_tag + 1, tag, kl);
    open_tag[1 + kl] = '>'; open_tag[2 + kl] = 0;
    close_tag[0] = '<'; close_tag[1] = '/'; memcpy(close_tag + 2, tag, kl);
    close_tag[2 + kl] = '>'; close_tag[3 + kl] = 0;

    p = mem_find(t, n, open_tag, from);
    if (!p || (region_end && p >= region_end)) return 0;
    end = mem_find(t, n, close_tag, p);
    if (!end) return 0;

    li = p;
    while (c->n < SE_CURVE_MAX_PTS) {
        const char *a, *b;
        float x, y;
        char  num[16];
        size_t i;
        li = mem_find(t, n, "<rdf:li>", li + 1);
        if (!li || li >= end) break;
        a = li + 8;
        /* "x, y" */
        i = 0; b = a;
        while (b < end && *b != ',' && i + 1 < sizeof num) num[i++] = *b++;
        num[i] = 0;
        if (!str_to_float(num, &x)) break;
        if (b < end && *b == ',') b++;
        while (b < end && *b == ' ') b++;
        i = 0;
        while (b < end && *b != '<' && i + 1 < sizeof num) num[i++] = *b++;
        num[i] = 0;
        if (!str_to_float(num, &y)) break;
        c->x[c->n] = (uint8_t)SE_CLAMP(x, 0.0f, 255.0f);
        c->y[c->n] = (uint8_t)SE_CLAMP(y, 0.0f, 255.0f);
        c->n++;
    }
    if (c->n < 2) { c->n = 0; return 0; }
    return 1;
}

/* ------------------------------------------------------------ parse -- */

static const char *g_band_tag[SE_HSL_BANDS] = {
    "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"
};

static void note_unsupported(se_xmp_info *info, const char *what)
{
    if (info->unsupported_n < SE_XMP_UNSUP_MAX)
        info->unsupported[info->unsupported_n++] = what;
}

se_err se_xmp_parse(const char *text, size_t len, se_grade *g, se_xmp_info *info)
{
    char  buf[32];
    float f;
    int   i;
    const char *look_start, *params;

    se_grade_init(g);
    memset(info, 0, sizeof *info);
    info->wb_preset = -1;

    if (!mem_find(text, len, "crs:", NULL)) {
        se_log("xmp: no Camera Raw settings in this file");
        return SE_E_BADFILE;
    }
    info->found_settings = 1;

    /* --- basic panel -------------------------------------------------- */
    if (attr_float(text, len, "crs:Exposure2012",   &f)) g->exposure   = f;
    if (attr_float(text, len, "crs:Contrast2012",   &f)) g->contrast   = f;
    if (attr_float(text, len, "crs:Highlights2012", &f)) g->highlights = f;
    if (attr_float(text, len, "crs:Shadows2012",    &f)) g->shadows    = f;
    if (attr_float(text, len, "crs:Whites2012",     &f)) g->whites     = f;
    if (attr_float(text, len, "crs:Blacks2012",     &f)) g->blacks     = f;
    if (attr_float(text, len, "crs:Vibrance",       &f)) g->vibrance   = f;
    if (attr_float(text, len, "crs:Saturation",     &f)) g->saturation = f;
    info->exposure = g->exposure;

    /* --- white balance ------------------------------------------------ */
    if (attr_str(text, len, "crs:WhiteBalance", buf, sizeof buf)) {
        size_t n = strlen(buf);
        if (n < sizeof info->wb_name) memcpy(info->wb_name, buf, n + 1);
        info->wb_preset = se_wb_from_name(buf);
        if (info->wb_preset < 0 && strcmp(buf, "Custom") != 0)
            se_log("xmp: white balance '%s' has no camera preset, using as-shot", buf);
    }

    /* --- HSL ---------------------------------------------------------- */
    for (i = 0; i < SE_HSL_BANDS; i++) {
        char key[48];
        const char *b = g_band_tag[i];
        size_t bl = strlen(b);

        memcpy(key, "crs:HueAdjustment", 17);
        memcpy(key + 17, b, bl); key[17 + bl] = 0;
        if (attr_float(text, len, key, &f)) g->hue[i] = f;

        memcpy(key, "crs:SaturationAdjustment", 24);
        memcpy(key + 24, b, bl); key[24 + bl] = 0;
        if (attr_float(text, len, key, &f)) g->sat[i] = f;

        memcpy(key, "crs:LuminanceAdjustment", 23);
        memcpy(key + 23, b, bl); key[23 + bl] = 0;
        if (attr_float(text, len, key, &f)) g->lum[i] = f;

        memcpy(key, "crs:GrayMixer", 13);
        memcpy(key + 13, b, bl); key[13 + bl] = 0;
        if (attr_float(text, len, key, &f)) g->mono_mix[i] = f;
    }

    if (attr_str(text, len, "crs:ConvertToGrayscale", buf, sizeof buf))
        g->monochrome = (buf[0] == 'T' || buf[0] == 't');

    /* --- curves ------------------------------------------------------- */
    /*
     * The profile's own tone curve lives inside <crs:Look> and uses the
     * same tag name as the user's. Split the document at <crs:Look so each
     * curve is read from the right half.
     */
    look_start = mem_find(text, len, "<crs:Look", NULL);

    read_curve(text, len, "crs:ToneCurvePV2012",      NULL, look_start, &g->master);
    read_curve(text, len, "crs:ToneCurvePV2012Red",   NULL, look_start, &g->red);
    read_curve(text, len, "crs:ToneCurvePV2012Green", NULL, look_start, &g->green);
    read_curve(text, len, "crs:ToneCurvePV2012Blue",  NULL, look_start, &g->blue);

    if (look_start) {
        params = mem_find(text, len, "<crs:Parameters", look_start);
        if (params)
            read_curve(text, len, "crs:ToneCurvePV2012", params, NULL, &g->profile);
    }

    /* --- grain -------------------------------------------------------- */
    if (attr_float(text, len, "crs:GrainAmount",    &f)) info->grain_amount    = f;
    if (attr_float(text, len, "crs:GrainSize",      &f)) info->grain_size      = f;
    if (attr_float(text, len, "crs:GrainFrequency", &f)) info->grain_frequency = f;

    /* --- things we cannot honour, reported rather than ignored -------- */
    if (attr_float(text, len, "crs:Texture",   &f) && f != 0.0f) note_unsupported(info, "Texture");
    if (attr_float(text, len, "crs:Clarity2012", &f) && f != 0.0f) note_unsupported(info, "Clarity");
    if (attr_float(text, len, "crs:Dehaze",    &f) && f != 0.0f) note_unsupported(info, "Dehaze");
    if (attr_float(text, len, "crs:Sharpness", &f) && f != 0.0f) note_unsupported(info, "Sharpness");
    if (attr_float(text, len, "crs:LuminanceSmoothing", &f) && f != 0.0f) note_unsupported(info, "Luminance NR");
    if (attr_str(text, len, "crs:LensProfileEnable", buf, sizeof buf) && buf[0] == '1')
        note_unsupported(info, "Lens profile");
    if (attr_float(text, len, "crs:PostCropVignetteAmount", &f) && f != 0.0f)
        note_unsupported(info, "Post-crop vignette");
    if (attr_str(text, len, "crs:LookTable", buf, sizeof buf))
        note_unsupported(info, "the profile's own LookTable (Adobe ships it as a binary table, not in the sidecar)");

    return SE_OK;
}
