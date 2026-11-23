# Building the 60D module

Everything the 60D needs is in this folder:

| path | what it is |
|:--|:--|
| `ml/60d/` | the Magic Lantern module: `soup.c` (menu, settings, watching for new frames), `soup_dev.c` (one frame's develop, and the playback viewer), `soup_ui.c` (the picker, the badge, the playback toggle) |
| `engine/` | this port's copy of the Soup Engine look engine (plain C, integer only) |
| `stocks/` | the 21 film stocks, with their long names; `INDEX.TXT` sets the order |
| `recipes/` | the 8 Fuji X Weekly recipes the recipe stocks are made from |
| `tools/` | `build-60d.sh`, `recipe2fs.py`, `gen-recipes.sh` |

## Build

The module is built against Magic Lantern's 2025-06-20 60D build, commit
`ed3e7c0` (see [`ml/60d/ML-PIN.txt`](ml/60d/ML-PIN.txt)). You need the ARM
toolchain Magic Lantern itself builds with.

```sh
git clone https://github.com/reticulatedpines/magiclantern_simplified
git -C magiclantern_simplified checkout ed3e7c0

# from Canon-EOS-60D/
sh src/tools/build-60d.sh /path/to/magiclantern_simplified
```

The script copies `ml/60d` into the Magic Lantern tree as `modules/soup`,
builds Magic Lantern for 60D.111 (for its symbol list), builds the module,
checks every symbol it imports is exported by the 60D build, and writes
`card/` (`ML/modules/soup.mo`, `ML/SETTINGS/soup.en`, `ML/FILM/`). The camera
only opens 8.3 file names, so long stock names are shortened on the way.

## Recipes

A recipe is a base film simulation plus Fujifilm camera settings.
`recipe2fs.py` turns one into a stock; `gen-recipes.sh` rebuilds all of them
into `stocks/`. Run `build-60d.sh` afterwards to refresh `card/`.

```sh
python3 src/tools/recipe2fs.py src/recipes/kchrom64.recipe -o src/stocks/kchrom64.fs --stocks src/stocks
```

The constants that map recipe settings to curves are estimates, kept in one
block at the top of `recipe2fs.py`.

## Desktop check

`ml/60d/host_test.c` runs the module's develop code on a desktop, against
the same engine files, so the camera's output can be compared:

```sh
E=src/engine; M=src/ml/60d
cc -O2 -std=gnu99 -DSOUP_HOST -I$E -I$M -o host_test $M/host_test.c $M/soup_dev.c \
   $E/se.c $E/cr2.c $E/jpeg_dec.c $E/jpeg_enc.c $E/look.c $E/stock.c $E/wb.c \
   $E/color.c $E/grade.c $E/lut.c -lm
./host_test IMG_1234.CR2 src/stocks/velvia.fs out.jpg
```

## Emulator

The port was also tested in [qemu-eos](https://github.com/reticulatedpines/qemu-eos),
which needs your own camera's ROM dump. **Never commit `ROM0.BIN` or
`ROM1.BIN`:** they are Canon's firmware (`.gitignore` blocks them).
