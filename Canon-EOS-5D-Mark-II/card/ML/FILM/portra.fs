# Kodak Portra 400: pastel, which is a thing curves cannot do.
#
# Portra is low-saturation portrait film -- creamy, slightly warm, with the
# blacks lifted well off zero so a print never looks harsh. "Pastel" means
# colours pulled TOWARD their own brightness, and a per-channel curve
# cannot do that: a curve on R has no idea what G and B are doing at that
# pixel. So this stock was, until there was a SAT control, just a warm
# low-contrast curve, which is not the same thing and never looked like it.
#
# SAT 82 is the whole character. The warm split on both ends is what keeps
# the desaturation from reading as grey.
VERSION   3
NAME      PORTRA
DESC      PASTEL, MILKY BLACKS, WARM SKIN, MUTED FOLIAGE
WB        AS_SHOT
CURVE     0,24 64,84 128,138 192,194 255,236
CURVE_R   0,16 64,86 128,143 192,203 255,250
CURVE_G   0,12 128,130 255,244
CURVE_B   0,10 64,66 128,124 192,186 255,238
SAT       86
HUE_SAT   96,112,78,100,104,100
HUE_ROT   7,3,-13,-5,-4,-4
HUE_LUM   105,107,93,98,96,103
SPLIT_SH  16,7,-4
SPLIT_HI  14,9,-7
HALATION  46 228 16
HAL_TINT  255,96,48
GRAIN     20
GRAIN_SIZE 36
