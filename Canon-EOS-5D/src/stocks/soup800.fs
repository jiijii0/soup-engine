# Soup 800: the halation stock.
#
# The look is motion-picture negative -- Kodak Vision3 500T -- with the
# remjet backing stripped off so it can go through ordinary C-41. Several
# respoolers sell exactly that, each under its own name and the speed it
# is rated at, which is where this one gets its own. Remjet is
# an anti-halation layer. Without it, light that reaches a highlight passes
# through the emulsion, scatters off the film base and comes back up through
# the emulsion again, exposing a red-orange halo around every bright thing
# in the frame. Street lights bloom. Windows bleed.
#
# That is the entire reason anyone shoots it, and it is a neighbourhood
# effect: a lookup table has no neighbours, so until engine/look.c grew a
# delay line this stock could not have existed here at all.
#
# The threshold is the whole difficulty. At 158 the glow spread across
# anything merely bright -- a white wall bloomed end to end and the frame
# came out washed pink. Halation is a CLIPPED-highlight effect: a street
# light, a window, a specular.
#
# Two things had to change in engine/look.c before this could bloom at
# all. The glow is a box blur, so what gets added is the MEAN of the mask
# over the window: a street light occupying a hundred pixels inside a
# 65x65 window averages away to nothing, and while HALATION was capped at
# 100 there was no way to ask for it back. Widening the radius made the
# bloom fainter rather than larger. 185 is a gain, and it is why this now
# glows where it used to just haze.
#
# The second is that the glow is weighted by how much headroom is left in
# the pixel it lands on. Halation is light that scattered off the base and
# exposed the emulsion twice; where the emulsion is already saturated the
# second exposure does nothing, so the halo only ever shows bleeding into
# something darker. Without that, gain made daylight unusable: a white
# building lit the whole mask and came back outlined in red.
#
# 228 with gain 185 is where a night street light blooms properly and a
# sunlit white wall still comes back white. Both were checked.
#
# The stock is tungsten balanced, so in daylight it goes blue. That lives in the
# curves and the shadow split rather than in a WB preset, for the reason
# cine.fs gives: a tungsten preset on a daylight scene is physically
# correct and creatively unusable. And it is an 800-speed motion picture
# stock pushed through a stills process, so the grain is coarse and big.
#
# The blue has to lean cyan, not violet. The hue sectors run R Y G C B M,
# so a POSITIVE rotation on the blue sector turns it toward magenta: this
# asked for +11 and came back purple, which is the one direction 800T
# never goes. Blue now rotates -16 toward cyan, magenta -24 behind it, and
# magenta's saturation drops to 78 so nothing drifts back. The shadow
# split had the same bug in RGB form -- green pulled DOWN while blue went
# up is a magenta cast by another name -- and is now -18,2,26.
VERSION   3
NAME      SOUP 800
DESC      RED HALATION, GLOWING BLUES, COARSE GRAIN
WB        AS_SHOT
CURVE     0,10 48,54 128,132 200,206 255,244
CURVE_R   0,5 64,69 128,133 192,197 255,250
CURVE_G   0,11 128,129 255,244
CURVE_B   0,20 64,84 128,144 192,201 255,252
SAT       102
HUE_SAT   110,92,78,124,132,78
HUE_ROT   9,-7,-18,6,-16,-24
HUE_LUM   100,95,84,106,114,102
SPLIT_SH  -18,2,26
SPLIT_HI  6,2,-2
HALATION  185 228 32
HAL_TINT  255,58,28
GRAIN     38
GRAIN_SIZE 40
