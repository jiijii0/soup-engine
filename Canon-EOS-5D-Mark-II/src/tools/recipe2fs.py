#!/usr/bin/env python3
"""
recipe2fs.py - turn a Fujifilm film-simulation Recipe into a Soup Engine stock.

    python3 tools/recipe2fs.py recipes/kodachrome64.recipe -o stocks/kchrom64.fs

A Recipe (the Fuji X Weekly kind) is a base film simulation plus a handful of
camera settings. Soup Engine already has the base simulations as stocks, so a
Recipe becomes: that base stock, with the settings layered on in the order a
Fujifilm camera applies them:

    white balance (+ shift)  ->  film simulation  ->  tone (Highlight/Shadow,
    DR, Clarity)  ->  Color  ->  Color Chrome Effect / FX Blue  ->  grain

Everything is folded into the stock format: the base stock's master curve and
per-channel curves are composed with the white balance and the tone
adjustments into three new per-channel curves (master left as identity), and
the colour settings scale the base stock's SAT / HUE_SAT / HUE_LUM.

This is an APPROXIMATION. Fujifilm does not publish what +1 Highlight or one
step of WB shift does numerically, and these looks run on a Canon JPEG, not a
Fujifilm sensor. The constants below are reasoned estimates, chosen so the
recipes land in the right place relative to each other; they are kept in one
block so they can be tuned in one place.

GPL v2, same as Soup Engine.
"""
import argparse
import math
import os
import re
import sys

# --------------------------------------------------------------- constants
# White balance. One Fuji WB-shift step, as a linear gain on that channel.
# Calibrated on the recipes themselves: Vericolor III 160 sets 3200K in
# daylight and cancels most of that blue with R+7 B-8, which only works if a
# step is several percent. At 4% that recipe came out 1.5x blue.
WB_STEP = 0.06
# Fixed-Kelvin and Daylight recipes are assumed to be shot in daylight.
WB_DAYLIGHT_K = 5500.0
# Fuji's Kelvin setting is not a physical blackbody correction either; take
# this fraction of the physical ratio (in log terms).
KELVIN_DAMP = 0.65

# The base stock's shadow tint is added after the curves, so a WB shift on
# the curves alone never reaches the deep shadows. On the camera, white
# balance moves the whole frame: shift the shadow tint by this many levels
# per unit of log gain.
SPLIT_WB = 40.0

# Tone. One Highlight / Shadow step moves the curve this many 0..255 levels
# at the centre of its band. Positive Highlight = brighter highlights
# (harder shoulder); positive Shadow = darker shadows (harder toe).
TONE_STEP = 4.5
HI_CENTRE, HI_WIDTH = 0.80, 0.15
SH_CENTRE, SH_WIDTH = 0.22, 0.14
# Dynamic Range expands highlight latitude: read as a softer shoulder.
DR_HIGHLIGHT = {"100": 0.0, "AUTO": -0.25, "200": -0.5, "400": -1.0}
# Clarity is local contrast, which a per-pixel curve cannot do. Its global
# share is a little midtone contrast, this many levels per step at the
# quarter tones.
CLARITY_STEP = 1.2

# Color. Each step scales saturation by this fraction.
COLOR_STEP = 0.06
# Upstream's measured ceiling: SAT x HUE_SAT past ~1.6x looks like neon.
SAT_CEILING = 1.6

# Color Chrome Effect: deeper, denser saturated colours (all but blue/cyan).
CCE_LUM = {"OFF": 1.0, "WEAK": 0.96, "STRONG": 0.92}
# Color Chrome FX Blue: deeper blues.
CCFXB_LUM = {"OFF": 1.0, "WEAK": 0.95, "STRONG": 0.90}
CCFXB_SAT = {"OFF": 1.0, "WEAK": 1.04, "STRONG": 1.08}

# Grain Effect -> GRAIN / GRAIN_SIZE.
GRAIN_AMOUNT = {"OFF": 0, "WEAK": 14, "STRONG": 26}
GRAIN_SIZE = {"SMALL": 14, "LARGE": 32}

