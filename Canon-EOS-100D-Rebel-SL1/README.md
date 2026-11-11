<div align="center">

# 🎞️ Soup Engine for the Canon EOS 100D / Rebel SL1 <sub><sup>beta</sup></sub>

### Film looks, in the camera, on a Canon EOS 100D (Rebel SL1, Kiss X7).

`GPL v2` &nbsp;·&nbsp; `28 film stocks` &nbsp;·&nbsp; `firmware 1.0.1` &nbsp;·&nbsp; `Magic Lantern 100D.101`

### 📦 [Download Canon-EOS-100D-Rebel-SL1.zip](https://github.com/jiijii0/soup-engine/releases/download/folders/Canon-EOS-100D-Rebel-SL1.zip)

</div>

> **Tested on one camera, by one owner.** On an SL1 (firmware 1.0.1, Magic
> Lantern 100D.101) the module loads, INFO opens the film screen, and shots
> develop with the film loaded. The Q/SET button on Home was fixed after that
> first test (see below). Keep your raws, and do not rely on it for a shoot you
> cannot repeat.

## What this is

[Soup Engine](https://github.com/jiijii0/soup-engine) by @jiijii0 puts film
looks in the camera: every shot gets a second JPEG, `FLM_xxxx.JPG`, beside the
raw, with the look on it. The raw is never modified. The
[700D port](../Canon-EOS-700D-Rebel-T5i) by @GitariDev does this as an
ordinary Magic Lantern module, `soupeng.mo`. The 100D is the 700D's close
sibling (the same 18 MP sensor at 5184x3456, DIGIC 5), so this folder is that
port moved across, with the fixes found bringing it up on a 6D. The engine is
unchanged.

## What you need

- A Canon EOS **100D** / Rebel **SL1** / Kiss **X7** on Canon firmware **1.0.1**.
- [Magic Lantern's 100D build](https://builds.magiclantern.fm/100D.html), installed and working: the camera starts and Magic Lantern's menu opens.
- An SD card with Magic Lantern on it.
- Image quality set to **RAW** or **RAW+JPEG**. With JPEG only there is nothing to develop.

## Install

1. [Download Canon-EOS-100D-Rebel-SL1.zip](https://github.com/jiijii0/soup-engine/releases/download/folders/Canon-EOS-100D-Rebel-SL1.zip) and unzip it.
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
up and down jump between shelves; **Q/SET** loads the film. The shutter closes
the screen and still fires.

**Shoot.** With Auto develop on, every shot gets `FLM_xxxx.JPG` beside its raw,
in the film loaded. The film and the progress of a develop are shown on the
shooting screen; do not switch off while a frame is developing.

**On the film screen:**

| button | does |
|:--|:--|
| Q/SET | load the film (on Home: open) |
| PLAY | Auto develop on or off |
| Trash | size: Quarter (1296x864), Half (2592x1728), Full (5184x3456) |
| Av (+/-) | develop the last shot; on Home, develop all (Darkroom) or redevelop a print (Prints) |
| MENU | Home: Darkroom (frames waiting, raws with no print), Prints (view, redevelop, delete prints), Roll, System |

The same settings are in Magic Lantern's menu under **Shoot > Soup Engine**,
and `src/ml/100d/README.rst` describes every screen.

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

**5D Classic** approximates the original Canon EOS 5D's colour and tone: warm,
rosy skin, reds leaning slightly magenta, greens held back, soft blacks and a
smooth shoulder. It is judged by eye, not measured against 5D files. It is a
colour look only: full-frame field of view and depth of field come from the
sensor and lens, not from processing. Shoot in the Standard or Neutral Picture
Style, since the engine works from the camera's JPEG preview.

Kodachrome, Ektachrome and Tri-X 400 were made for these ports; the rest are
Soup Engine's and SoupDesk's stocks, unchanged.

## What changed from the 700D port

| where | change |
|:--|:--|
| `src/ml/100d/ml.h` | the 100D's button codes, from Magic Lantern's `platform/100D.101/gui.h`: the cross keys and touch codes are shifted from the 700D's. |
| `src/ml/100d/screen.c` | the 100D has one Q/SET button. On the shooting screen Magic Lantern times the press itself, so a module sees only the release: the release is taken as the click, and a long press counts once. "Last shot" and "Develop all" are on the Av (+/-) button, and the on-screen legend says so. |
| `src/engine/cr2.c` | the 100D / REBEL SL1 / Kiss X7 sensor entries (LibRaw); models match exactly, not by prefix. |
| `src/stocks/`, `src/ml/100d/data/SOUPUI.BIN` | 28 films, including 5D Classic, with the art pack regenerated so every film has its can. |
| `src/ml/100d/*` | from the 6D work: the watcher logs each develop step to `ML/LOGS/SOUPENG.LOG`; the plain film list scrolls; "TRI-X" keeps its hyphen. |
| `src/tools/sim100d/` | the simulator speaks the 100D's codes, including the Q/SET release timing. |

## Building

```sh
cd src
make test               # the desktop engine's tests
make sim100d            # the module's real sources against a mock 100D
make card-100d          # soupeng.mo and the card files, in out/card-100d
```

`make card-100d` needs an Arm GNU toolchain and a Magic Lantern source checkout
in `src/vendor/` (not distributed here); see `src/ml/100d/Makefile`. The module
in `card/` was built with Arm GCC 15.2.1. At the time of this folder: `make test`
all pass, and `make sim100d` 214 checks, 0 failed.

## Known limits

- The art pack is about 3 MB and is read into memory when the film screen opens. If the camera cannot spare it, INFO shows a plain list of the films instead, and `ML/LOGS/SOUPENG.LOG` says why.
- Develop times at Half and Full have not been measured on this body.

## Thanks

To @jiijii0 for Soup Engine, to @GitariDev for the 700D port this is adapted
from, and to Magic Lantern, whose work this runs inside. Please do not take
problems with this port to them.

Port by [@lbrock3](https://github.com/lbrock3).
GPL v2: see [LICENSE](LICENSE), including the no-warranty terms in sections 11 and 12.
