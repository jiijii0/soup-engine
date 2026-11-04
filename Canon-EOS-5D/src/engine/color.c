#include "color.h"
#include "se_libc.h"

/* sRGB primaries -> XYZ, D65. */
static const float g_srgb_to_xyz[9] = {
    0.412453f, 0.357580f, 0.180423f,
    0.212671f, 0.715160f, 0.072169f,
    0.019334f, 0.119193f, 0.950227f
};

static void mat3_mul(const float *a, const float *b, float *out)
{
    int i, j, k;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            float s = 0.0f;
            for (k = 0; k < 3; k++) s += a[i * 3 + k] * b[k * 3 + j];
            out[i * 3 + j] = s;
        }
}

static int mat3_inv(const float *m, float *out)
{
    float det =
        m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) +
        m[2] * (m[3] * m[7] - m[4] * m[6]);
    float id;
    if (det > -1e-9f && det < 1e-9f) return -1;
    id = 1.0f / det;
    out[0] =  (m[4] * m[8] - m[5] * m[7]) * id;
    out[1] = -(m[1] * m[8] - m[2] * m[7]) * id;
    out[2] =  (m[1] * m[5] - m[2] * m[4]) * id;
    out[3] = -(m[3] * m[8] - m[5] * m[6]) * id;
    out[4] =  (m[0] * m[8] - m[2] * m[6]) * id;
    out[5] = -(m[0] * m[5] - m[2] * m[3]) * id;
    out[6] =  (m[3] * m[7] - m[4] * m[6]) * id;
    out[7] = -(m[0] * m[7] - m[1] * m[6]) * id;
    out[8] =  (m[0] * m[4] - m[1] * m[3]) * id;
    return 0;
}

static float se_exp2f(float x)
{
    /* Small, dependency-free 2^x: the camera build has no libm. */
    int   n = (int)(x < 0 ? x - 0.5f : x + 0.5f);
    float f = x - (float)n;
    float r = 1.0f, p = 1.0f;
    int   i;
    const float ln2 = 0.69314718f;
    for (i = 1; i <= 7; i++) { p *= f * ln2 / (float)i; r += p; }
    while (n > 0) { r *= 2.0f; n--; }
    while (n < 0) { r *= 0.5f; n++; }
    return r;
}

se_err se_color_setup(se_color *c, const se_cr2_info *info,
                      const float *wb_override, float exposure_ev)
{
    float wb[4];
    float cam_from_srgb[9], srgb_from_cam[9];
    float gain;
    int   i, site;

    memset(c, 0, sizeof *c);
    c->white        = info->white;
    c->exposure_ev  = exposure_ev;

    for (i = 0; i < 4; i++) {
        c->black_cfa[i] = info->black_measured ? info->black_cfa[i] : info->black;
        wb[i] = wb_override ? wb_override[i] : info->wb[i];
        if (wb[i] <= 0.0f) wb[i] = 1024.0f;
    }

    /*
     * Normalise the white balance so the *smallest* multiplier is 1.0. That
     * keeps every channel >= unity, so conditioning never throws away
     * precision, and the headroom scale below handles the rest.
     */
    {
        float mn = wb[0];
        for (i = 1; i < 4; i++) if (wb[i] < mn) mn = wb[i];
        /* wb[] is R,G1,B,G2 as levels: a *larger* level means that channel
         * was weaker and needs a *larger* gain, so gains are wb[i]/min. */
        for (i = 0; i < 4; i++) wb[i] = wb[i] / mn;
    }

    /*
     * The gains above are normalised so the weakest is 1.0, and that is the
     * channel whose saturation defines white: a neutral highlight hits the
     * sensor's saturation point in every channel at once, by definition of
     * white balance. So map (white - black) to 65535 and let the channels
     * with larger gains clip there -- that is a genuinely blown highlight,
     * not a scaling error. Normalising by the largest gain instead would
     * darken the whole frame by the red/green ratio.
     */
    gain = 65535.0f / (float)(info->white - info->black);
    gain *= se_exp2f(exposure_ev);

    /* CFA site index is (y&1)*2 + (x&1); map that to the wb[] slot. */
    for (site = 0; site < 4; site++) {
        int sy = site >> 1, sx = site & 1;
        /* absolute raw position parity -> colour, raw origin is RGGB */
        int is_r = (!sy && !sx);
        int is_b = ( sy &&  sx);
        int wb_slot = is_r ? 0 : is_b ? 2 : (sy ? 3 : 1);
        float g = wb[wb_slot] * gain;
        c->cfa_chan[site] = is_r ? 0 : is_b ? 2 : 1;
        int   span = info->white - c->black_cfa[site];
        c->cfa_scale_q[site] = (int32_t)(g * (float)SE_QONE + 0.5f);
        c->clip_cfa[site]    = (int)SE_CLAMP((float)span * g, 0.0f, 65535.0f);
    }

    /* cam_from_srgb = (XYZ -> cam) . (sRGB -> XYZ) */
    if (!info->cam_xyz_present) {
        se_log("color: no camera matrix for '%s', passing raw RGB through",
               info->model);
        memset(c->cam_to_srgb, 0, sizeof c->cam_to_srgb);
        memset(c->cam_to_srgb_q, 0, sizeof c->cam_to_srgb_q);
        c->cam_to_srgb[0] = c->cam_to_srgb[4] = c->cam_to_srgb[8] = 1.0f;
        c->cam_to_srgb_q[0] = c->cam_to_srgb_q[4] = c->cam_to_srgb_q[8] = SE_QONE;
        return SE_OK;
    }

    mat3_mul(info->cam_xyz, g_srgb_to_xyz, cam_from_srgb);

    /*
     * Normalise each row to sum to 1 so that a neutral camera triple maps to
     * a neutral sRGB triple. Without this the matrix carries its own white
     * balance and fights the as-shot multipliers.
     */
    for (i = 0; i < 3; i++) {
        float s = cam_from_srgb[i * 3] + cam_from_srgb[i * 3 + 1] + cam_from_srgb[i * 3 + 2];
        if (s > 1e-6f || s < -1e-6f) {
            cam_from_srgb[i * 3 + 0] /= s;
            cam_from_srgb[i * 3 + 1] /= s;
            cam_from_srgb[i * 3 + 2] /= s;
        }
    }

    if (mat3_inv(cam_from_srgb, srgb_from_cam) != 0) {
        se_log("color: camera matrix is singular, passing raw RGB through");
        memset(c->cam_to_srgb, 0, sizeof c->cam_to_srgb);
        memset(c->cam_to_srgb_q, 0, sizeof c->cam_to_srgb_q);
        c->cam_to_srgb[0] = c->cam_to_srgb[4] = c->cam_to_srgb[8] = 1.0f;
        c->cam_to_srgb_q[0] = c->cam_to_srgb_q[4] = c->cam_to_srgb_q[8] = SE_QONE;
        return SE_OK;
    }
    memcpy(c->cam_to_srgb, srgb_from_cam, sizeof c->cam_to_srgb);
    for (i = 0; i < 9; i++)
        c->cam_to_srgb_q[i] = (int32_t)(c->cam_to_srgb[i] * (float)SE_QONE +
                                        (c->cam_to_srgb[i] < 0 ? -0.5f : 0.5f));
    return SE_OK;
}

