# Kodachrome 64, as a look: warm, dense, and very specific about red.
#
# Kodachrome is not a saturated film in the Velvia sense. Its colour comes
# from dye layers that were dense and a little narrow, so what it does is
# pick certain colours out: reds are deep and a touch orange, yellows go to
# gold, blue skies are deep and lean toward cyan, and greens turn olive
# instead of going bright. Skin stays warm and natural. Blacks are properly
# black and the whites have a faint cream in them, not a clean white.
#
# So SAT is moderate (above Provia, well under Velvia) and the character is
# in HUE_SAT / HUE_ROT / HUE_LUM: greens are rotated toward yellow and held
# back, reds are pushed, blues are darkened. The curve is a firm S with
# real shadow density. The split tone adds the cream highlights and a very
# slight cool in the deepest shadows.
#
# Grain is fine: K64 was among the finest-grained films made.
VERSION   3
NAME      KODACHROME
DESC      DEEP REDS, GOLD WARMTH, OLIVE GREENS
WB        DAYLIGHT
CURVE     0,0 30,15 64,50 128,135 192,215 232,245 255,253
CURVE_R   0,0 64,66 128,134 192,200 255,255
CURVE_G   0,0 64,63 128,128 192,193 255,254
CURVE_B   0,2 64,59 128,121 192,184 255,246
SAT       114
HUE_SAT   126,110,88,98,112,100
HUE_ROT   3,-5,-12,2,-5,0
HUE_LUM   96,104,90,96,88,98
SPLIT_SH  -4,2,8
SPLIT_HI  12,6,-12
HALATION  26 236 10
HAL_TINT  255,80,40
GRAIN     7
GRAIN_SIZE 8
