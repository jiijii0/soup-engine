#!/bin/sh
# Build soup.mo for the EOS 5D Mark II (firmware 2.1.2).
#
#   tools/build-5d2.sh /path/to/magiclantern_simplified
#
# Copies src/ml/5d2 into the Magic Lantern tree
# as modules/soup, builds Magic Lantern for 5D2.212 (needed for the symbol
# list modules are checked against), builds the module, then checks every
# symbol it imports is exported by the 5D2 build.
#
# Output: card/ in this folder, laid out as the card root
# (ML/modules/soup.mo, ML/SETTINGS/soup.en, ML/FILM/*). Magic Lantern
# itself comes from the official 5D2 build.
set -eu

ML=${1:?usage: build-5d2.sh /path/to/magiclantern_simplified}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(dirname "$HERE")
ROOT=$(dirname "$SRC")
MOD="$SRC/ml/5d2"
OUT="$ROOT/card"

rm -rf "$ML/modules/soup"
cp -r "$MOD" "$ML/modules/soup"

# Only the symbol list is needed, not the whole camera zip (which also
# wants `zip` installed): build/5D2_212.sym depends on magiclantern.sym.
make -s -C "$ML/platform/5D2.212" build/5D2_212.sym >/dev/null
make -s -C "$ML/modules/soup" SOUP_ENGINE="$SRC/engine" clean >/dev/null
make -C "$ML/modules/soup" SOUP_ENGINE="$SRC/engine"

SYM="$ML/platform/5D2.212/build/magiclantern.sym"
DEP="$ML/modules/soup/build/soup.dep"
missing=0
for s in $(cat "$DEP"); do
    if ! grep -q " $s\$" "$SYM"; then
        echo "MISSING on 5D2: $s"; missing=1
    fi
done
[ $missing -eq 0 ] || { echo "soup.mo imports symbols the 5D2 build does not export"; exit 1; }
echo "all $(wc -l < "$DEP") imported symbols are exported by 5D2.212"

rm -rf "$OUT"
mkdir -p "$OUT/ML/modules" "$OUT/ML/FILM" "$OUT/ML/SETTINGS"
cp "$ML/modules/soup/build/soup.mo" "$OUT/ML/modules/"
# Empty file: tells Magic Lantern to load the module at start-up (the same
# file its Modules tab writes when you mark soup for loading).
: > "$OUT/ML/SETTINGS/soup.en"
# The camera's file system only opens 8.3 names, so long stock file names
# are shortened here. The name shown in the menu comes from each file's NAME
# line, so nothing visible changes.
: > "$OUT/ML/FILM/INDEX.TXT"
while read -r f; do
    [ -n "$f" ] || continue
    base=${f%.fs}
    case $base in
        bleachbypass)  short=bleach ;;
        classicchrome) short=cchrome ;;
        classicneg)    short=cneg ;;
        nostalgicneg)  short=nneg ;;
        *)             short=$base ;;
    esac
    cp "$SRC/stocks/$f" "$OUT/ML/FILM/$short.fs"
    printf '%s.fs\n' "$short" >> "$OUT/ML/FILM/INDEX.TXT"
done < "$SRC/stocks/INDEX.TXT"

# Every file going on the card must be 8.3.
bad=$(cd "$OUT" && find . -type f | sed 's|.*/||' | awk -F. '{ if (length($1) > 8 || NF > 2 || (NF == 2 && length($2) > 3)) print }')
[ -z "$bad" ] || { echo "not 8.3: $bad"; exit 1; }
echo "card files are all 8.3"
ls -l "$OUT/ML/modules/soup.mo"
