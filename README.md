<div align="center">

# 🎞️ Soup Engine <sub><sup>beta</sup></sub>

### Film looks, in the camera, on a Canon EOS 5D Classic or EOS 50D.

<br>

<img src="doc/camera.gif" alt="scrolling the film stocks on the camera" width="620">

<br>
<br>

`beta` &nbsp;·&nbsp; `GPL v2` &nbsp;·&nbsp; `13 film stocks` &nbsp;·&nbsp; `EOS 5D Classic` &nbsp;·&nbsp; `EOS 50D`

</div>

<br>

The same look engine runs on both bodies. How you pick the stock, and
what lands on the card, differs by camera — see **Using it** below for
each.

On the **5D Classic**, press the **trash button**, pick a film, shoot.
On the **50D**, open the Magic Lantern **Film** page and turn **Film**
on.

```
# 5D Classic
IMG_1234.CR2        your raw file, untouched
IMG_1234.FLM.JPG    the same photo with the look on it

# 50D
IMG_1234.CR2        your raw file (preview updated when Delete CR2 is off)
FLM_1234.JPG        the same photo with the look on it, 2352×1568
```

<br>

![all thirteen film looks on one frame](doc/stocks.jpg)

<div align="center"><sub>
All thirteen, one frame, straight off the card.
</sub></div>

<br>

> ⚠️ **Beta.** Unofficial code on twenty year old cameras. It's easy to
> remove. Supported firmware is **5D Classic 1.1.1** and **50D 1.0.9**;
> other bodies are out of scope.

> ⚖️ **No warranty.** Like all GPL software this comes with none. You
> install it at your own risk. See sections 11 and 12 of the
> [licence](LICENSE).

> 🎨 **The looks are still being tweaked** and will keep changing. The
> names are shorthand for the films they're based on, not exact copies.
> All trademarks belong to their owners.

<br>

---

<br>

## 📦 Installing

Pick your body. The steps are not interchangeable: the Classic boots
`SE.BIN` through a patched Magic Lantern; the 50D boots a Magic Lantern
rebuild with the Film page linked in.

<br>

### EOS 5D Classic (firmware 1.1.1)

#### 1️⃣ &nbsp; Install Magic Lantern

> ### 👉 [builds.magiclantern.fm/5DC.html](https://builds.magiclantern.fm/5DC.html)

Follow their instructions, then check the camera starts and their menu
opens before going further. Soup Engine never touches your camera's
boot flag; their installer does that.

<br>

#### 2️⃣ &nbsp; Back up their `autoexec.bin`

Copy it off the card somewhere safe. Putting it back is how you
uninstall.

<br>

#### 3️⃣ &nbsp; Copy the files to the root of your card

> ### 📦 [Download the latest release](https://github.com/jiijii0/soup-engine/releases/latest)

Unzip to the card root, replacing `AUTOEXEC.BIN`. Or copy
[`card/`](card) from this repo, which is the same files.

| file | what it is |
|:--|:--|
| 📄 `AUTOEXEC.BIN` | replaces Magic Lantern's |
| ⚙️ `SE.BIN` | the engine |
| 🎞️ `ML/FILM/` | the film stocks |

<br>

#### ✅ &nbsp; Put the card in and switch the camera on

That's it. Nothing to enable, no menu to find.

> While Soup Engine is installed you won't have Magic Lantern's own
> menu, since this replaces the file that provides it. Put their
> `autoexec.bin` back whenever you want it again.

<br>

### EOS 50D (firmware 1.0.9)

Magic Lantern's build for that firmware is the one this was made
against: `magiclantern.2025-06-20.50D.109`.

<br>

#### 1️⃣ &nbsp; Install Magic Lantern

> ### 👉 [builds.magiclantern.fm/50D.html](https://builds.magiclantern.fm/50D.html)

Follow their instructions, then check the camera starts and their menu
opens before going further.

<br>

#### 2️⃣ &nbsp; Back up their `autoexec.bin`

Copy it off the card somewhere safe. Putting it back (with a matching
`ML/modules/50D_109.sym`) is how you uninstall.

<br>

#### 3️⃣ &nbsp; Build their `AUTOEXEC.BIN` with the Film page

The 50D does not boot `SE.BIN`, and `tools/patch-film.py` must not be
run on it. The body boots Magic Lantern plus a small overlay in this
repo. Why the Classic patch path fails here is in
[`src/ml/platforms/50D.109/PITFALLS.md`](src/ml/platforms/50D.109/PITFALLS.md).

```sh
python3 src/tools/build-50d.py --card /path/to/card
```

That checks out Magic Lantern `ed3e7c0d` (the 2025-06-20 50D.109
build), copies [`integrations/magiclantern/50D.109/`](integrations/magiclantern/50D.109/)
over it, and writes three things to the card:

| file | what it is |
|:--|:--|
| 📄 `AUTOEXEC.BIN` | Magic Lantern, with the Film page linked in |
| 📄 `ML/modules/50D_109.sym` | must match that `AUTOEXEC.BIN` |
| 🎞️ `ML/FILM/` | the film stocks |

