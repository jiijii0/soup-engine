#!/bin/bash
# Byte-exact output check for engine changes: 27 stocks x JPG/CR2 at HALF,
# FULL for 4 stocks, odd-size 4:2:0/4:4:4 JPEGs through decode+grade.
# Run it once on the unchanged engine to make a reference, then after the
# change, and diff the two files: any speedup must keep every hash.
# SAMPLES must hold _B7D0526.CR2/.JPG (any 7D RAW+JPEG L pair renamed) and
# speed-ref/: _B7D0002.JPG and odd61/odd1001/odd2593.jpg (sips-resized JPEGs).
# usage: SAMPLES=<dir> refrun.sh <streamcheck-binary> <outfile>   (dechash binary = same path with sc_ -> dh_)
B=$1; O=$2; S=$(mktemp -d); SM="${SAMPLES:?set SAMPLES}"; F="$(cd "$(dirname "$0")/../../card/ML/FILM" && pwd)"
: > $O
for st in "$F"/*.fs; do n=$(basename "$st" .fs)
  for sc in 2; do
    $B "$SM/speed-ref/_B7D0002.JPG" "$st" $S/o.jpg $sc jpg >/dev/null 2>&1; echo "$n jpg0002 s$sc $(shasum -a 256 $S/o.jpg | cut -c1-16)" >> $O
    $B "$SM/_B7D0526.CR2" "$st" $S/o.jpg $sc >/dev/null 2>&1; echo "$n cr2-0526 s$sc $(shasum -a 256 $S/o.jpg | cut -c1-16)" >> $O
  done
done
for st in gold tmax soup800 kodachr; do
  $B "$SM/_B7D0526.JPG" "$F/$st.fs" $S/o.jpg 1 jpg >/dev/null 2>&1; echo "$st jpg0526 s1 $(shasum -a 256 $S/o.jpg | cut -c1-16)" >> $O
done
D=${B/sc_/dh_}
for o in odd61 odd1001 odd2593; do for sc in 1 2; do for st in gold tmax soup800 kodachr velvia; do
  echo "dh $st $o s$sc $($D "$SM/speed-ref/$o.jpg" "$F/$st.fs" $sc)" >> $O
done; done; done
