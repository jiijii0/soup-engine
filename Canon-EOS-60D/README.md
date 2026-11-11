<div align="center">

# 🎞️ Soup Engine for the Canon EOS 60D <sub><sup>beta</sup></sub>

### Film looks, in the camera, on a Canon EOS 60D.

<br>

<img src="doc/picker.png" alt="the film picker on the 60D" width="560">

<br>
<br>

`beta` &nbsp;·&nbsp; `GPL v2` &nbsp;·&nbsp; `21 film stocks` &nbsp;·&nbsp; `EOS 60D` &nbsp;·&nbsp; `firmware 1.1.1`

A community port by [@kookueh](https://github.com/kookueh).
<br>[← All cameras](../README.md#-pick-your-camera)

### 📦 [Download Canon-EOS-60D.zip](https://github.com/jiijii0/soup-engine/releases/download/folders/Canon-EOS-60D.zip)

</div>

<br>

Press **SET**, pick a film, shoot. Every
photo you take while Film is on gets a second JPEG with the look already
on it, made in the camera shortly after the shot.

```
DCIM/100CANON/IMG_1234.CR2    your photo (RAW or JPEG), untouched
FILM/100CANON/FLM_1234.JPG    the same photo with the look on it
```

> ⚠️ **Beta.** Unofficial code on a sixteen year old camera. It's easy to
> remove. Supported firmware is **60D 1.1.1** only.

> ⚖️ **No warranty.** Like all GPL software this comes with none. You
> install it at your own risk. See sections 11 and 12 of the
> [licence](LICENSE).

> 🎨 **The looks are still being tweaked.** The names are shorthand for
> the films and recipes they're based on, not exact copies. All
> trademarks belong to their owners.

<br>

---

<br>

## ✨ About this port

The look engine, the film stocks and the picker design are Soup Engine's.
On the 60D:

- **A Magic Lantern module.** It runs as `soup.mo` next to the official
  Magic Lantern 60D build; Magic Lantern itself is not changed.
- **The Soup Engine picker on the 60D**, opened with SET, with the
  original's **DEVELOPING** and **DEVELOPED** pages.
- **A small progress badge** on the shooting screen while frames develop.
- **A playback toggle.** In playback, SET swaps the photo on screen for
  its film version, and SET again brings back the original.
- **8 Fuji X Weekly recipes** as new stocks, and a translator
  (`src/tools/recipe2fs.py`) that turns a Fujifilm recipe into a stock.
- **RAW, RAW+JPEG and JPEG-only.** The film file keeps the shot's date,
  shutter, aperture and ISO, and records the stock in EXIF *Software*.
- **Only shots taken while Film is on are developed.** Photos already on
  the card are left alone.
- **Film files live outside DCIM**, so Canon's playback is never confused
  by them.

<br>

## 📦 Installing

#### 1️⃣ &nbsp; Install Magic Lantern

Install the official Magic Lantern build for the 60D first, following
their instructions:

> ### 👉 [builds.magiclantern.fm/60D.html](https://builds.magiclantern.fm/60D.html)

This port is built and checked against
`magiclantern.2025-06-20.60D.111`. The camera must be on firmware 1.1.1.

#### 2️⃣ &nbsp; Download this folder

[Download Canon-EOS-60D.zip](https://github.com/jiijii0/soup-engine/releases/download/folders/Canon-EOS-60D.zip)
and unzip it. The files for the card are in its [`card/`](card) folder. Or
build them yourself (see [`src/README.md`](src/README.md)).

#### 3️⃣ &nbsp; Copy the files to the card

Camera off, card in your computer. Copy the `ML` folder from `card/` to
the root of the card, merging into the `ML` folder Magic Lantern put
there. Eject the card properly, put it back and switch the camera on.

| file | what it is |
|:--|:--|
| ⚙️ `ML/modules/soup.mo` | the module |
| ✅ `ML/SETTINGS/soup.en` | an empty file that tells Magic Lantern to load it |
| 🎞️ `ML/FILM/` | `INDEX.TXT` and the 21 film stocks |

The module makes the `FILM` folder at the top of the card itself, and keeps its settings in
`ML/SE.CFG` and `ML/SE.NXT`.

#### ✅ &nbsp; Check it

Press **SET** on the shooting screen. The film picker should fill the
back screen. Pick a film with the wheel and press **SET**.

<br>

## 📷 Using it

### Picking a film

| Where | Button | What it does |
| --- | --- | --- |
| Shooting screen | **SET** | open the film picker |
| Picker | **wheel** or **◀ ▶** | browse the films |
| Picker | **SET** | choose this film (also turns Film on) |
| Picker | **Trash** or **MENU** | close |

The same settings are in the Magic Lantern menu under **Shoot → Soup
Film**: Film on/off, Stock, and a Status line.

> 💡 Most recipe looks are meant to be shot **+1/3 to +1 stop over**.

### While it develops

Developing takes a little while per frame. A small badge sits at the top right of
the shooting screen, with **+2** meaning two more frames are waiting, then
**FILM DONE** (or **FILM FAILED**) for a moment.

<img src="doc/badge.png" alt="the progress badge" width="760">

Press **SET** while a frame is developing to see the **DEVELOPING** page.
Turn the wheel there to go to the film list and pick the next stock. When
the queue finishes with the picker open, **DEVELOPED** shows how many
frames were done and how long it took.

<p>
<img src="doc/developing.png" alt="the DEVELOPING page" width="400">
<img src="doc/developed.png" alt="the DEVELOPED page" width="400">
</p>

### Seeing the look on the camera

Press **Play**. Once the photo is on screen, press **SET**: Soup swaps it
for its film version and shows **FILM &nbsp; SET: ORIGINAL** at the bottom.
Press **SET** again for Canon's original. Any other button puts the
original back first, then does what it normally does.

| Message | Meaning |
| --- | --- |
| `FILM: NO FILM VERSION OF THIS PHOTO` | taken with Film off, or before Soup was installed |
| `FILM: STILL DEVELOPING` | try again in a moment |
| `FILM: CAN'T TELL WHICH PHOTO THIS IS` | please open an issue, say it's the 60D, and attach `ML/LOGS/SOUP.LOG` |

### What lands on the card

| File | What it is |
| --- | --- |
| `DCIM/100CANON/IMG_1234.CR2` / `.JPG` | your photo, never changed |
| `FILM/100CANON/FLM_1234.JPG` | the film version: 2592×1728 from a full-size shot, half size from smaller ones |
| `ML/SE.CFG` | Film on/off and the chosen stock |
| `ML/SE.NXT` | where Soup got to, so it can catch up after a restart |
| `ML/LOGS/SOUP.LOG` | one line per develop, with the stock used |

The film folder follows the camera's DCIM folder number. Photo import apps
that only look in `DCIM` won't pick up the film files, so copy the `FILM`
folder across yourself.

<br>

## 🎞️ The films

They're small text files in `ML/FILM` on the card, so you can read them,
change them, or make your own with the
[Soup Stocks](https://jiijii0.github.io/soup-stocks/) editor.

Thirteen stocks come from Soup Engine. Eight (marked 🍳) are recipes from
[Fuji X Weekly](https://fujixweekly.com), translated into stocks by
`recipe2fs.py`. These are approximations: Fujifilm doesn't publish what
its settings do numerically, and here they run on a Canon JPEG.

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
Magic Lantern's **Modules** tab and restart. Your photos and film files stay where they are. If
the camera ever hangs, take the battery out.

<br>

## ⚠️ Known limits

- **Firmware 1.1.1 only.** The playback toggle finds out which photo is
  showing by reading two of Canon's own variables, located for 1.1.1. It
  only reads them.
- **SET in playback belongs to Soup** while the module is loaded, so Magic
  Lantern's "SET + dial" playback shortcuts won't see it.
- **The playback picture is the film file shrunk to the screen.** Zoom in
  and you are back on Canon's original.
- **The picker doesn't open in Live View or movie mode**, and nothing is
  developed in movie mode.

<br>

## 🛠️ Building from source

Everything is in [`src/`](src): the module in [`src/ml/60d/`](src/ml/60d),
this port's copy of the engine in [`src/engine/`](src/engine), the stocks,
the recipes and the tools. [`src/README.md`](src/README.md) has the build
steps. In short, with Magic Lantern checked out at `ed3e7c0`:

```sh
sh src/tools/build-60d.sh /path/to/magiclantern_simplified
```

<br>

## ❤️ Thanks

**Soup Engine** is the work of **luigi manga**
([jiijii0](https://github.com/jiijii0/soup-engine)): the look, the film
stocks, the engine and the picker are theirs. The 60D port is by
[kookueh](https://github.com/kookueh), written with an AI assistant
(Claude), as the commits say. It grew out of the 50D port by
[rebelt2i](https://github.com/rebelt2i).

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

GPL v2, same as Soup Engine. See [LICENSE](LICENSE) in this folder. Distributed **without
any warranty**, including the implied warranties of merchantability and
fitness for a particular purpose (sections 11 and 12).
