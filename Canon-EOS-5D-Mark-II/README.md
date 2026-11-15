<div align="center">

# 🎞️ Soup Engine for the Canon EOS 5D Mark II <sub><sup>beta</sup></sub>

### Film looks, in the camera, on a Canon EOS 5D Mark II.

<br>

`beta` &nbsp;·&nbsp; `GPL v2` &nbsp;·&nbsp; `21 film stocks` &nbsp;·&nbsp; `EOS 5D Mark II` &nbsp;·&nbsp; `firmware 2.1.2`

A community port, adapted from the [60D port](../Canon-EOS-60D) by [@kookueh](https://github.com/kookueh).
<br>[← All cameras](../README.md#-pick-your-camera)

</div>

<br>

> 🚧 **Early.** This folder is the 60D module moved to the 5D Mark II.
> It has developed its first frame on a 5D Mark II (one camera, one
> owner, JPEG shooting), but it is far from tested. Don't use it on a
> shoot you can't repeat. See [Status](#-status) below.

<br>

Press **SET**, pick a film, shoot. Every photo you take while Film is on
gets a second JPEG with the look already on it, made in the camera
shortly after the shot.

```
DCIM/100CANON/IMG_1234.CR2    your photo (RAW or JPEG), untouched
FILM/100CANON/FLM_1234.JPG    the same photo with the look on it
```

> ⚖️ **No warranty.** Like all GPL software this comes with none. You
> install it at your own risk. See sections 11 and 12 of the
> [licence](LICENSE).

> 🎨 **The looks are still being tweaked.** The names are shorthand for
> the films and recipes they're based on, not exact copies. All
> trademarks belong to their owners.

<br>

---

<br>

## 🚧 Status

| | |
|:--|:--|
| Module source | adapted from the 60D module, see [What changed from the 60D port](#-what-changed-from-the-60d-port) |
| Builds against ML `5D2.212` | yes: `soup.mo` 107,244 bytes; all 84 functions it imports are exported by `5D2.212`; relocations only `R_ARM_ABS32` / `R_ARM_CALL` / `R_ARM_JUMP24`; no `%c` in its strings |
| Loads on a 5D Mark II | yes, first try (2026-11-15): reads all 21 stocks, finds the CF card as `A:`. Nothing was developed then: Film was left OFF, and the card's folder was `108_FUJI`, which the module couldn't find (fixed since, see below) |
| Finds the real DCIM folder | yes: `dcim_test.c` passes 16 checks, and on the camera the module found `A:/DCIM/108_FUJI` where Magic Lantern said `108EOS5D` |
| Develops on a desktop | yes: the module's own `soup_dev.c` (`host_test.c`) on a synthetic 5616×3744 "Canon EOS 5D Mark II" CR2 gives a 2800×1872 film JPEG with Velvia, Portra, Acros and Soup 800; peak memory about 1.5 MB, nothing leaked |
| Develops a frame on the camera | yes (2026-11-15): a JPEG L shot (5616×3744, 8.2 MB) became `FILM/108_FUJI/FLM_8661.JPG`, 2800×1872, 701 KB, Classic Chrome, in **68 s**, EXIF model and stock name correct. RAW and RAW+JPEG not tried yet |
| Faster engine | the 7D port's decoder, encoder and look code (see below): byte-identical output on all 21 stocks. On the camera: **45–46 s** a JPEG L frame, Classic Chrome (was 68 s) |
| Card reads | the source is now read 256 KB at a time (34 card reads for a JPEG L, was about 1,000 of 8 KB), same bytes out. `SOUP.LOG` gives the card's share of each develop (`card: read N ms ...`). Not yet timed on the camera |
| Playback toggle (SET in playback) | **off**: needs the 5D2's playback addresses, see below |
| Picker on the 4:3 screen | expected to work, about 11% narrower than drawn |

<br>

## ✨ About this port

Everything the 60D port does, on the 5D Mark II:

- **A Magic Lantern module.** It runs as `soup.mo` next to the official
  Magic Lantern 5D Mark II build. Magic Lantern itself is not changed.
- **The Soup Engine picker**, opened with SET on the shooting screen, with
  the **DEVELOPING** and **DEVELOPED** pages.
- **A small progress badge** on the shooting screen while frames develop.
- **RAW, RAW+JPEG and JPEG-only.** From a CR2 it develops the full-size
  JPEG Canon embeds in it (5616×3744), at half size: 2800×1872 (half is 2808 wide; the JPEG encoder trims to a multiple of 16).
- **Only shots taken while Film is on are developed.** Photos already on
  the card are left alone.
- **Film files live outside DCIM**, so Canon's playback is never confused
  by them.

<br>

## 📦 Installing

#### 1️⃣ &nbsp; Install Magic Lantern

You need a **Canon EOS 5D Mark II on firmware 2.1.2**, the only 5D Mark II
firmware Magic Lantern supports. Install the official Magic Lantern build
first, following their instructions:

> ### 👉 [builds.magiclantern.fm/5D2.html](https://builds.magiclantern.fm/5D2.html)

This port is built against `magiclantern.2025-06-20.5D2.212`, Magic
Lantern commit `ed3e7c0`, the same commit as the 60D and 50D ports.

#### 2️⃣ &nbsp; Get the module

[`card/`](card) has a `soup.mo` built from this folder with Arm GCC
13.2.1 (Ubuntu's `gcc-arm-none-eabi`) against Magic Lantern `ed3e7c0`. Or
build it yourself from [`src/`](src) (see [`src/README.md`](src/README.md));
the build writes it to `card/ML/modules/soup.mo`.

#### 3️⃣ &nbsp; Copy the files to the card

Camera off, card in your computer. Copy the `ML` folder from `card/` to
the root of the CompactFlash card, merging it into the `ML` folder Magic
Lantern put there.

| file | what it is |
|:--|:--|
| ⚙️ `ML/modules/soup.mo` | the module |
| ✅ `ML/SETTINGS/soup.en` | an empty file that tells Magic Lantern to load it |
| 🎞️ `ML/FILM/` | `INDEX.TXT` and the 21 film stocks |

The module creates the `FILM` folder at the top of the card itself, and
keeps its settings in `ML/SE.CFG` and `ML/SE.NXT`.

#### ✅ &nbsp; Check it

Press **SET** on the shooting screen. The film picker should fill the
back screen. If SET does nothing, check that **C.Fn IV-3 Assign SET
button** is on **0: Normal (disabled)**, or open the picker from Magic
Lantern's menu: **Shoot → Soup Film**, then SET.

<br>

## 📷 Using it

| Where | Button | What it does |
| --- | --- | --- |
| Shooting screen | **SET** | open the film picker |
| Picker | **main dial**, **Quick Control dial** or **joystick ◀ ▶** | browse the films |
| Picker | **SET** | choose this film (also turns Film on) |
| Picker | **Trash** or **MENU** | close |
| Picker | **half-press** | close and shoot |

The same settings are in the Magic Lantern menu under **Shoot → Soup
Film**: Film on/off, Stock, and a Status line.

While a frame develops, a badge at the top right of the shooting screen
shows progress (**+2** means two more frames are waiting), then **FILM
DONE** or **FILM FAILED** for a moment.

### What lands on the card

| File | What it is |
| --- | --- |
| `DCIM/100CANON/IMG_1234.CR2` / `.JPG` | your photo, never changed |
| `FILM/100CANON/FLM_1234.JPG` | the film version: 2800×1872 from a full-size shot, half size from smaller ones |
| `ML/SE.CFG` | Film on/off and the chosen stock |
| `ML/SE.NXT` | where Soup got to, so it can catch up after a restart |
| `ML/LOGS/SOUP.LOG` | one line per develop, with the stock used |

Photo import apps that only look in `DCIM` won't pick up the film files,
so copy the `FILM` folder across yourself.

<br>

## 🎞️ The films

They're small text files in `ML/FILM` on the card, so you can read them,
change them, or make your own with the
[Soup Stocks](https://jiijii0.github.io/soup-stocks/) editor.

Thirteen stocks come from Soup Engine. Eight (marked 🍳) are the 60D
port's recipes from [Fuji X Weekly](https://fujixweekly.com), translated
into stocks by `recipe2fs.py`. These are approximations: Fujifilm doesn't
publish what its settings do numerically, and here they run on a Canon
JPEG.

| Film | Look |
| --- | --- |
| Acros | black and white, black shadows, hard shoulder |
| Astia | soft contrast, calm skin, clean slide blacks |
| Bleach Bypass | silver retained, crushed blacks, near grey |
| Classic Chrome | olive greens, cyan blues, dense muted mids |
| Classic Neg | teal shadows, amber skin, olive foliage |
| Eterna | flat cinema negative, green-cyan shadows |
| Gold | gold highlights, dark blue shadows, warm mids |
| Nostalgic Neg | amber light, dense violet blues, open shadows |
| Portra | pastel, milky blacks, warm skin, muted foliage |
| Provia | natural slide film, clean whites, real blacks |
| Reala Ace | true colour, hard blacks, snappy shoulder |
| Soup 800 | red halation, glowing blues, coarse grain |
| Velvia | dense blue-greens, violet skies, black shadows |
| 🍳 Reggie's Portra | warm soft Portra, kind skin, muted greens |
| 🍳 Portra 400 v2 | golden-hour Portra, yellow-green lean |
| 🍳 Kodachrome 64 | rich reds, deep skies, crisp slide contrast |
| 🍳 Vintage Kodachrome | 1935–60 Kodachrome, bronze skin, cyan sky |
| 🍳 Pacific Blues | beach negative, cyan sea, coarse grain |
| 🍳 Reggie's Superia | moody Fujicolor negative, green tint, grainy |
| 🍳 Vericolor III 160 | 80s portrait negative, soft and gentle |
| 🍳 Tri-X 400 | gritty black and white, open mids, big grain |

<br>

## 🧹 Removing it

Delete `ML/modules/soup.mo` and `ML/SETTINGS/soup.en`, or turn it off in
Magic Lantern's **Modules** tab and restart. Your photos and film files
stay where they are. If the camera ever hangs, take the battery out.

<br>

## ⚠️ Known limits

- **Photos in a folder another camera made** (say `DCIM/108_FUJI` on a
  card that was in a Fujifilm) are found: the module lists `DCIM` and
  uses the folder with the current number that holds your latest shot.
  The film files mirror its name, `FILM/108_FUJI/`. If a frame really
  can't be found, `ML/LOGS/SOUP.LOG` says `nnnn: no CR2 or JPG in ...,
  skipped`.
- **Firmware 2.1.2 only**, because that's the only firmware Magic Lantern
  supports on the 5D Mark II.
- **The playback toggle is off.** On the 60D, SET in playback swaps the
  photo for its film version. To know which photo is showing, the module
  reads two of Canon's own variables, and their addresses are different
  on every body. The 5D Mark II's haven't been found yet, so SET in
  playback says **FILM: CAN'T TELL WHICH PHOTO THIS IS**. Finding them
  means stepping through playback in
  [qemu-eos](https://github.com/reticulatedpines/qemu-eos) with a 5D2 ROM
  dump. The two addresses go in `soup_play_current()` in
  [`src/ml/5d2/soup.c`](src/ml/5d2/soup.c).
- **SET in playback belongs to Soup** while the module is loaded, so Magic
  Lantern's "SET + dial" playback shortcuts won't see it.
- **The screen is 4:3.** The picker was laid out for the 60D's 3:2 screen,
  so on the 5D Mark II it shows about 11% narrower than drawn.
- **The picker doesn't open in Live View**, and nothing is developed while
  Live View is set to movies.

<br>

## 🔁 What changed from the 60D port

| where | change |
|:--|:--|
| `src/ml/60d/` → `src/ml/5d2/` | renamed |
| `src/ml/5d2/soup.c` | playback-photo lookup gated on the 5D Mark II and firmware 2.1.2, with its two addresses left empty (the 60D's are never read); fallback drive `A:` for the CF card; sizes in comments |
| `src/ml/5d2/soup.c`, `soup_dcim.h`, `dcim_test.c` | finds the DCIM folder really on the card instead of Magic Lantern's guess (number + `EOS5D`), so a folder another camera made, like `108_FUJI`, works; logs any frame it skips. Tested by `dcim_test.c` |
| `src/ml/5d2/soup_dev.c`, `soup_port.h` | the source JPEG is read through a 256 KB read-ahead instead of a seek and read per 8 KB; each develop logs the time spent reading and writing the card |
| `src/ml/5d2/soup_ui.c` | the playback log line no longer reads the 60D's addresses; notes on SET (C.Fn IV-3) and the 4:3 screen |
| `src/ml/5d2/soup_dev.c` | EXIF model fallback is `Canon EOS 5D Mark II` |
| `src/ml/5d2/ML-PIN.txt`, `Makefile`, `README.rst` | 5D2.212 |
| `src/tools/build-5d2.sh` | builds and checks symbols against `platform/5D2.212`; builds only Magic Lantern's symbol list, not its whole zip, so `zip` isn't needed |
| `card/ML/modules/soup.mo` | rebuilt for the 5D Mark II (the 60D build won't run on it) |
| `doc/` | removed: those were 60D screenshots |
| `src/engine/jpeg_dec.c`, `jpeg_enc.c`, `jpeg_enc.h`, `look.c` | taken from the [7D port](../Canon-EOS-7D) by @thebakers-club, whose speed work these are: a factored IDCT, skipping coefficients a half-size decode never uses, no library divides (the ARM946 has no divider), a colour memo and lookup tables in the look. Checked here before adopting: all 21 stocks on a real 5D Mark II JPEG and a synthetic 5D Mark II CR2 give byte-identical output to the 60D engine. `jpeg_enc.h` also gains an optional EXIF orientation, left at normal here |
| `src/engine/` (the rest) | unchanged. It already knows the 5D Mark II's raw (white level and colour matrix in `cr2.c`) |

<br>

## 🛠️ Building from source

Everything is in [`src/`](src): the module in [`src/ml/5d2/`](src/ml/5d2),
this port's copy of the engine in [`src/engine/`](src/engine), the stocks,
the recipes and the tools. [`src/README.md`](src/README.md) has the build
steps. In short, with Magic Lantern checked out at `ed3e7c0`:

```sh
sh src/tools/build-5d2.sh /path/to/magiclantern_simplified
```

<br>

## ❤️ Thanks

**Soup Engine** is the work of **luigi manga**
([jiijii0](https://github.com/jiijii0/soup-engine)): the look, the film
stocks, the engine and the picker are theirs. This port is adapted from
the 60D port by [kookueh](https://github.com/kookueh), which grew out of
the 50D port by [rebelt2i](https://github.com/rebelt2i). The faster
decoder, encoder and look code are from the 7D port by
[thebakers-club](https://github.com/thebakers-club).

The recipe stocks are based on recipes from
[Fuji X Weekly](https://fujixweekly.com).

This wouldn't exist without [Magic Lantern](https://magiclantern.fm).
Soup Engine runs inside their work, and years of their reverse
engineering made it possible to understand. They're worth supporting.

Soup Engine isn't Magic Lantern's project, so please don't take problems
with it to them. Open an issue here instead, and say which camera you're
using.

<br>

## 📄 Source and licence

GPL v2, same as Soup Engine. See [LICENSE](LICENSE) in this folder.
Distributed **without any warranty**, including the implied warranties of
merchantability and fitness for a particular purpose (sections 11 and 12).
