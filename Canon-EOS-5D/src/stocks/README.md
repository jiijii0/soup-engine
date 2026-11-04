# Film stocks

One file per stock. Drop them in `ML/FILM/` on the card; the camera lists
whatever it finds there, so adding a stock needs no rebuild.

A stock has two halves, and the split is not cosmetic — it is the
difference between the two develop paths.

### Always available

Per-channel tone response: a master curve, then a curve per channel, plus
white balance preset, exposure, black point, contrast, matte lift and
highlight rolloff. This is exactly the set of operations that can be
applied to **mosaiced** sensor data, where every sample is one known colour
and no pixel has all three — which is what lets DIGIC do the demosaic and
the JPEG at hardware speed.

Per-channel curves are how film actually works: three emulsion layers,
three characteristic curves. Most of a film look lives here. Steepening all
three *does* raise apparent saturation, and crossing the channels against
each other *does* shift colour.

| key | what it does |
|---|---|
| `CURVE`, `CURVE_R/G/B` | master then per-channel, points as `x,y` in 0–255 |
| `WB` | white balance preset — **raw path only**, see below |
| `EXPOSURE` | stops, applied in linear |
| `GRAIN`, `GRAIN_SIZE` | 0–100 amount, 0–100 cell size (1–9 px) |
| `[RAW]` | starts a block overriding the above for the sensor path only |

### JPEG path only

These need all three channels at one pixel, so they can only run on the
path that grades the full-size JPEG DIGIC already made. On the raw path
they are silently absent, because there is nothing there to apply them to.
A stock that leans on them looks different developed from raw, and that is
a property of the sensor data, not a bug.

| key | what it does |
|---|---|
| `SAT` | 100 = unchanged. Global saturation. |
| `HUE_SAT` | `r,y,g,c,b,m` — saturation per hue sector, 100 = unchanged, interpolated between neighbours. **This is what makes a film simulation rather than a tint.** |
| `SPLIT_SH`, `SPLIT_HI` | `r,g,b` signed, shadow tint weighted by (255−luma), highlight by luma |
| `HALATION` | `amount threshold radius` — the bloom around clipped highlights |
| `HAL_TINT` | `r,g,b` of the glow; defaults to the red-orange halation actually is |

### Different settings per source: `[RAW]`

The two paths do not see the same picture, so a stock can say what to do
differently when it is developed from the sensor data:

```
NAME      VELVIA
CURVE     0,0 28,9 64,42 ...     the JPEG look
SAT       150

[RAW]
CURVE     0,0 20,14 128,132 ...  gentler, and here is why
EXPOSURE  +0.15
GRAIN     12
```

Everything above a section header is the **JPEG look**, because that is
the path the camera uses unless told otherwise. `[RAW]` overrides it for
the sensor path only; `[JPEG]` returns to the common block.

**The rule of thumb is that `[RAW]` curves want to be gentler.** On the
JPEG path we grade a picture Canon has already put a Picture Style
through, so it arrives with contrast baked in. On the raw path our curve
goes on first and DIGIC applies its Picture Style *afterwards* — so a
curve that looks right on one will double up on the other.

Only the keys the raw path can physically honour are allowed there:
`CURVE`, `CURVE_R/G/B`, `EXPOSURE`, `WB`, `GRAIN`, `GRAIN_SIZE`.
`SAT`, `HUE_*`, `SPLIT_*` and `HALATION` are refused inside `[RAW]` with
a warning rather than silently ignored, so you don't spend an evening
tuning a number that cannot take effect.

Two things worth knowing:

- **`WB` does nothing on the JPEG path.** White balance is a pre-demosaic
  operation on raw samples. The preview path decodes an already-balanced
  JPEG, so a stock's colour cast has to live in its curves and splits.
- **`HALATION` costs memory and time.** It is the one spatial effect here,
  so it keeps a delay line of rows; the camera caps the radius to fit its
  budget and logs when it does. Only `soup800.fs` uses it.

## The set

Thirteen stocks: ten Fujifilm simulations from the current bodies, plus
three that are not Fuji and earn their place anyway.

| | character |
|---|---|
| `PROVIA` | the baseline. Natural slide film. |
| `VELVIA` | maximum saturation, dense blacks, greens and blues pushed |
| `ASTIA` | soft transparency, calm skin, real blacks |
| `CLASSIC CHROME` | muted documentary. Greens at 34 against reds at 116. |
| `CLASSIC NEG` | teal shadows, amber skin, punchy drugstore print |
| `NOSTALGIC NEG` | 70s American New Color: amber light, deep blues |
| `ETERNA` | flat cine negative, green-cyan shadows, waiting for a grade |
| `BLEACH BYPASS` | silver retained: near-grey with the steepest curve here |
| `ACROS` | black and white. `SAT 0`. |
| `REALA ACE` | true colour, firm blacks — the opinion is the contrast |
| `SOUP 800` | the halation stock, and the only one that uses it |
| `GOLD` | gold highlights over blue shadows |
| `PORTRA` | pastel negative, milky blacks, warm throughout |

Retired ones are in `retired/`, with a note on why.

## Keeping them apart

A set of looks is only worth having if the names mean something, so the
differences are measured rather than eyeballed: the mean per-pixel RGB
distance between every pair, computed separately over the coloured
subject and the background. Pairs that come out close are pairs that will
look the same on a real photograph.

That check found Gold and Nostalgic Neg 3.6 apart (both warm films),
Astia and Provia 4.2, and Classic Chrome only 6.7 from Provia when it is
supposed to be the muted one. The closest pair is 6.1 now, and it is
Astia against Provia — which is correct, because Astia *is* the soft
Provia.

## Why saturation alone never looks like film

Two things happen automatically to every stock, and neither is tunable,
because they are properties of emulsion rather than of a look:

- **Chroma rolls off toward white and toward black.** At the top all
  three dye layers run out together, so a bright colour desaturates on
  its way to white; at the bottom there is too little exposure left to
  hold a hue. Digital saturation does neither — it holds a colour at
  full strength right up to clipping, and that is the single clearest
  tell that something came from a slider.
- **Out-of-gamut colours lose chroma, not channels.** Clipping takes one
  channel to 0 or 255 and leaves the others, which gives the purest,
  most electric version of that hue with all detail gone. Film cannot do
  it: the dye layers overlap.

What this means when authoring: **`SAT` and `HUE_SAT` multiply**, so
`SAT 150` with `HUE_SAT 138` is 2.07× on that hue. Measured on real
foliage that came out at 2.1× and looked like neon. Velvia — the most
saturated film there is — sits at about **1.6×** now. Treat much past
that as a mistake.

Thresholds are worth tuning by eye. Halation should bloom from *clipped*
highlights — a street light, a window, a specular — not from anything
merely bright: at threshold 158 a white wall blooms end to end and the
frame comes out washed pink.

These are original designs, not measured emulations of the stocks they are
named after. `filmdev mkstock photo.xmp out.fs` extracts one from your own
Lightroom edit.
