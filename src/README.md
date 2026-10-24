# Source

All of it. The engine, the camera payload, the desktop tools, the
simulator and the film stocks.

## Layout

| | |
|:--|:--|
| `engine/` | the look, the stock format, the CR2 reader, the JPEG codec. Freestanding C99, integer only on every per-pixel path, no libc. The same files build for the camera and for the desktop. |
| `ml/payload/` | shared camera runtime. `film.c` is the engine's life on the body: the picker, the watcher, the develop queue. |
| `ml/platforms/` | one directory per body (`5DC.111`, `50D.109`, …): `cam.h`, `mlmap.h`, `payload.ld`, `mlstub.S`, and `patch.json`. |
| `host/` | `filmdev`, the desktop driver. Develops frames, builds contact sheets, writes cards. |
| `tools/` | the simulator, the cross-checks, the release script. |
| `stocks/` | the thirteen films, as plain text. |

## Platforms

`PLATFORM` selects the body. The original shipping body is the EOS 5D
Classic (`5DC.111`). This tree's Make default is still `50D.109` so local
50D work stays convenient; pass `PLATFORM=` explicitly when you mean the
other.

```sh
make payload PLATFORM=5DC.111   # original 5D Classic path
make payload PLATFORM=50D.109   # default in this tree
make simtest PLATFORM=5DC.111
```

Each platform directory holds the absolute addresses and Magic Lantern
patch holes for that body. Shared look code stays in `engine/`; shared
payload logic stays in `ml/payload/`. That split is what makes small
engine fixes upstreamable without dragging a whole second camera along.

Day-to-day install for each body is in the root [README](../README.md).

## Building

```sh
make all        # the desktop tools
make test       # the host suite
make simtest    # the real payload against a mock camera, byte for byte
make arm        # check the engine still builds freestanding for ARM946E-S
make payload    # AUTOEXEC.BIN and SE.BIN for the card (5D Classic path)
```

You need `arm-none-eabi-gcc` for the camera build and nothing unusual
for the rest.

`make payload` is the 5D Classic path. It needs Magic Lantern's own
`autoexec.bin` under `vendor/magiclantern-5Dc/`. That binary is theirs
to distribute, so it is not included here, and `make payload` stops
with a note rather than guessing.

The 50D does not use `make payload`. Build it with `tools/build-50d.py`,
described under 50D.109 below.

`make simtest` is the one that matters. It runs the actual payload
against a mock of Canon's API and compares what the simulated camera
writes against a desktop reference, byte for byte. That equality is why
the look lives in one shared file rather than being written twice.

## AUTOEXEC.BIN

### 5DC.111

Magic Lantern's program for the 5D Classic, with the loader and the
button hook written into padding gaps and the init-task pointer aimed at
`se_boot`. `tools/patch-film.py` applies that using
`ml/platforms/5DC.111/patch.json`, and recomputes the checksum so the
camera still accepts the file. The file stays the same length.

```sh
make payload PLATFORM=5DC.111
python3 tools/patch-film.py --platform 5DC.111
```

`make payload` needs Magic Lantern's `autoexec.bin` under
`vendor/magiclantern-5Dc/`. The patch writes `card/AUTOEXEC.BIN` and
`card/SE.BIN`; copy those with `ML/FILM/` to the CompactFlash card (or
use the release zip). Stub over `dump_edmac` at `0x00980820`, payload at
`0x009D0000` — see `ml/platforms/5DC.111/patch.json`.

`patch-film.py` is the Classic path only. Do not run it for the 50D.

### 50D.109

The body boots a Magic Lantern `autoexec.bin` rebuilt with the Film
page. It does not boot `SE.BIN`, and it does not survive the GUI hook
`patch-film.py` would plant at `0xB6964` / `0x60854`. Those addresses
and the reasons are in `ml/platforms/50D.109/PITFALLS.md`.

```sh
python3 tools/build-50d.py
python3 tools/build-50d.py --card /path/to/card
```

The script checks out Magic Lantern `ed3e7c0d`
(`magiclantern.2025-06-20.50D.109`) under `vendor/magiclantern/`,
copies `integrations/magiclantern/50D.109/` over that tree, and builds
`platform/50D.109`. The platform Makefile compiles `engine/` into the
same image. You need `arm-none-eabi-gcc` (`ARM_BINPATH`) and, on
Windows, Git's `usr\bin` on `PATH` so the Magic Lantern makefiles can
find `sh`.

Copy `build/autoexec.bin` to `AUTOEXEC.BIN` on the card and
`build/50D_109.sym` to `ML/modules/50D_109.sym` in the same breath.
Modules call addresses out of that sym file.

* Their source: <https://github.com/reticulatedpines/magiclantern_simplified>
* The 50D build this was made from: <https://builds.magiclantern.fm/50D.html>

## Licence

GPL v2. See [../LICENSE](../LICENSE).
