# TRI-X 400 -- a Fujifilm Recipe, translated by tools/recipe2fs.py.
#
# Recipe: Kodak Tri-X 400, mid contrast (X-Trans IV), by Anders Lindborg, published by Ritchie Roesch / Fuji X Weekly.
# Source: https://fujixweekly.com/2020/06/18/fujifilm-x100v-film-simulation-recipe-kodak-tri-x-400
# Settings: acros.fs base, WB DAYLIGHT +9 R -9 B, DR200, Highlight +0, Shadow +3,
#   Color +0, Clarity +4, Grain STRONG LARGE, CCE STRONG, CC FX Blue OFF.
# The WB shift (R+9 B-9) acts as a warm contrast filter before the B&W conversion.
#
# Generated from the acros.fs stock: its curves, composed with the
# recipe's white balance and tone settings, are folded into the three
# channel curves below. An approximation of the recipe, not a measured
# copy: Fuji's settings have no published numeric meaning, and this
# runs on a Canon JPEG. Edit recipes/*.recipe and regenerate, rather
# than editing this file.
VERSION   3
NAME      TRI-X 400
DESC      GRITTY BLACK AND WHITE, OPEN MIDS, BIG GRAIN
WB        DAYLIGHT
CURVE_R   0,0 8,3 16,6 32,15 48,31 64,61 80,101 96,139 112,171 128,195 144,213 160,225 176,234 192,241 208,247 224,249 240,251 255,255
CURVE_G   0,0 8,2 16,3 32,7 48,15 64,26 80,43 96,69 112,101 128,132 144,160 160,181 176,199 192,215 208,230 224,241 240,244 255,250
CURVE_B   0,0 8,1 16,2 32,3 48,6 64,12 80,17 96,25 112,35 128,51 144,78 160,110 176,142 192,167 208,186 224,203 240,214 255,221
SAT       0
HUE_SAT   100,100,100,100,100,100
HUE_LUM   92,92,92,100,100,92
HALATION  42 230 15
HAL_TINT  255,255,255
GRAIN     26
GRAIN_SIZE 32
