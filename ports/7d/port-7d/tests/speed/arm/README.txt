ARM instruction-count profiler: the camera's develop pipeline (same compiler and
flags as the module's engine objects) run under Unicorn, counted per function
and per source line, with a rough ARM946E-S cycle estimate. The Mac's own
timings do not show ARM9 costs (64-bit multiplies, aliasing reloads).

One-time:  python3 -m venv /tmp/uv && /tmp/uv/bin/pip install unicorn pyelftools
Input:     a 4:2:2 crop without restart markers, e.g.
           gcc -std=gnu99 -O1 -w -I../../../src/engine mkcrop.c ../../../src/engine/{se,jpeg_dec,jpeg_enc}.c -o /tmp/mkcrop
           /tmp/mkcrop <2592x1728 4:2:2 jpeg, e.g. streamcheck output> /tmp/crop.jpg
Build:     ./build.sh ../../../src/engine /tmp/new.elf      (TUNE=-mcpu=... to try other tuning)
Run:       /tmp/uv/bin/python run.py /tmp/new.elf /tmp/crop.jpg ../../../card/ML/FILM/tmax.fs label
Lines:     PROF_FN=jd_run /tmp/uv/bin/python run.py ... > /tmp/p.txt
           python3 group.py /tmp/p.txt jpeg_dec.c 297:360:idct 761:820:colour
Compare an old engine: git archive HEAD src/engine | tar -x -C /tmp/old --strip-components=2

idctcheck.c: the 14-multiply IDCT factoring against the plain form (20M random blocks).
graincheck.c: proves the grain clamp never triggers for grain 0..100.
