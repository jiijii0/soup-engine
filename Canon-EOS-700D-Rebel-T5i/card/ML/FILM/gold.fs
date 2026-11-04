# Kodak Gold 200: the gold tint, and the blue underneath it.
#
# What everyone remembers about Gold is the warmth. What makes it Gold
# rather than just "warm" is that the warmth is in the HIGHLIGHTS while the
# shadows go blue -- the two ends pull in opposite directions, and the
# picture reads as golden hour because of the contrast between them, not
# because everything is yellow.
#
# That is split toning, and it is not something per-channel curves can
# express: the tint has to depend on how bright the pixel is, which needs
# all three channels at once. CURVE_B is deliberately gentler than it was:
# with SPLIT_HI also pulling blue out of the highlights, the old curve
# doubled up and the whole frame went sepia instead of golden. The old version warmed the whole frame with a
# red lift and a blue pull, and came out looking like a tungsten white
# balance error. SPLIT_SH and SPLIT_HI are doing the work now.
VERSION   3
NAME      GOLD
DESC      GOLD HIGHLIGHTS, DARK BLUE SHADOWS, WARM MIDS
WB        CLOUDY
CURVE     0,12 64,74 128,136 192,199 255,249
CURVE_R   0,9 64,79 128,145 192,207 255,255
CURVE_G   0,5 128,131 255,250
CURVE_B   0,2 64,58 128,118 192,182 255,238
SAT       110
HUE_SAT   110,130,81,91,96,104
HUE_ROT   9,7,-15,-7,5,2
HUE_LUM   105,113,86,91,78,103
SPLIT_SH  -16,-4,26
SPLIT_HI  26,14,-22
HALATION  50 226 16
HAL_TINT  255,84,30
GRAIN     24
GRAIN_SIZE 30
