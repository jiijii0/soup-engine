# PORTRA 400 V2 -- a Fujifilm Recipe, translated by tools/recipe2fs.py.
#
# Recipe: Kodak Portra 400 v2 (X-Trans IV), by Ritchie Roesch / Fuji X Weekly.
# Source: https://fujixweekly.com/2022/12/16/kodak-portra-400-v2-fujifilm-x-t5-x-trans-v-film-simulation-recipe/
# Settings: classicchrome.fs base, WB 5200K +1 R -6 B, DR400, Highlight +0, Shadow -2,
#   Color +2, Clarity -2, Grain STRONG SMALL, CCE STRONG, CC FX Blue WEAK.
#
# Generated from the classicchrome.fs stock: its curves, composed with the
# recipe's white balance and tone settings, are folded into the three
# channel curves below. An approximation of the recipe, not a measured
# copy: Fuji's settings have no published numeric meaning, and this
# runs on a Canon JPEG. Edit recipes/*.recipe and regenerate, rather
# than editing this file.
VERSION   3
NAME      PORTRA 400 V2
DESC      GOLDEN-HOUR PORTRA, YELLOW-GREEN LEAN
WB        DAYLIGHT
CURVE_R   0,14 8,18 16,22 32,33 48,48 64,64 80,80 96,95 112,114 128,138 144,162 160,183 176,200 192,214 208,225 224,234 240,238 255,243
CURVE_G   0,15 8,19 16,24 32,34 48,49 64,64 80,79 96,93 112,110 128,131 144,152 160,170 176,186 192,202 208,216 224,227 240,231 255,237
CURVE_B   0,22 8,25 16,28 32,35 48,45 64,57 80,70 96,82 112,94 128,109 144,129 160,151 176,171 192,188 208,203 224,217 240,225 255,231
SAT       83
HUE_SAT   112,54,42,72,89,100
HUE_ROT   7,-12,-30,-24,-18,-5
HUE_LUM   95,83,72,84,82,90
SPLIT_SH  -13,-2,7
SPLIT_HI  8,4,-6
HALATION  36 232 13
HAL_TINT  255,80,44
GRAIN     26
GRAIN_SIZE 14
