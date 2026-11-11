# VINTAGE KODACHROME -- a Fujifilm Recipe, translated by tools/recipe2fs.py.
#
# Recipe: Vintage Kodachrome (X-Trans III), by Ritchie Roesch / Fuji X Weekly.
# Source: https://fujixweekly.com/2017/10/21/my-fujifilm-x100f-vintage-kodachrome-film-simulation-recipe/
# Settings: classicchrome.fs base, WB AUTO +2 R -4 B, DR200, Highlight +4, Shadow -2,
#   Color +4, Clarity +0, Grain STRONG LARGE, CCE OFF, CC FX Blue OFF.
# X-Trans III recipe: no Color Chrome settings; "Grain Strong" read as Strong, Large.
#
# Generated from the classicchrome.fs stock: its curves, composed with the
# recipe's white balance and tone settings, are folded into the three
# channel curves below. An approximation of the recipe, not a measured
# copy: Fuji's settings have no published numeric meaning, and this
# runs on a Canon JPEG. Edit recipes/*.recipe and regenerate, rather
# than editing this file.
VERSION   3
NAME      VINTAGE KODACHROME
DESC      1935-60 KODACHROME, BRONZE SKIN, CYAN SKY
WB        DAYLIGHT
CURVE_R   0,14 8,18 16,23 32,34 48,50 64,66 80,81 96,98 112,120 128,148 144,180 160,207 176,225 192,234 208,240 224,243 240,244 255,247
CURVE_G   0,15 8,19 16,23 32,34 48,48 64,63 80,78 96,92 112,109 128,132 144,157 160,183 176,206 192,223 208,234 224,240 240,242 255,244
CURVE_B   0,22 8,25 16,28 32,35 48,46 64,59 80,71 96,83 112,96 128,113 144,136 160,163 176,190 192,212 208,226 224,235 240,239 255,242
SAT       92
HUE_SAT   112,54,42,72,86,100
HUE_ROT   7,-12,-30,-24,-18,-5
HUE_LUM   103,90,78,88,86,98
SPLIT_SH  -9,-2,10
SPLIT_HI  8,4,-6
HALATION  36 232 13
HAL_TINT  255,80,44
GRAIN     26
GRAIN_SIZE 32
