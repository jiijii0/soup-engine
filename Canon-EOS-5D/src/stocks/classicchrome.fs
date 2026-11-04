# CLASSIC CHROME -- the one that made people care about film simulations.
#
# It is a documentary look: muted, a little cold, with real tonal contrast
# underneath. The mistake is to read "muted" as "turn the saturation down",
# which gives you a flat grey picture. What Classic Chrome actually does is
# selective -- it drains the greens and yellows almost to nothing while
# letting reds keep their weight, so a red coat in a dull street still
# reads as red and the street stays dull.
#
# That is HUE_SAT, and it is the whole stock. A global SAT cannot say it
# (it moves every hue together) and per-channel curves cannot either (a
# curve on R has no idea what G and B are doing at that pixel). Greens at
# 52 against reds at 108 is a ratio of better than two to one.
VERSION   3
NAME      CLASSIC CHROME
DESC      OLIVE GREENS, CYAN BLUES, DENSE MUTED MIDS
WB        DAYLIGHT
CURVE     0,8 32,24 96,86 160,179 224,239 255,248
CURVE_R   0,5 64,66 128,132 192,199 255,251
CURVE_G   0,6 128,127 255,245
CURVE_B   0,12 64,67 128,124 192,186 255,240
SAT       74
HUE_SAT   112,54,42,72,86,100
HUE_ROT   7,-12,-30,-24,-18,-5
HUE_LUM   103,90,78,88,86,98
SPLIT_SH  -14,-2,20
SPLIT_HI  8,4,-6
HALATION  36 232 13
HAL_TINT  255,80,44
GRAIN     16
GRAIN_SIZE 20