<br>

#### ✅ &nbsp; Put the card in and switch the camera on

Open the Magic Lantern menu. The **Film** page is the one with the `F`
tab. Turn **Film** on. Leave **Delete CR2** off until you have seen a
picture you like.

<br>

---

<br>

## 📷 Using it

<br>

### EOS 5D Classic

Press the **trash button** and the picker fills the screen.

| button | what it does |
|:--|:--|
| 🎛️ **Wheel** | choose a film |
| ↕️ **Up / Down** | choose where the look comes from |
| ✅ **`SET`** | pick that film and close |
| 🗑️ **Trash** | close, changing nothing |

Your choice survives switching the camera off.

Each photo takes about twenty seconds to develop. You can keep shooting
while it works, and the screen shows `FRAME 2 OF 3` when there's a
queue.

<br>

> ### ⚠️ Set your Picture Style to Standard, Neutral or Faithful
>
> On Monochrome every look comes out black and white.

<br>

**`SOURCE: JPEG`** is the default. Use this.

**`SOURCE: RAW`** works from the sensor data instead. It takes minutes
rather than seconds, some colour adjustments can't be applied, and
`ACROS` won't be black and white.

`FILM SPEED 50` on the picker is only a label. It does not change your
camera's ISO.

Your raw files are never modified. Delete the JPEG and the photo is
exactly as the camera shot it.

<br>

### EOS 50D

On the **Film** page:

| row | what it does |
|:--|:--|
| **Film** | off leaves every file alone. On develops each new raw. |
| **Delete CR2** | off keeps the raw. On deletes it only after `FLM_nnnn.JPG` exists. |
| **Stock** | the look. The wheel changes it. |

The choice is written to `ML/SE.CFG` and survives switching the camera off.

Take a photo. The screen shows `FILM ACROS 43,71%` until the JPEG is
finished. That is often half a minute on this body, and you can keep
shooting while it works. When the box goes away, open playback on that
shot: with **Delete CR2** off, the raw's preview is already the look.
`FLM_1234.JPG` is the same picture as its own file, and the camera
lists it after the next power-on.

<br>

> ### ⚠️ Set your Picture Style to Standard, Neutral or Faithful
>
> On Monochrome every look comes out black and white, because the camera
> hands this a grey picture to start from and there's no colour in it
> to work with.

<br>

The look is applied to the JPEG preview Canon already embedded in the
raw, not to the sensor data. That is why a monochrome picture style
comes out black and white: the preview arrives with no colour left.

<br>

---

<br>

## 🎞️ The films

Thirteen, still under development.

They're small text files in `ML/FILM` on the card, so you can read them,
change them, or write your own. The camera lists whatever it finds
there.

<br>

<div align="center">

### 🎨 [Soup Stocks](https://jiijii0.github.io/soup-stocks/)

**A web editor for the film stocks.**

Drop in a photo, move sliders, download a `.fs`, put it on the card.

The preview is this engine's own code compiled to WebAssembly, so it
matches what the camera writes.<br>Your photo never leaves your browser.

</div>

<br>

---

<br>

## 🧹 Removing it

<br>

### EOS 5D Classic

Put Magic Lantern's original `autoexec.bin` back. That's all.

To pause it without uninstalling, delete `SE.BIN`. The camera boots
normally without it.

> 🆘 **If the camera ever misbehaves:** pull the battery, put the card
> in a computer, delete `SE.BIN`. Don't delete `AUTOEXEC.BIN` while the
> boot flag is on, because with the flag set and no `AUTOEXEC.BIN` the
> camera won't start at all.

<br>

### EOS 50D

Put Magic Lantern's original `autoexec.bin` back, and put a matching
`ML/modules/50D_109.sym` beside it. The 50D does not boot `SE.BIN`;
deleting that file does not uninstall the Film page.

> 🆘 **If the camera ever misbehaves:** pull the battery, put the card
> in a computer, restore stock Magic Lantern's `AUTOEXEC.BIN` and
> `50D_109.sym`. Don't delete `AUTOEXEC.BIN` while the boot flag is on.

<br>

---

<br>

## ❤️ Thanks

This wouldn't exist without [Magic Lantern](https://magiclantern.fm).

Soup Engine runs inside their work. Their builds for the 5D Classic and
the 50D are what let the cameras run any of this, and years of their
reverse engineering made it possible to understand. They're worth
supporting.

Soup Engine isn't their project, so please don't take problems with it
to them. Open an issue here instead.

<br>

## 📄 Source and licence

All of it is in [`src/`](src).

On the **5D Classic**, `AUTOEXEC.BIN` is Magic Lantern's program with
215 bytes changed, and `tools/patch-film.py` is what applies them.

On the **50D**, `AUTOEXEC.BIN` is a rebuilt Magic Lantern with the Film
page from [`integrations/magiclantern/50D.109/`](integrations/magiclantern/50D.109/),
built by `tools/build-50d.py`.

GPL v2. See [LICENSE](LICENSE). Distributed **without any warranty**,
including the implied warranties of merchantability and fitness for a
particular purpose (sections 11 and 12).