CURVE_X = [0, 8, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192,
           208, 224, 240, 255]

# ------------------------------------------------------------- colour math
def srgb_dec(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

def srgb_enc(v):
    v = min(max(v, 0.0), 1.0)
    return v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055

def blackbody_rgb(k):
    """Linear-light RGB of a blackbody, normalised to green. Tanner Helland's
    fit, which is plenty for a ratio between two temperatures."""
    t = k / 100.0
    r = 255.0 if t <= 66 else 329.698727446 * (t - 60) ** -0.1332047592
    g = (99.4708025861 * math.log(t) - 161.1195681661 if t <= 66
         else 288.1221695283 * (t - 60) ** -0.0755148492)
    b = (255.0 if t >= 66 else 0.0 if t <= 19
         else 138.5177312231 * math.log(t - 10) - 305.0447927307)
    rgb = [srgb_dec(min(max(c, 1.0), 255.0) / 255.0) for c in (r, g, b)]
    return [c / rgb[1] for c in rgb]

def wb_gains(wb, shift_r, shift_b):
    """Per-channel linear gains for a recipe's white balance on a picture
    the camera already balanced. Auto: only the shift. Daylight / Kelvin:
    the camera set to that temperature in daylight, plus the shift."""
    g = [1.0, 1.0, 1.0]
    wb = wb.upper()
    if wb != "AUTO":
        k = WB_DAYLIGHT_K if wb == "DAYLIGHT" else float(wb.rstrip("K"))
        ref, setk = blackbody_rgb(WB_DAYLIGHT_K), blackbody_rgb(k)
        g = [(ref[i] / setk[i]) ** KELVIN_DAMP for i in range(3)]
    g[0] *= math.exp(WB_STEP * shift_r)
    g[2] *= math.exp(WB_STEP * shift_b)
    return g

def tint(v, gain):
    """Apply a linear gain to an sRGB value, holding black and white: a tint,
    not an exposure change, so whites stay white the way a print's paper
    does instead of clipping one channel."""
    x = srgb_dec(v)
    y = gain * x / (1.0 + (gain - 1.0) * x)
    return srgb_enc(y)

# ------------------------------------------- the engine's curve, in Python
def curve_eval(pts, x):
    """Port of engine/grade.c se_curve_eval: monotone cubic Hermite
    (Fritsch-Carlson), points in 0..255."""
    n = len(pts)
    if n < 2:
        return x
    xs = [p[0] / 255.0 for p in pts]
    ys = [p[1] / 255.0 for p in pts]
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    d = [(ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]) if xs[i + 1] - xs[i] > 1e-6
         else 0.0 for i in range(n - 1)]
    m = [0.0] * n
    m[0], m[-1] = d[0], d[-2]
    for i in range(1, n - 1):
        m[i] = 0.0 if d[i - 1] * d[i] <= 0 else (d[i - 1] + d[i]) * 0.5
    for i in range(n - 1):
        if d[i] == 0.0:
            m[i] = m[i + 1] = 0.0
            continue
        a, b = m[i] / d[i], m[i + 1] / d[i]
        s = a * a + b * b
        if s > 9.0:
            f = 3.0 / math.sqrt(s)
            m[i], m[i + 1] = f * a * d[i], f * b * d[i]
    k = 0
    for i in range(n - 1):
        if xs[i] <= x <= xs[i + 1]:
            k = i
            break
    h = xs[k + 1] - xs[k]
    t = (x - xs[k]) / h if h > 1e-6 else 0.0
    t2, t3 = t * t, t * t * t
    return ((2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h * m[k]
            + (-2 * t3 + 3 * t2) * ys[k + 1] + (t3 - t2) * h * m[k + 1])

def bump(x, c, w):
    return math.exp(-((x - c) / w) ** 2)

def tone(v, hi, sh, clarity):
    """Highlight / Shadow / Clarity as a curve on the finished value. The
    bumps are pinned to zero at black and white."""
    d = (hi * TONE_STEP * bump(v, HI_CENTRE, HI_WIDTH)
         - sh * TONE_STEP * bump(v, SH_CENTRE, SH_WIDTH)) / 255.0
    d += clarity * CLARITY_STEP / 255.0 * math.sin(2 * math.pi * v) * -1.0 \
        * bump(v, 0.5, 0.3)
    pin = min(1.0, v * 12.0) * min(1.0, (1.0 - v) * 12.0)
    return v + d * pin

# ------------------------------------------------------------ stock files
CURVE_KEYS = ("CURVE", "CURVE_R", "CURVE_G", "CURVE_B")

def parse_points(s):
    return [tuple(int(v) for v in p.split(",")) for p in s.split()]

def read_stock(path):
    """Top-level (JPEG) keys of a .fs, plus the [RAW] block kept verbatim."""
    keys, order, raw = {}, [], []
    in_raw = False
    for line in open(path, encoding="utf-8"):
        s = line.split("#", 1)[0].rstrip()
        if not s.strip():
            continue
        if s.strip().upper() == "[RAW]":
            in_raw = True
            raw.append("[RAW]")
            continue
        if s.strip().upper() == "[JPEG]":
            in_raw = False
            continue
        if in_raw:
            raw.append(s.strip())
            continue
        k, _, v = s.strip().partition(" ")
        keys[k.upper()] = v.strip()
        order.append(k.upper())
    return keys, order, raw

def triple(s, n):
    return [float(v) for v in re.split(r"[,\s]+", s.strip()) if v][:n]

def read_recipe(path):
    r = {}
    for line in open(path, encoding="utf-8"):
        s = line.split("#", 1)[0].strip()
        if not s:
            continue
        k, _, v = s.partition(" ")
        r[k.upper()] = v.strip()
    return r

def fmt_curve(pts):
    return " ".join("%d,%d" % p for p in pts)

def translate(recipe_path, stocks_dir):
    r = read_recipe(recipe_path)
    base_path = os.path.join(stocks_dir, r["BASE"])
    keys, order, raw = read_stock(base_path)

    # --- curves: WB -> base master -> base channel -> tone ---------------
    shift = [float(v) for v in r.get("SHIFT", "0 0").split()]
    gains = wb_gains(r.get("WB", "AUTO"), shift[0], shift[1])
    hi = float(r.get("HIGHLIGHT", "0")) + DR_HIGHLIGHT[r.get("DR", "100").upper()]
    sh = float(r.get("SHADOW", "0"))
    clarity = float(r.get("CLARITY", "0"))
    master = parse_points(keys["CURVE"]) if "CURVE" in keys else []
    chans = [parse_points(keys[k]) if k in keys else [] for k in CURVE_KEYS[1:]]

    curves = []
    for c in range(3):
        pts, last = [], -1
        for x in CURVE_X:
            v = tint(x / 255.0, gains[c])
            v = curve_eval(master, v)
            v = curve_eval(chans[c], v)
            v = tone(min(max(v, 0.0), 1.0), hi, sh, clarity)
            y = int(round(min(max(v, 0.0), 1.0) * 255))
            y = max(y, last)            # keep it monotone after rounding
            pts.append((x, y))
            last = y
        curves.append(pts)

    # --- colour ----------------------------------------------------------
    sat = float(keys.get("SAT", "100"))
    hue_sat = triple(keys.get("HUE_SAT", "100,100,100,100,100,100"), 6)
    hue_lum = triple(keys.get("HUE_LUM", "100,100,100,100,100,100"), 6)
    color = float(r.get("COLOR", "0"))
    if sat > 0:
        sat *= 1.0 + COLOR_STEP * color
    cce = r.get("CCE", "OFF").upper()
    ccb = r.get("CCFXB", "OFF").upper()
    for i in (0, 1, 2, 5):                      # R Y G M
        hue_lum[i] *= CCE_LUM[cce]
    for i in (3, 4):                            # C B
        hue_lum[i] *= CCFXB_LUM[ccb]
    hue_sat[4] *= CCFXB_SAT[ccb]
    if sat > 0:
        peak = sat / 100.0 * max(hue_sat) / 100.0
        if peak > SAT_CEILING:
            sat *= SAT_CEILING / peak

    grain = r.get("GRAIN", "OFF").upper().split()
    g_amt = GRAIN_AMOUNT[grain[0]]
    g_size = GRAIN_SIZE[grain[1]] if len(grain) > 1 else GRAIN_SIZE["SMALL"]

    # --- write -----------------------------------------------------------
    out = []
    out.append("# %s -- a Fujifilm Recipe, translated by tools/recipe2fs.py." % r["NAME"])
    out.append("#")
    out.append("# Recipe: %s, by %s." % (r.get("RECIPE", r["NAME"]), r.get("CREDIT", "?")))
    out.append("# Source: %s" % r.get("SOURCE", "?"))
    out.append("# Settings: %s base, WB %s %+g R %+g B, DR%s, Highlight %+g, Shadow %+g,"
               % (r["BASE"], r.get("WB", "AUTO"), shift[0], shift[1],
                  r.get("DR", "100"), float(r.get("HIGHLIGHT", "0")), sh))
    out.append("#   Color %+g, Clarity %+g, Grain %s, CCE %s, CC FX Blue %s."
               % (color, clarity, r.get("GRAIN", "OFF"), cce, ccb))
    if r.get("NOTE"):
        out.append("# " + r["NOTE"])
    out.append("#")
    out.append("# Generated from the %s stock: its curves, composed with the" % r["BASE"])
    out.append("# recipe's white balance and tone settings, are folded into the three")
    out.append("# channel curves below. An approximation of the recipe, not a measured")
    out.append("# copy: Fuji's settings have no published numeric meaning, and this")
    out.append("# runs on a Canon JPEG. Edit recipes/*.recipe and regenerate, rather")
    out.append("# than editing this file.")
    out.append("VERSION   3")
    out.append("NAME      %s" % r["NAME"])
    out.append("DESC      %s" % r.get("DESC", r["NAME"])[:47])
    if "WB" in keys:
        out.append("WB        %s" % keys["WB"])
    out.append("CURVE_R   " + fmt_curve(curves[0]))
    out.append("CURVE_G   " + fmt_curve(curves[1]))
    out.append("CURVE_B   " + fmt_curve(curves[2]))
    out.append("SAT       %d" % round(sat))
    out.append("HUE_SAT   " + ",".join("%d" % round(v) for v in hue_sat))
    for k in ("HUE_ROT",):
        if k in keys:
            out.append("%-9s %s" % (k, keys[k]))
    out.append("HUE_LUM   " + ",".join("%d" % round(v) for v in hue_lum))
    sh_t = triple(keys.get("SPLIT_SH", "0,0,0"), 3)
    if sat > 0:      # a B&W stock takes the WB as a filter, not as a tint
        sh_t = [sh_t[i] + SPLIT_WB * math.log(gains[i]) for i in range(3)]
    if any(abs(v) >= 0.5 for v in sh_t):
        out.append("SPLIT_SH  " + ",".join("%d" % round(v) for v in sh_t))
    for k in ("SPLIT_HI", "HALATION", "HAL_TINT", "EXPOSURE"):
        if k in keys:
            out.append("%-9s %s" % (k, keys[k]))
    out.append("GRAIN     %d" % g_amt)
    out.append("GRAIN_SIZE %d" % g_size)
    if raw:
        out.append("")
        out.append("# The sensor path keeps the base simulation: the recipe's tone and")
        out.append("# colour settings are JPEG-path operations.")
        out.extend(raw)
    return "\n".join(out) + "\n", gains

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("recipe")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--stocks", default=os.path.join(os.path.dirname(__file__), "..", "stocks"))
    a = ap.parse_args()
    text, gains = translate(a.recipe, a.stocks)
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(text)
    print("%s -> %s  (WB gains R %.3f G %.3f B %.3f)" % (a.recipe, a.out, *gains))

if __name__ == "__main__":
    main()
