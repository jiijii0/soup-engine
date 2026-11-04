# Fujichrome Velvia 50, as a look: the most saturated thing in the box.
#
# Velvia's whole reputation is colour past the point of plausibility, with
# blacks that go properly dense rather than grey. Steepening the curves
# raises apparent saturation, but only so far -- it also crushes the
# shadows and clips the highlights to get there, which is why this one used
# to read as "contrasty" rather than "saturated". SAT does the colour and
# the curve is left to do tone, which is the division of labour the two
# controls actually have.
#
# HUE_SAT is what makes it a landscape film rather than just a loud one:
# greens and blues get the extra push, yellows are held back so foliage
# does not go acid, and reds stay dense without going orange.
#
# Against PROVIA: that is the same manufacturer's honest film. These two
# should not be mistakable. Velvia is SAT 152 with a steep curve and a cool
# shadow; Provia is SAT 112, a gentle curve and no split at all.
VERSION   3
NAME      VELVIA
DESC      DENSE BLUE-GREENS, VIOLET SKIES, BLACK SHADOWS
WB        DAYLIGHT
CURVE     0,0 28,9 64,42 128,136 192,217 232,247 255,255
CURVE_R   0,0 64,63 128,136 192,202 255,255
CURVE_G   0,0 64,64 128,130 192,196 255,254
CURVE_B   0,4 64,60 128,120 192,184 255,248
SAT       126
HUE_SAT   112,96,120,114,122,103
HUE_ROT   -6,-10,14,8,-6,-2
HUE_LUM   98,92,80,84,74,96
HALATION  38 232 14
HAL_TINT  255,70,30
GRAIN     9
GRAIN_SIZE 10
