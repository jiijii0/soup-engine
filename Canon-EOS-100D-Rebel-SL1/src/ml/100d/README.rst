Soup Engine
===========

Film looks, in the camera. Put the films (the ``.fs`` files) in
``ML/FILM`` on the card and pick one under Shoot > Soup Engine. Every
shot after that gets a second JPEG beside the raw, ``FLM_1234.JPG``,
with the look on it. The raw is never modified.

Film screen
-----------

In shooting mode press INFO: the film screen is the first stop of
Canon's INFO cycle.

Left and right, or the dial, move along the films. Up and down jump
between shelves. SET loads the film.

PLAY turns Auto develop on or off: bottom left of the screen.
Trash steps Quality: bottom right. Av (+/-) develops the last shot.
On this body Q and SET are one button; on the film screen it is SET.

The number at the top is how many frames are waiting to develop.

Leaving it
----------

INFO goes on to Canon's next screen. The shutter closes it and still
fires. It closes by itself after 20 seconds, which System can change.

With auto off, shoot and look first, then develop the shot you want.

The art is ML/DATA/SOUPUI.BIN. Without that file, or without the
memory for it, INFO opens a plain list of the films instead.

Home
----

MENU on the film screen opens Home. Up and down move, SET opens, MENU
goes back a step, and Trash closes. PLAY on Home opens Prints.

Darkroom lists frames developing and raws with no print. SET develops
one, Av (+/-) all of them.

Prints shows what is on the card. SET opens one, left and right flip,
Av (+/-) redevelops it in the film in the camera, and Trash deletes the print
after asking. The raw stays.

Roll counts your shots. It has no size, and a new film starts a new one.

Shooting screen
---------------

The current film is shown at the bottom right, with a red ML BRACKET
ON above it if Advanced Bracket is on in this Shoot menu: that makes
every shutter press a sequence. While a frame develops the same line
shows its progress and how many frames wait behind it; do not switch
off until it is clear.

Sizes
-----

Quarter (1296x864) takes about 10 seconds. Half (2592x1728) about 40.
Full (18 MP) about three minutes. Keep shooting while it works: frames
queue, and the camera will not power off with frames waiting. A frame
keeps the film and size it was shot with, even if you change them
while it waits.

Cards
-----

Always eject the card from the computer before removing it, or the
camera may not start with it until it is ejected cleanly again.

:Author: luigi manga (engine); 700D port by GitariDev; 100D (Rebel SL1) adaptation of that port
:License: GPL v2
:Summary: Film looks in the camera. A graded JPEG beside every raw.
:Website: https://github.com/jiijii0/soup-engine
