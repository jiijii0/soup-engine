# usage: build_dh.sh <output-binary>   (run from soup-7d/port-7d/tests)
E=../../src/engine
gcc -std=gnu99 -O2 -I"$E" speed/dechash.c "$E"/se.c "$E"/jpeg_dec.c "$E"/look.c "$E"/stock.c "$E"/wb.c "$E"/color.c "$E"/grade.c "$E"/lut.c -lm -o $1
