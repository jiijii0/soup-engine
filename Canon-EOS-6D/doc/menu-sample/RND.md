# INFO menu for the EOS 700D: R&D notes

`index.html` is the design, and it opens in a browser. This file is how that
design runs on the camera, and what was measured to get there. The UX and UI
are inspired by [M-Film](https://github.com/HNGSeries/M-Film), by
[@hng.spl](https://www.instagram.com/hng.spl), and the layout follows the menu
in their video (5D Mark III). This was built from that video and from Soup
Engine's own source; M-Film's code was not read or used.

## Where this stands

The Film screen, Home, Darkroom, Prints, Roll, Lens and System run on the
camera. The code is `src/ml/700d/ui.c`, `screen.c` and `prints.c`, with the art
in `src/ml/700d/data/SOUPUI.BIN` from `src/tools/mkui.js`. `doc/screens/` has
each page as the simulator draws it.

What has not run on a camera is the error-diffused preview described under
Prints. It has passed the simulator.

Two things were dropped from the design:

- **Viewfinder.** The 700D has an optical finder and nothing can draw in it.
  `CONFIG_BEEP` is off on this body, so a wind reminder has no way out either.
- **Touch.** Magic Lantern reports that a finger touched, not where (`menu.c`
  turns any touch into Q).

## What the camera showed

From `ML/LOGS/SOUPENG.LOG`:

| Question | Answer |
|:--|:--|
| Does the palette hold on the idle screen? | Yes. Canon reloaded it 0 times across 9 sessions, and the colours stayed right while frames developed |
| How long does a frame take? | Full frame 18-28 ms, one slide step 9-17 ms. Opening reads the pack in 150-190 ms |
| Does it fit beside a develop? | Yes, with no change in develop time: 39.7 and 40.3 s for a half-size frame |
| AE lock and AF point? | They stay Canon's. The latest log has `ui: raw 52 obj 0 arg 0`, so they appear to send one unnamed event with nothing to tell them apart |
| Does anything else draw on the screen? | Yes: Magic Lantern's flexinfo. It goes to the back buffer while the screen is open, see `ui.c` |

## Drawing it on the 700D

Everything is on the bitmap layer, without Canon's full-colour layer.

- The layer is 720x480, one byte a pixel, through a 256-entry palette of
  opacity, Y, U, V. Magic Lantern leaves entries 0, 3, 0x14 and 255 alone
  and keeps one for zebras; about 250 are free.
- Everything a module needs is in `700D_115.sym`: `LCD_Palette` and
  `EngDrvOut` to set entries, `bmp_draw_to_idle` and `bmp_idle_copy` to draw
  off screen and swap, and `canon_gui_disable_front_buffer` so Canon cannot
  paint over the menu, which is how Magic Lantern's own menu does it.
- Reducing all 13 films and Home to one shared 250-colour palette with ordered
  dither came out at 37.5 dB against true colour and reads the same at a
  glance. `8bit-check.png` shows it: true colour above, 8-bit below.
- Dithered art does not compress: a frame is 345,600 bytes raw and 644,978
  with RLE8. Sprites are stored raw, in one pack of about 1.8 MB that is read
  when the menu opens. That is too much for malloc (137 KB free in the card's
  log), so it goes through `fio_malloc`, the same place a develop gets its
  768 KB of buffers and, through `__mem_malloc`, its 1.5 MB arena. That memory
  prefers Canon's shoot pool, which has to be handed back before some Canon
  settings change, so the sprites are freed when the menu closes.
- Magic Lantern's own code re-applies its palette at least once a second
  because Canon can reset it, and the entries are put back from
  `LCD_Palette[3*i + 2]` on close, or Canon's screens would come back in the
  wrong colours.
- A `.fs` file not in the pack gets a plain can at the end of the row, so
  dropping a new film on the card needs no rebuild.

## Buttons

| Button | Film screen | Reaches the module as |
|:--|:--|:--|
| ◄ ► | Films | `MODULE_KEY_PRESS_LEFT/RIGHT` |
| Main dial | Films | `MODULE_KEY_WHEEL_*`; **still unconfirmed on the camera** |
| ▲ ▼ | Shelves | `MODULE_KEY_PRESS_UP/DOWN` |
| SET | Load | `MODULE_KEY_PRESS_SET` |
| PLAY | Auto develop | `MODULE_KEY_PLAY` |
| Trash | Quality | `MODULE_KEY_TRASH` |
| Q | Develop last shot | `MODULE_KEY_Q` |
| MENU | Home | `MODULE_KEY_MENU` |
| AE lock, AF point | Canon's | Appear to share raw event 0x52, obj 0, arg 0; `se_ui_saw_raw` logs unnamed events |
| INFO | Close, on to Canon | `MODULE_KEY_INFO`, passed on |
| Half-press | Close and shoot | `MODULE_KEY_PRESS_HALFSHUTTER`, passed on |

PLAY and Trash sit side by side on the back of the camera, so the left button
is the left toggle and the right button the right one.

## Prints

Flipping through frames at seconds each is not a viewer, so a preview is saved
beside each print while it develops: `ML/PRINTS/FLM_1234.SPV`, 648x432 and a
216x144 thumbnail, already in the screen's colours. The rows are passing
through anyway. A print without one gets it from the worker, decoding the JPEG,
the first time it is asked for. Calibrated from a frame that decodes in 14.2 s
on the camera and 40.7 ms on a Mac, these are estimates, not camera timings:

| Print | File size | Decoded to | Mac | Camera, estimated |
|:--|:--|:--|:--|:--|
| Quarter | 90–200 KB | 648x432 at 1/2 | 1.8–3.4 ms | 0.6–1.2 s |
| Half | 290–1070 KB | 648x432 at 1/4 | 5.4–16 ms | 1.9–5.6 s |
| Full | 2.8 MB | 1296x864 at 1/4 | 45 ms | 16 s, then a 2:1 reduction |

Photos are on the bitmap layer through a 6x6x5 colour cube at fixed entries,
which is what lets the worker write a preview with the pack not even loaded.
The camera proved that palette holds, and it keeps the half-press working.
Canon's YUV layer exists only in play mode (`enter_play_mode`, then
`get_yuv422_vram`, which is what `pic_view` does) and has never been run on this
camera, so true colour remains untried. The cost of the cube is that a preview
is for judging a frame, not a grade.

### The dither

The first build used a 4x4 Bayer dither, which reads on the LCD as a cross-hatch.
Four frames, two of them public, were reduced to 648x432 and scored against the
original. "Blurred" is after a 3x3 tent filter, roughly what the eye does at
arm's length, and it is the number that matters: unblurred scores mostly
measure the dither itself.

| Method | Colours | Luma, dB | Blurred RGB, dB | Blurred luma, dB |
|:--|:--|:--|:--|:--|
| Bayer 4x4 on the cube | 180 | 22.0–23.3 | 34.3–36.0 | 37.3–38.9 |
| Floyd-Steinberg on the cube | 180 | 25.2–26.3 | 37.3–38.2 | 41.1–41.9 |
| Floyd-Steinberg on 6x7x4 RGB | 168 | 26.4–27.2 | 36.4–37.3 | 42.2–42.9 |
| Floyd-Steinberg on 12x5x3 YUV | 180 | 29.4–29.8 | 33.7–34.4 | 40.3–42.5 |

`src/ml/700d/prints.c` uses the second row, serpentine, in integers. It costs
no palette change and no pack change. The luma-heavy YUV lattices have less
grain but lose colour, and the worst of them fell far further than this row
shows; 6x7x4 is a wash. `doc/screens/prints-dither.png` shows the same crop
before and after at 3x.

The cost is about 136 ARM instructions a pixel, counted from the compiled loop,
against about 35 for Bayer. With 304,000 pixels that is about 0.28 s a frame at
150 M instr/s, up from about 0.07 s: 2% on a quarter-size frame, under 1% on
half. Two rows of error add 10 KB of static memory. These are counts, not
camera timings.

The file's magic went from `SPV1` to `SPV2`, so older previews are made again
rather than shown beside new ones.

## Three things the menu exposed

These were in the module and mattered once the counter was on screen. All three
are fixed.

| Before | Now |
|:--|:--|
| A frame took whatever film and size were selected when its turn came, not what it was shot with. Five frames waiting, load VELVIA, and they all came out VELVIA | `se_job` keeps the film and size from when the frame was queued |
| A burst past 8 frames: the rest were logged and never developed | They wait in the watcher, are queued as room appears, and the counter includes them |
| `NotifyBox` announced each finished frame, and would draw over the menu in the wrong colours | Silent while the menu is open; the counter says it |

## Still unknown

- Whether the main dial reaches the module in shooting mode.
- Whether `FLM_` files show in Canon's PLAY. Our EXIF has no thumbnail, which
  may be why they would not.
- Whether the event for a touch carries coordinates at all.

## Notes for sharing

- ISO on a can is only a label, as `FILM SPEED` is on the 5D. PORTRA, GOLD,
  PROVIA, VELVIA, ACROS and SOUP 800 take theirs from the film's name or
  `.fs` notes. The other seven are guesses. Film files have no brand, shelf,
  ISO or process, so all four live in the design and are editable there.
- Cans carry brand names as shorthand, like the film names themselves, and
  those names belong to their owners. The brand line is one field per film in
  the Edit tab.
- The photos in the design are `doc/stocks.jpg`, the engine's real output, and
  already public in the repository.
- The fonts are Barlow and Barlow Semi Condensed, SIL OFL 1.1; see
  `OFL-Barlow.txt`.
