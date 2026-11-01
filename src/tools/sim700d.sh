#!/bin/sh
# Run the real ml/700d module in the simulator against a reference the
# desktop computed, byte for byte. tools/simtest.sh is the 5D's version.
#
#   sh tools/sim700d.sh <sample.CR2>
#
# Builds a card from the sample and the films, renders the reference with
# previewcheck exactly as the module will (the remembered film, quarter
# size, 4:2:0), then runs out/sim700d, which checks everything else.
set -e
SAMPLE="$1"
CARD=out/sim700d-card
[ -f "$SAMPLE" ] || { echo "sim700d: no sample raw"; exit 2; }

rm -rf "$CARD"
mkdir -p "$CARD/ML/FILM" "$CARD/ML/LOGS" "$CARD/ML/DATA" "$CARD/DCIM/100CANON"
cp stocks/*.fs "$CARD/ML/FILM/"
cp ml/700d/data/SOUPUI.BIN "$CARD/ML/DATA/"
rm -rf "$CARD/ML/PRINTS"
cp "$SAMPLE" "$CARD/sample.CR2"

# The module boots on the first film by name order when nothing is
# remembered, which is ACROS. Quarter size, 4:2:0: what the module writes.
FIRST=$(ls stocks/*.fs | sort | head -1)
./out/previewcheck "$SAMPLE" "$FIRST" 4 out/sim700d-ref.jpg 420 | grep "look hash"

for short in 1 0; do
  rm -f "$CARD"/DCIM/100CANON/*
  SE_SIM_SHORT_READS=$short ./out/sim700d "$CARD" out/sim700d-ref.jpg > out/sim700d-run.txt 2>&1 \
    && rc=0 || rc=$?
  grep -E "^  (ok|FAIL)|^sim700d:" out/sim700d-run.txt
  [ $rc = 0 ] || { echo "sim700d: FAILED with short_reads=$short (see out/sim700d-run.txt)"; exit 1; }
done
echo "sim700d: the module writes what the desktop computes"
