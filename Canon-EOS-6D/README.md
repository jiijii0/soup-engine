<div align="center">

# 🎞️ Soup Engine for the Canon EOS 6D <sub><sup>beta</sup></sub>

### Film looks, in the camera, on a Canon EOS 6D (the original, not the Mark II).

`beta` &nbsp;·&nbsp; `GPL v2` &nbsp;·&nbsp; `28 film stocks` &nbsp;·&nbsp; `EOS 6D` &nbsp;·&nbsp; `firmware 1.1.6` &nbsp;·&nbsp; `Magic Lantern 6D.116`

A community port by [@lbrock3](https://github.com/lbrock3).
<br>[← All cameras](../README.md#-pick-your-camera)

### 📦 [Download Canon-EOS-6D.zip](https://github.com/jiijii0/soup-engine/releases/download/folders/Canon-EOS-6D.zip)

</div>

> **Tested on one camera, by one owner.** On a 6D (firmware 1.1.6, Magic
> Lantern 6D.116) the module loads, INFO opens the film screen, the dial,
> cross keys and buttons work, all 28 films show on their shelves, and every
> shot gets its film print at Quarter, Half and Full size. Keep your raws, and
> do not rely on it for a shoot you cannot repeat.

## What this is

[Soup Engine](https://github.com/jiijii0/soup-engine) by @jiijii0 puts film
looks in the camera: every shot gets a second JPEG, `FLM_xxxx.JPG`, beside the
raw, with the look on it. The raw is never modified. The
[700D port](../Canon-EOS-700D-Rebel-T5i) by @GitariDev does this as an
ordinary Magic Lantern module, `soupeng.mo`. This folder is that port moved to
the 6D, which is also a DIGIC 5 body. The engine is unchanged.

## What you need

- A Canon EOS **6D** (the original; not the 6D Mark II) on Canon firmware **1.1.6**.
- [Magic Lantern's 6D build](https://builds.magiclantern.fm/6D.html), installed and working: the camera starts and Magic Lantern's menu opens.
- An SD card with Magic Lantern on it.
- Image quality set to **RAW** or **RAW+JPEG**. With JPEG only there is nothing to develop.

## Install

1. [Download Canon-EOS-6D.zip](https://github.com/jiijii0/soup-engine/releases/download/folders/Canon-EOS-6D.zip) and unzip it.
2. Copy the `ML` folder from `card/` onto the root of the card, merging it into the `ML` folder already there. It adds:

   | file | what it is |
   |:--|:--|
   | `ML/modules/soupeng.mo` | the module |
   | `ML/SETTINGS/soupeng.en` | loads the module at start-up |
   | `ML/DATA/SOUPUI.BIN` | the film screen's art (about 3 MB) |
   | `ML/FILM/*.fs` | the 28 films |

3. Eject the card properly, put it in the camera and switch on. If the module does not load, open Magic Lantern's **Modules** tab, turn on `soupeng` and restart.

Keep `ML/FILM` and `ML/DATA/SOUPUI.BIN` together, as they come in the zip.
A film whose name is not in `SOUPUI.BIN`, or a second file with the same
film name, shows a blank can on the film screen.

## Use

**Pick a film.** In shooting mode press **INFO**: the film screen is the first
stop of Canon's INFO cycle. Left and right, or the dial, move along the films;
up and down jump between shelves; **SET** loads the film. The shutter closes the
screen and still fires.

**Shoot.** With Auto develop on, every shot gets `FLM_xxxx.JPG` beside its raw,
in the film loaded. The film and the progress of a develop are shown at the
bottom right of the shooting screen; do not switch off while a frame is
developing.

**On the film screen:**

| button | does |
|:--|:--|
| PLAY | Auto develop on or off |
| Trash | size: Quarter (1368x912, about 10 s), Half (2736x1824), Full (5472x3648, a few minutes) |
| Q | develop the last shot |
| MENU | Home: Darkroom (frames waiting, raws with no print), Prints (view, redevelop, delete prints), Roll, System |

**Film sets ISO.** Loading a film sets the camera to its box speed, as loading
a roll would: Tri-X 400 at 400, Delta 3200 at 3200, Portra 160 at 160. Films
below ISO 100 (Velvia 50, Kodachrome 64, Classic Chrome) get 100; 5D Classic
leaves ISO alone. It acts only when a film is loaded, so an ISO you set
afterwards stays, and only in P, Tv, Av, M and A-DEP. Turn it off under
Shoot > Soup Engine > Film sets ISO. *This is new and has not been confirmed
on a camera yet.*

The same settings are in Magic Lantern's menu under **Shoot > Soup Engine**,
and `src/ml/6d/README.rst` describes every screen.

**Your own films.** Any `.fs` made with the
[Soup Stocks](https://jiijii0.github.io/soup-stocks/) editor can go in
`ML/FILM`, up to 32 films. A film the art pack does not know shows a blank can
but works.

## Remove

Delete `ML/modules/soupeng.mo` and `ML/SETTINGS/soupeng.en` from the card.
To remove everything, also delete `ML/DATA/SOUPUI.BIN` and the `ML/FILM`
folder. Your raws and prints stay on the card.

## The films

ACROS, ASTIA, BLEACH BYPASS, CLASSIC CHROME, CLASSIC NEG, DELTA 3200,
EKTACHROME, EKTAR 100, ETERNA, GOLD, HP5 PLUS, KODACHROME, NOSTALGIC NEG,
PORTRA, PORTRA 160, PORTRA 800, PRO 400H, PROVIA, REALA ACE, SOUP 800,
SUPERIA 400, T-MAX 400, TRI-X 400, ULTRAMAX 400, VELVIA, VISION3 250D,
VISION3 500T, and **5D CLASSIC**.

5D Classic is an approximation of the original Canon EOS 5D's colour and tone
(warm, rosy skin, soft contrast), judged by eye rather than measured against
5D files. Kodachrome, Ektachrome and Tri-X 400 were made for this port; the
rest are Soup Engine's and SoupDesk's stocks, unchanged.

## What changed from the 700D port

| where | change |
|:--|:--|
| `src/engine/cr2.c` | the 6D's sensor entry (white level and colour matrix from LibRaw). Models match exactly, not by prefix, so a 6D Mark II raw is never developed with 6D numbers. |
| `src/ml/6d/ml.h` | the 6D's button codes, from Magic Lantern's `platform/6D.116/gui.h`. |
| `src/ml/6d/screen.c` | the 6D's controller is 8-way and every direction is released with one code, so releases are paired through one slot; diagonals are ignored rather than closing the screen; the 700D's touch and zoom-out handling is gone. |
| `src/ml/6d/ui.c` | Canon sends the dial and cross keys to a module on the 6D only in GUI mode 2, the mode Magic Lantern's menu uses; the film screen switches to it while open. |
| `src/ml/6d/soupeng.c` | Film sets ISO. |
| `src/ml/6d/*` | size labels for 5472-wide frames; the watcher logs each develop step to `ML/LOGS/SOUPENG.LOG`; the plain film list scrolls. |
| `src/stocks/`, `src/ml/6d/data/SOUPUI.BIN` | 28 films, with the art pack regenerated so every film has its can. |
| `src/tools/sim6d/` | the simulator speaks the 6D's codes and develops a synthetic 5472x3648 "Canon EOS 6D" raw. |
| `src/host/main.c` | the card checker's macOS-only call is guarded, so the desktop tools build on Linux. |

## Building

```sh
cd src
make test               # the desktop engine's tests
make sim6d              # the module's real sources against a mock 6D
make card-6d            # soupeng.mo and the card files, in out/card-6d
```

`make card-6d` needs an Arm GNU toolchain and a Magic Lantern source checkout
in `src/vendor/` (not distributed here); see `src/ml/6d/Makefile`. The module
in `card/` was built with Arm GCC 15.2.1 against Magic Lantern commit
`ed3e7c0df`. At the time of this folder: `make test` all pass, and
`make sim6d` 208 checks, 0 failed.

## Known limits

- The art pack is about 3 MB and is read into memory when the film screen opens. If the camera cannot spare it, INFO shows a plain list of the films instead, and `ML/LOGS/SOUPENG.LOG` says why.
- Full-size develops take minutes. The camera restarted once during a first Full develop; later ones completed. `SOUPENG.LOG` logs progress every 5 seconds while a develop runs.

## Thanks

To @jiijii0 for Soup Engine, to @GitariDev for the 700D port this is adapted
from, and to Magic Lantern, whose work this runs inside. Please do not take
problems with this port to them.

Port by [@lbrock3](https://github.com/lbrock3).
GPL v2: see [LICENSE](LICENSE), including the no-warranty terms in sections 11 and 12.
