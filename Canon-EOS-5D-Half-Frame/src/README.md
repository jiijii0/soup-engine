# Source

All of it. The engine, the camera payload, the desktop tools, the
simulator and the film stocks.

## Layout

| | |
|:--|:--|
| `engine/` | the look, the stock format, the CR2 reader, the JPEG codec. Freestanding C99, integer only on every per-pixel path, no libc. The same files build for the camera and for the desktop. |
| `ml/payload/` | the camera side. `film.c` is the engine's life on the body: the picker, the watcher, the develop queue. `mlstub.S` is the 215 bytes that live inside Magic Lantern's program. |
| `host/` | `filmdev`, the desktop driver. Develops frames, builds contact sheets, writes cards. |
| `tools/` | the simulator, the cross-checks, the release script. |
| `stocks/` | the thirteen films, as plain text. |

## Building

```sh
make all        # the desktop tools
make test       # the host suite
make simtest    # the real payload against a mock camera, byte for byte
make arm        # check the engine still builds freestanding for ARM946E-S
make payload    # AUTOEXEC.BIN and SE.BIN for the card
```

You need `arm-none-eabi-gcc` for the camera build and nothing unusual
for the rest.

`make payload` also needs Magic Lantern's own `autoexec.bin`, the one
their installer put on your card, at `vendor/magiclantern-5Dc/`. It is
theirs to distribute so it is not included here, and `make payload`
stops with a note rather than guessing. Everything else builds without
it, and you do not need it to use the card files in `../card`.

`make simtest` is the one that matters. It runs the actual payload
against a mock of Canon's API and compares what the simulated camera
writes against a desktop reference, byte for byte. That equality is why
the look lives in one shared file rather than being written twice.

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
