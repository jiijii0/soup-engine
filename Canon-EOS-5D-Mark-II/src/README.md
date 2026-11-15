# Building the 5D Mark II module

Everything the 5D Mark II needs is in this folder:

| path | what it is |
|:--|:--|
| `ml/5d2/` | the Magic Lantern module: `soup.c` (menu, settings, watching for new frames), `soup_dev.c` (one frame's develop, and the playback viewer), `soup_ui.c` (the picker, the badge, the playback toggle) |
| `engine/` | this port's copy of the Soup Engine look engine (plain C, integer only); `jpeg_dec.c`, `jpeg_enc.c/.h` and `look.c` are the 7D port's faster, byte-identical versions |
| `stocks/` | the 36 film stocks, with their long names (13 Soup Engine, 12 from the 7D port, 3 from the 6D port, 8 recipes); `INDEX.TXT` sets the order |
| `recipes/` | the 8 Fuji X Weekly recipes the recipe stocks are made from |
| `tools/` | `build-5d2.sh`, `recipe2fs.py`, `gen-recipes.sh` |

## Build

The module is built against Magic Lantern's 2025-06-20 5D Mark II build, commit
`ed3e7c0` (see [`ml/5d2/ML-PIN.txt`](ml/5d2/ML-PIN.txt)). You need the ARM
toolchain Magic Lantern itself builds with. On Ubuntu (or Ubuntu in WSL
on Windows), `sudo apt install gcc-arm-none-eabi make gcc` covers it.

```sh
git clone https://github.com/reticulatedpines/magiclantern_simplified
git -C magiclantern_simplified checkout ed3e7c0

# from Canon-EOS-5D-Mark-II/
sh src/tools/build-5d2.sh /path/to/magiclantern_simplified
```

The script copies `ml/5d2` into the Magic Lantern tree as `modules/soup`,
builds Magic Lantern for 5D2.212 (for its symbol list), builds the module,
checks every symbol it imports is exported by the 5D2 build, and writes
`card/` (`ML/modules/soup.mo`, `ML/SETTINGS/soup.en`, `ML/FILM/`). The camera
only opens 8.3 file names, so long stock names are shortened on the way.

## Recipes

A recipe is a base film simulation plus Fujifilm camera settings.
`recipe2fs.py` turns one into a stock; `gen-recipes.sh` rebuilds all of them
into `stocks/`. Run `build-5d2.sh` afterwards to refresh `card/`.

```sh
python3 src/tools/recipe2fs.py src/recipes/kchrom64.recipe -o src/stocks/kchrom64.fs --stocks src/stocks
```

The constants that map recipe settings to curves are estimates, kept in one
block at the top of `recipe2fs.py`.

## Desktop check

`ml/5d2/host_test.c` runs the module's develop code on a desktop, against
the same engine files, so the camera's output can be compared:

```sh
E=src/engine; M=src/ml/5d2
cc -O2 -std=gnu99 -DSOUP_HOST -I$E -I$M -o host_test $M/host_test.c $M/soup_dev.c \
   $E/se.c $E/cr2.c $E/jpeg_dec.c $E/jpeg_enc.c $E/look.c $E/stock.c $E/wb.c \
   $E/color.c $E/grade.c $E/lut.c -lm
./host_test IMG_1234.CR2 src/stocks/velvia.fs out.jpg
```

`ml/5d2/dcim_test.c` checks how the module picks the DCIM folder the
camera is really writing to (`soup_dcim.h`), including a card whose folder
another camera made (`108_FUJI`):

```sh
cc -std=gnu99 -o dcim_test src/ml/5d2/dcim_test.c && ./dcim_test
```

## Emulator

The 60D port was also tested in [qemu-eos](https://github.com/reticulatedpines/qemu-eos);
this one hasn't been yet. It is also how the 5D Mark II's playback
addresses can be found (see `soup_play_current()` in `ml/5d2/soup.c`).
qemu-eos needs your own camera's ROM dump: Magic Lantern writes it to
`ML/LOGS/ROM0.BIN` and `ROM1.BIN` on the card. **Never commit `ROM0.BIN` or
`ROM1.BIN`:** they are Canon's firmware (`.gitignore` blocks them).
