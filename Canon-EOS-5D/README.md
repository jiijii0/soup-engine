<div align="center">

# 🎞️ Soup Engine <sub><sup>beta</sup></sub>

### Film looks, in the camera, on a Canon EOS 5D Classic.

<br>

<img src="doc/camera.gif" alt="scrolling the film stocks on the camera" width="620">

<br>
<br>

`beta` &nbsp;·&nbsp; `GPL v2` &nbsp;·&nbsp; `13 film stocks` &nbsp;·&nbsp; `EOS 5D Classic`

</div>

<br>

Press the **trash button** and a list of film stocks fills the back
screen. Turn the wheel to pick one, press `SET`. Every photo you take
after that gets a second JPEG saved beside the raw, with the look
already on it.

```
IMG_1234.CR2        your raw file, untouched
IMG_1234.FLM.JPG    the same photo with the look on it
```

Your raw files are never modified. Delete the JPEG and the photo is
exactly as the camera shot it.

<br>

![all thirteen film looks on one frame](doc/stocks.jpg)

<div align="center"><sub>
All thirteen, one frame, straight off the card.
</sub></div>

<br>

> ⚠️ **Beta.** Unofficial code on a twenty year old camera. It's easy to
> remove, and it never modifies your raw files.

> ⚖️ **No warranty.** Like all GPL software this comes with none. You
> install it at your own risk. See sections 11 and 12 of the
> [licence](LICENSE).

> 🎨 **The looks are still being tweaked** and will keep changing. The
> names are shorthand for the films they're based on, not exact copies.
> All trademarks belong to their owners.

> 📷 **Canon EOS 50D.** A community port by
> [@rebelt2i](https://github.com/rebelt2i) lives on its own branch,
> [`50d`](https://github.com/jiijii0/soup-engine/tree/50d).

<br>

---

<br>

## 📦 Installing

You need a **Canon EOS 5D Classic** (the original 2005 one, firmware
1.1.1) and a CompactFlash card. It will not work on any other Canon:
the camera-side code is built for this body's firmware.

<br>

### 1️⃣ &nbsp; Install Magic Lantern

> ### 👉 [builds.magiclantern.fm/5DC.html](https://builds.magiclantern.fm/5DC.html)

Follow their instructions, then check the camera starts and their menu
opens before going further. Soup Engine never touches your camera's
boot flag; their installer does that.

<br>

### 2️⃣ &nbsp; Back up their `autoexec.bin`

Copy it off the card somewhere safe. Putting it back is how you
uninstall.

<br>

### 3️⃣ &nbsp; Copy the files to the root of your card

> ### 📦 [Download the latest release](https://github.com/jiijii0/soup-engine/releases/latest)

Unzip to the card root, replacing `AUTOEXEC.BIN`. Or copy
[`card/`](card) from this repo, which is the same files.

| file | what it is |
|:--|:--|
| 📄 `AUTOEXEC.BIN` | replaces Magic Lantern's |
| ⚙️ `SE.BIN` | the engine |
| 🎞️ `ML/FILM/` | the film stocks |

<br>

### ✅ &nbsp; Put the card in and switch the camera on

That's it. Nothing to enable, no menu to find.

> While Soup Engine is installed you won't have Magic Lantern's own
> menu, since this replaces the file that provides it. Put their
> `autoexec.bin` back whenever you want it again.

<br>

---

<br>

## 📷 Using it

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

<br>

---

<br>

## 🎞️ The films

Thirteen, still under development.

They're small text files in `ML/FILM` on the card, so you can read them,
change them, or write your own.

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

Put Magic Lantern's original `autoexec.bin` back. That's all.

To pause it without uninstalling, delete `SE.BIN`. The camera boots
normally without it.

> 🆘 **If the camera ever misbehaves:** pull the battery, put the card
> in a computer, delete `SE.BIN`. Don't delete `AUTOEXEC.BIN` while the
> boot flag is on, because with the flag set and no `AUTOEXEC.BIN` the
> camera won't start at all.

<br>

---

<br>

## ❤️ Thanks

This wouldn't exist without [Magic Lantern](https://magiclantern.fm).

Soup Engine runs inside their work. Their build for the 5D Classic is
what lets the camera run any of this, and years of their reverse
engineering made it possible to understand. They're worth supporting.

Soup Engine isn't their project, so please don't take problems with it
to them. Open an issue here instead.

<br>

## 📄 Source and licence

All of it is in [`src/`](src). `AUTOEXEC.BIN` is Magic Lantern's program
with 215 bytes changed, and the script that applies them is there too.

GPL v2. See [LICENSE](LICENSE). Distributed **without any warranty**,
including the implied warranties of merchantability and fitness for a
particular purpose (sections 11 and 12).
