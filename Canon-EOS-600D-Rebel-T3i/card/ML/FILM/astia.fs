# ASTIA -- the soft one, built for skin.
#
# Astia exists because Velvia is unusable on people. Lower contrast than
# Provia, gentle highlight roll-off, and -- the actual trick -- reds and
# oranges held DOWN while greens and blues keep their strength. That is
# what stops faces going hot while the rest of the frame stays alive.
#
# It should read as calm, not as weak. The saturation is near neutral; the
# softness is in the curve and in the hue balance.
VERSION   3
NAME      ASTIA
DESC      SOFT CONTRAST, CALM SKIN, CLEAN SLIDE BLACKS
WB        DAYLIGHT
CURVE     0,2 48,48 128,130 200,203 255,248
CURVE_R   0,2 64,67 128,131 255,251
CURVE_G   0,2 128,128 255,250
CURVE_B   0,3 128,129 255,249
SAT       96
HUE_SAT   100,100,94,96,102,94
HUE_ROT   3,1,-4,0,-3,-4
HUE_LUM   105,103,98,100,98,103
SPLIT_SH  8,4,0
SPLIT_HI  12,7,-4
HALATION  40 230 14
HAL_TINT  255,86,44
GRAIN     12
GRAIN_SIZE 14
