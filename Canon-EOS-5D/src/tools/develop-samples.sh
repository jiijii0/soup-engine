#!/bin/sh
# Every sample through every stock, by the path its folder asks for.
#
#   samples/fromJpeg/  -> out/previewcheck, scale 2
#   samples/fromRaw/   -> filmdev develop --half
#
# Both are the camera at SIZE HALF, but they are genuinely different
# pictures rather than two renderings of one:
#
#   The preview path grades the JPEG DIGIC already embedded in the CR2.
#   Everything applies: curves, SAT, the per-hue stage, split tone,
#   halation, grain. 1248x832 out.
#
#   The raw path rewrites mosaiced sensor samples before demosaic, so no
#   pixel has all three channels yet. Only curves, WB, exposure and grain
#   can apply -- SAT, HUE_*, SPLIT_* and HALATION are refused by
#   engine/stock.c, which says so when it reads the stock. Films come out
#   tamer and ACROS does not come out black and white. 2184x1456 out,
#   from more data.
#
# Verified against the camera: IMG_0486.FLM.JPG off the card is
# byte-for-byte identical to the preview path at scale 2 with gold.fs.
#
# Each photo also gets a _sheet.jpg of all thirteen. That is for choosing
# a look, never for judging one: filmdev contact downsamples every frame
# into a tile, and grain and halation are measured in output pixels.
set -eu

cd "$(dirname "$0")/.."

OUT=${1:-out/develop}
SCALE=${SCALE:-2}          # 2 = SIZE HALF, 1 = the preview's own size

[ -x out/previewcheck ] || make -s out/previewcheck
[ -x filmdev ] || { echo "build filmdev first: make all" >&2; exit 1; }

nstock=$(ls stocks/*.fs | wc -l | tr -d ' ')
njpeg=$(ls samples/fromJpeg/*.CR2 2>/dev/null | wc -l | tr -d ' ')
nraw=$(ls samples/fromRaw/*.CR2 2>/dev/null | wc -l | tr -d ' ')
total=$(( (njpeg + nraw) * nstock ))
n=0

for cr2 in samples/fromJpeg/*.CR2; do
    photo=$(basename "$cr2" .CR2)
    mkdir -p "$OUT/fromJpeg/$photo"
    for fs in stocks/*.fs; do
        stock=$(basename "$fs" .fs)
        n=$((n + 1))
        printf '\r  %d/%d  jpeg  %s / %s                 ' "$n" "$total" "$photo" "$stock"
        ./out/previewcheck "$cr2" "$fs" "$SCALE" "$OUT/fromJpeg/$photo/$stock.jpg" \
            >/dev/null 2>&1 || printf '\n  FAILED: %s / %s\n' "$photo" "$stock" >&2
    done
    ./filmdev contact "$cr2" "$OUT/fromJpeg/$photo/_sheet.jpg" >/dev/null 2>&1 || true
done

for cr2 in samples/fromRaw/*.CR2; do
    photo=$(basename "$cr2" .CR2)
    mkdir -p "$OUT/fromRaw/$photo"
    for fs in stocks/*.fs; do
        stock=$(basename "$fs" .fs)
        n=$((n + 1))
        printf '\r  %d/%d  raw   %s / %s                 ' "$n" "$total" "$photo" "$stock"
        ./filmdev develop --stock "$fs" --half -o "$OUT/fromRaw/$photo/$stock.jpg" "$cr2" \
            >/dev/null 2>&1 || printf '\n  FAILED: %s / %s\n' "$photo" "$stock" >&2
    done
    ./filmdev contact "$cr2" "$OUT/fromRaw/$photo/_sheet.jpg" --from-raw >/dev/null 2>&1 || true
done

printf '\r  %d renders into %s/                               \n' "$n" "$OUT"
du -sh "$OUT" | awk '{print "  " $1 " total"}'