void se_color_scale_row(const se_color *c, const uint16_t *src, uint16_t *dst,
                        int width, int x0, int y)
{
    int site_even = ((y & 1) << 1) | 0;
    int site_odd  = ((y & 1) << 1) | 1;
    int32_t ge = c->cfa_scale_q[site_even], go = c->cfa_scale_q[site_odd];
    int     be = c->black_cfa[site_even],   bo = c->black_cfa[site_odd];
    int     ce = c->clip_cfa[site_even],    co = c->clip_cfa[site_odd];
    const se_lut1 *te = NULL, *to = NULL;
    int     x;

    if (c->tone) {
        te = &c->tone[c->cfa_chan[site_even]];
        to = &c->tone[c->cfa_chan[site_odd]];
    }

    for (x = 0; x < width; x++) {
        int odd = (x0 + x) & 1;
        int32_t scale = odd ? go : ge;
        int     black = odd ? bo : be;
        int     clip  = odd ? co : ce;
        int     v = (int)src[x] - black;
        int32_t w;
        if (v < 0) v = 0;
        /* v <= 16383 and scale <= ~8 in Q14, so this stays inside int32. */
        w = (int32_t)(((int64_t)v * scale + (SE_QONE / 2)) >> SE_Q);
        if (w > clip) w = clip;
        /* the film stock, one lookup per sample */
        if (te) w = se_lut1_eval(odd ? to : te, (uint16_t)w);
        dst[x] = (uint16_t)w;
    }
}

void se_color_matrix_row(const se_color *c, uint16_t *rgb, int width)
{
    const int32_t *m = c->cam_to_srgb_q;
    int x;
    for (x = 0; x < width; x++) {
        int32_t r = rgb[x * 3 + 0];
        int32_t g = rgb[x * 3 + 1];
        int32_t b = rgb[x * 3 + 2];
        /* 16-bit inputs times Q14 coefficients overflow int32, so accumulate
         * in 64 bits; ARMv5TE does this with a single smlal per term. */
        int64_t R = (int64_t)m[0] * r + (int64_t)m[1] * g + (int64_t)m[2] * b;
        int64_t G = (int64_t)m[3] * r + (int64_t)m[4] * g + (int64_t)m[5] * b;
        int64_t B = (int64_t)m[6] * r + (int64_t)m[7] * g + (int64_t)m[8] * b;
        R >>= SE_Q; G >>= SE_Q; B >>= SE_Q;
        rgb[x * 3 + 0] = (uint16_t)SE_CLAMP(R, 0, 65535);
        rgb[x * 3 + 1] = (uint16_t)SE_CLAMP(G, 0, 65535);
        rgb[x * 3 + 2] = (uint16_t)SE_CLAMP(B, 0, 65535);
    }
}
