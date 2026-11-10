Soup Film
=========

Film looks for every new shot, developed in the camera.

After each frame, Soup Film runs the picture through the selected film
stock and writes FILM/xxxCANON/FLM_nnnn.JPG (outside DCIM), 2592x1728
from a full-size shot. In playback, SET shows the film version. The
original CR2 or JPG is never modified.

Shoot RAW or RAW+JPEG and it uses the full-size JPEG Canon embeds in the
CR2. Shoot JPEG only and it uses the camera JPEG.

Stocks live in ML/FILM on the card (INDEX.TXT plus .fs files), and can be
edited with the Soup Stocks editor. Choice and on/off are kept in ML/SE.CFG.

Beta. A port of Soup Engine (jiijii0) via the 50D integration (rebelt2i).
Not for paid or important work.

:Summary: Film looks for every new shot, developed in camera
:Author: kookueh (60D port of Soup Engine by jiijii0)
:License: GPL v2
:Website: https://github.com/jiijii0/soup-engine
