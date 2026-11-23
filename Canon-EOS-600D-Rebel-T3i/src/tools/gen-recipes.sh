#!/bin/sh
# Regenerate every recipe stock from src/recipes/*.recipe into src/stocks
# and list them in src/stocks/INDEX.TXT after the original stocks. Run
# build-60d.sh afterwards to refresh card/. Safe to re-run.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(dirname "$HERE")
ROOT=$(dirname "$SRC")
for r in "$SRC"/recipes/*.recipe; do
    n=$(basename "$r" .recipe)
    python3 "$HERE/recipe2fs.py" "$r" -o "$SRC/stocks/$n.fs" --stocks "$SRC/stocks"
    grep -qx "$n.fs" "$SRC/stocks/INDEX.TXT" || printf '%s\n' "$n.fs" >> "$SRC/stocks/INDEX.TXT"
done
