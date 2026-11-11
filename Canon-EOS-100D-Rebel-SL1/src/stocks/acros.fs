# ACROS -- black and white, and the reason SAT exists.
#
# This stock was impossible here until the look stage could do saturation.
# Desaturation is inherently cross-channel: you cannot reach grey with
# three independent curves, because none of them can see the other two.
# SAT 0 is the entire conversion.
#
# The per-channel curves still matter, and they are doing the job a
# coloured filter does on real monochrome film: they run BEFORE the
# desaturation, so pulling blue down and lifting red darkens a sky and
# opens skin, exactly like screwing a yellow filter onto the lens. That is
# also why this stock cannot work on the raw path -- there is no "the
# other two channels" before demosaic.
#
# Acros is famously fine-grained for a 100 speed film, with long, soft
# shadow detail under a hard shoulder.
VERSION   3
NAME      ACROS
DESC      BLACK AND WHITE, BLACK SHADOWS, HARD SHOULDER
WB        DAYLIGHT
CURVE     0,0 40,13 96,80 160,182 224,246 255,255
CURVE_R   0,0 64,78 128,150 192,212 255,255
CURVE_G   0,0 128,128 255,250
CURVE_B   0,0 64,44 128,98 192,164 255,222
SAT       0
HALATION  42 230 15
HAL_TINT  255,255,255
GRAIN     20
GRAIN_SIZE 16
