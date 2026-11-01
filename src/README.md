# Source

All of it. The engine, the camera payload, the desktop tools, the
simulator and the film stocks.

## Layout

| | |
|:--|:--|
| `engine/` | the look, the stock format, the CR2 reader, the JPEG codec. Freestanding C99, integer only on every per-pixel path, no libc. The same files build for the camera and for the desktop. |
| `ml/payload/` | the 5D Classic's camera side. `film.c` is the engine's life on the body: the picker, the watcher, the develop queue. `mlstub.S` is the 215 bytes that live inside Magic Lantern's program. |
| `ml/700d/` | the EOS 700D's camera side: an ordinary Magic Lantern module, `soupeng.mo`, built with their module build against a pinned checkout of their source. See below. |
| `host/` | `filmdev`, the desktop driver. Develops frames, builds contact sheets, writes cards. |
| `tools/` | the simulator, the cross-checks, the release script. |
| `stocks/` | the thirteen films, as plain text. |

## Building

The desktop side, which every camera shares:

```sh
make all        # the desktop tools
make test       # the host suite
make test-full  # the suite, the sample-driven checks, and a sanitiser build
make arm        # check the engine still builds freestanding for ARM946E-S
```

Nothing unusual is needed for those. The camera builds mostly want a
cross-compiler and something of Magic Lantern's, though `make sim700d`
needs neither:

```sh
make simtest    # 5D Classic: the real payload against a mock camera, byte for byte
make payload    # 5D Classic: AUTOEXEC.BIN and SE.BIN for the card
make sim700d    # EOS 700D: the real module against a mock camera, byte for byte
make card-700d  # EOS 700D: soupeng.mo and the card files, in out/card-700d
```

`make payload` needs Magic Lantern's own `autoexec.bin`, the one their
installer put on your card, at `vendor/magiclantern-5Dc/`; so does
`make simtest`. It is theirs to distribute so it is not included here,
and both stop with a note rather than guessing. `make card-700d` needs
their source instead, which `make vendor-700d` fetches (see below).
Nothing of theirs is committed here, and you do not need any of it to
use the card files in `../card`.

`make test-full` and `make simtest` use the raws in `samples/`, which are
the owner's photos and are not committed. `make sim700d` uses them too if
they are there. On a fresh clone they are not, so it runs on `out/public.CR2`,
which `tools/mkcr2.c` builds from one frame of `doc/stocks.jpg`: the same JPEG
path and the same checks, with no sensor data in the file.

The two sim targets are the ones that matter. Each runs the actual
camera code against a mock of the camera's API and compares what the
simulated camera writes against a desktop reference, byte for byte.
That equality is why the look lives in one shared file rather than
being written twice. On the 700D the same check runs against the real
camera too: `tools/verify-700d.py` re-renders every frame the camera
wrote from the frame's own EXIF and compares the files.

## The EOS 700D module

The 700D's Magic Lantern port has a module loader, so there is no
patching: `ml/700d/soupeng.c` is a normal module that asks Magic Lantern
for files, memory, tasks and the menu, and `ml/700d/Makefile` borrows
their module build. `make card-700d` needs `rst2html` from Python's docutils,
and two things outside this repository:

| | |
|:--|:--|
| `vendor/magiclantern_simplified/` | their source, checked out at the commit the camera's `autoexec.bin` was built from (`ed3e7c0df` for the 2025-06-20 700D build) plus `ml/700d/patches/`, small fixes to their build scripts: current Python, a module living outside their tree, and no user or host name written into a module. `make vendor-700d` clones, checks out and patches it; `vendor/` is ignored by git. |
| Arm's GNU toolchain | `arm-none-eabi-gcc` with newlib's headers, which Homebrew's package lacks. The Makefile looks for it at `~/.local/opt/arm-gnu-toolchain-15.3.rel2`; set `ARM_TOOLCHAIN` otherwise. It must live at a path without spaces, because their Makefile does not quote the compiler. |

```sh
make card-700d   # fetches their source the first time; soupeng.mo, the films,
                 # the art and the enable file, in out/card-700d/
```

A checkout from before the user-name fix makes the build stop, rather than
write your user name into the module. Delete
`vendor/magiclantern_simplified` and run it again.

The module is six files -- `soupeng.c` (films, menu, config, lifetime),
`develop.c` (one frame), `watch.c` (the queue and its tasks), `screen.c`
(the label and the keys), `ui.c` (the screens), `prints.c` (the previews) --
sharing `soupeng.h`,
and they reach Magic Lantern only through `ml.h`. `tools/sim700d/` supplies
a mock of that one header and runs the real six on the desktop: it
develops a frame and compares it with `previewcheck` byte for byte, then
drives the watcher through a burst, a half-written file and a power-off,
and the screen through its keys.

The film screen's art is `ml/700d/data/SOUPUI.BIN`, which `make card-700d`
copies to `ML/DATA` on the card. `tools/mkui.js` renders it from the design
in `doc/menu-sample/`, and runs only when the design changes, so a build never
does. It needs `playwright-core` from npm and a Chromium, neither of which is
in this repository:

```sh
npm i playwright-core                # anywhere; NODE_PATH points at it
npx playwright-core install chromium
NODE_PATH=<dir>/node_modules CHROME=<path to the chromium binary> node tools/mkui.js
```

A different Chromium may not give the same bytes. The pack that is committed
is the one that has run on the camera.

The pictures in `doc/screens/` come from the simulator, each screen as a PPM
from the real module through the real palette. Always use the public sample
for anything you publish, because with raws in `samples/` the default is your
own photo:

```sh
mkdir -p /tmp/screens
SE_SIM_DUMP=/tmp/screens make sim700d SIM700D_SAMPLE=out/public.CR2
```

Copy the contents of `out/card-700d` onto a card that already has
Magic Lantern's 700D build on it, eject it, and switch the camera on
with the card in. Soup Engine is under Shoot in Magic Lantern's menu.
On a Mac, copy with `COPYFILE_DISABLE=1 cp -R`, or run `dot_clean` afterwards.

What the camera writes is checked against the desktop the same way as
the 5D's: `tools/previewcheck <raw> <stock> <scale>` prints the hash
the module logs in `ML/LOGS/SOUPENG.LOG`, and the files have matched
byte for byte on every frame so far. The 700D writes `FLM_1234.JPG`
rather than `IMG_1234.FLM.JPG`, because its filesystem is 8.3.

## AUTOEXEC.BIN

Magic Lantern's program with 215 bytes changed. `tools/patch-film.py`
applies them to the `autoexec.bin` their installer put on your card:

```sh
python3 tools/patch-film.py          # reads vendor/magiclantern-5Dc/autoexec.bin
```

| | |
|:--|:--|
| `0x00980820` | `mlstub.S` goes here, over `dump_edmac`. That function is dead in the shipped binary: nothing in the relocated image branches into it or holds its address. 236 bytes available before Magic Lantern's veneer table, and the script refuses to exceed it. |
| `0x00989DB4` | one `bl` retargeted, so button presses reach Soup Engine first and tail-call onward if it doesn't want them |
| `0x00980774` | the init-task literal, so our loader runs. It runs Magic Lantern's own init first, then reads `SE.BIN`. |

The file stays exactly 221,184 bytes. Nothing is appended, the boot path
isn't touched, and no instruction of Magic Lantern's is rewritten. Their
`autoexec.bin` isn't included here because it's theirs to distribute, and
you already have it.

* Their source: <https://github.com/reticulatedpines/magiclantern_simplified>
* The build this was made from: <https://builds.magiclantern.fm/5DC.html>

## Licence

GPL v2. See [../LICENSE](../LICENSE).
