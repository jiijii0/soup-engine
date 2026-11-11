# Soup Engine

# Raw files to test against. They live in samples/fromJpeg and
# samples/fromRaw now, grouped by which develop path suits them, so this
# looks in both and keeps the old flat layout working. When the samples
# moved, this matched nothing and simtest failed with "no preview",
# which says nothing about a missing sample.
SAMPLES := $(wildcard samples/*.CR2) $(wildcard samples/*/*.CR2)

.PHONY: all test test-full test-camera simtest sim100d asan arm clean contact payload card-100d vendor-100d

all:
	@$(MAKE) -s -C host

# The checks that need no sample files. Run this one by default.
test: all
	@./filmdev selftest

# Everything, including the sample-driven checks and the sanitiser build.
test-full: all
	@./filmdev selftest
	@echo
	@for f in $(SAMPLES); do ./filmdev stress $$f 200 || exit 1; echo; done
	@echo "--- rebuilding under ASan + UBSan ---"
	@$(MAKE) -s -C host asan
	@./filmdev selftest
	@for f in $(SAMPLES); do ./filmdev stress $$f 30 || exit 1; done
	@for f in $(SAMPLES); do ./filmdev develop $$f --stock velvia --half -q -o /dev/null >/dev/null || exit 1; done
	@./filmdev ui out >/dev/null
	@echo "clean under ASan + UBSan"
	@echo
	@echo "--- restoring the release build ---"
	@$(MAKE) -s -C host clean && $(MAKE) -s -C host

# The camera-only code: the raw spool and the payload's libc. Needs a sample
# to compare against, and checks every scale -- only scale 1 seeks backwards.
SDK := $(shell for s in MacOSX26.5.sdk MacOSX26.sdk MacOSX15.sdk; do \
                 p=/Library/Developer/CommandLineTools/SDKs/$$s; \
                 if [ -d $$p ]; then echo -isysroot $$p; break; fi; done)
CAMTEST_CC = cc -std=c99 -O2 -DSE_HOSTED $(SDK) -Iengine -Iml/payload

test-camera:
	@mkdir -p out
	@$(CAMTEST_CC) -c -o out/payload_libc.o ml/payload/libc.c \
	   -Dvsnprintf=se_vsnprintf -Dmemcpy=se_x_memcpy -Dmemset=se_x_memset \
	   -Dstrlen=se_x_strlen -Dstrcmp=se_x_strcmp -Dstrncmp=se_x_strncmp
	@$(CAMTEST_CC) -o out/camtest tools/camtest.c out/payload_libc.o \
	   engine/se.c engine/wb.c engine/cr2.c engine/color.c engine/debayer.c engine/lut.c \
	   engine/stock.c engine/grade.c engine/jpeg_enc.c engine/pipeline.c -lm
	@for f in $(SAMPLES); do for s in 1 2 4; do \
	   echo "--- `basename $$f` scale=$$s"; \
	   ./out/camtest $$f $$s | grep -E 'vsnprintf|RESULT|STREAM|stream:' || exit 1; \
	 done; done

# Run the real ml/payload/film.c on the desktop, against a mock Canon API
# that returns short reads, caps the heap at 7 MB and gives the worker a real
# 48 KB stack. Builds a simulated card from the first sample raw.
SIMCARD = out/simcard

simtest: all
	@mkdir -p out
	@# Delete it first. This line used to be `cc ... | grep ...; test -x
	@# out/camsim`, which takes grep's exit status and then happily finds
	@# the *previous* binary -- so a failed compile ran the last good build
	@# and the run was reported as a pass. That is the trap documented in
	@# docs/HANDOFF.md section 6, still live in a second form. Now nothing
	@# can be run unless this compile produced it.
	@rm -f out/camsim
	@cc -std=c99 -O1 -g -DSE_HOSTED $(SDK) -Itools/camsim -Iengine -o out/camsim \
	   tools/camsim/camsim.c ml/payload/film.c \
	   engine/se.c engine/wb.c engine/cr2.c engine/color.c engine/debayer.c engine/lut.c \
	   engine/stock.c engine/grade.c engine/jpeg_enc.c engine/jpeg_dec.c \
	   engine/pipeline.c engine/look.c engine/ui.c engine/ui_font.c \
	   -lm -lpthread 2> out/camsim.cc.log \
	   || { grep -v 'reducing alignment' out/camsim.cc.log; \
	        echo "camsim did not compile"; exit 1; }
	@grep -v 'reducing alignment' out/camsim.cc.log || true
	@test -x out/camsim
	@$(MAKE) -s out/previewcheck
	@$(CAMTEST_CC) -o out/hashcheck tools/hashcheck.c engine/se.c \
	   engine/wb.c engine/cr2.c engine/lut.c engine/stock.c engine/grade.c -lm
	@echo "expected, computed on the desktop:"
	@./out/previewcheck $(firstword $(SAMPLES)) stocks/soup800.fs 2 /dev/null
	@sh tools/simtest.sh $(firstword $(SAMPLES)) \
	   "`./out/previewcheck $(firstword $(SAMPLES)) stocks/soup800.fs 2 /dev/null | grep 'look hash' | sed 's/look hash //'`"

# previewcheck is the preview path on the desktop, and simtest's
# reference: the camera's output is compared against it byte for byte.
#
# It used to be built only as a recipe line inside simtest, so `make all`
# and `make test` left it alone and it went quietly stale. Rendering a
# stock through a binary four minutes older than engine/look.c showed no
# change from an engine edit that had definitely taken effect, which is a
# very convincing way to conclude the edit did nothing.
PREVIEW_SRC = tools/previewcheck.c engine/se.c engine/wb.c engine/cr2.c \
	      engine/lut.c engine/stock.c engine/grade.c engine/jpeg_enc.c \
	      engine/jpeg_dec.c engine/look.c

out/previewcheck: $(PREVIEW_SRC) $(wildcard engine/*.h)
	@mkdir -p out
	@$(CAMTEST_CC) -o $@ $(PREVIEW_SRC) -lm

# Run the real ml/100d module on the desktop, against a mock of Magic
# Lantern (tools/sim100d/ml.h stands in for ml/100d/ml.h) with a 3 MB DMA
# pool, short reads, a real worker thread and a real shutdown flag. It
# develops a frame and compares it with previewcheck byte for byte, then
# drives the watcher and the picker. See tools/sim100d/sim.c.
SIM100D_SRC = tools/sim100d/sim.c ml/100d/soupeng.c ml/100d/develop.c \
              ml/100d/watch.c ml/100d/screen.c ml/100d/ui.c ml/100d/prints.c
SIM100D_ENG = engine/se.c engine/wb.c engine/cr2.c engine/color.c engine/debayer.c \
              engine/lut.c engine/stock.c engine/grade.c engine/jpeg_enc.c \
              engine/jpeg_dec.c engine/pipeline.c engine/look.c

out/sim100d: $(SIM100D_SRC) tools/sim100d/ml.h ml/100d/soupeng.h $(SIM100D_ENG) $(wildcard engine/*.h)
	@mkdir -p out
	@# the module's sources see the mock through <ml.h>; the engine's see the host libc
	@for f in ml/100d/soupeng.c ml/100d/develop.c ml/100d/watch.c ml/100d/screen.c ml/100d/ui.c ml/100d/prints.c; do \
	   cc -std=gnu99 -O1 -g $(SDK) -Itools/sim100d -Iml/100d -Iengine -Wall -Wno-unused-function \
	      -c -o out/sim100d_`basename $$f .c`.o $$f || exit 1; done
	@cc -std=gnu99 -O1 -g $(SDK) -Itools/sim100d -Iengine -DSE_HOSTED -Wall -Wno-unused-function \
	   -c -o out/sim100d_sim.o tools/sim100d/sim.c
	@cc -std=c99 -O1 -g -DSE_HOSTED $(SDK) -Iengine -o $@ \
	   out/sim100d_sim.o out/sim100d_soupeng.o out/sim100d_develop.o out/sim100d_watch.o out/sim100d_screen.o out/sim100d_ui.o out/sim100d_prints.o \
	   $(SIM100D_ENG) -lm -lpthread 2>&1 | grep -v 'reducing alignment' || true
	@test -x $@

# With no raws in samples/ (which is every fresh clone: they are the owner's
# photos and are not committed) it runs on out/public.CR2, made from one frame
# of doc/stocks.jpg. SIM100D_SAMPLE=out/public.CR2 picks that one regardless,
# and it is the only sample whose SE_SIM_DUMP pictures are fit to publish.
SIM100D_SAMPLE ?= $(or $(firstword $(SAMPLES)),out/public.CR2)

sim100d: all out/previewcheck out/sim100d $(filter out/public.CR2,$(SIM100D_SAMPLE))
	@sh tools/sim100d.sh $(SIM100D_SAMPLE)

MKCR2_SRC = tools/mkcr2.c engine/se.c engine/jpeg_enc.c engine/jpeg_dec.c

out/mkcr2: $(MKCR2_SRC) $(wildcard engine/*.h)
	@mkdir -p out
	@$(CAMTEST_CC) -o $@ $(MKCR2_SRC) -lm

out/public.CR2: out/mkcr2 ../doc/stocks.jpg
	@./out/mkcr2 ../doc/stocks.jpg $@

# The camera build. Produces two files for the card:
#   ml/build/AUTOEXEC.BIN  the 2012 binary with 262 bytes changed, same size
#   ml/build/SE.BIN        the soup engine itself, loaded on demand
payload:
	@$(MAKE) -s -C ml/payload
	@python3 tools/patch-film.py

# The EOS 100D (Rebel SL1) build: a Magic Lantern module, and everything the card
# needs beside it, in out/card-100d/. Copy that folder's contents onto a
# card that already has Magic Lantern's 100D build on it. Needs the
# pinned Magic Lantern checkout (vendor-100d below fetches it) and the
# Arm toolchain described in ml/100d/Makefile.
card-100d: vendor-100d
	@$(MAKE) -s -C ml/100d
	@rm -rf out/card-100d
	@mkdir -p out/card-100d/ML/modules out/card-100d/ML/FILM out/card-100d/ML/SETTINGS out/card-100d/ML/DATA
	@cp ml/100d/build/soupeng.mo out/card-100d/ML/modules/
	@cp ml/100d/data/SOUPUI.BIN out/card-100d/ML/DATA/
	@cp stocks/*.fs out/card-100d/ML/FILM/
	@touch out/card-100d/ML/SETTINGS/soupeng.en
	@# Magic Lantern's script writes user@host into a module, and these get
	@# handed around. The patch stops that; a checkout from before it does not.
	@if strings -a out/card-100d/ML/modules/soupeng.mo | grep -q "`whoami`@"; then \
	   echo "soupeng.mo carries your user name: delete vendor/magiclantern_simplified and run this again"; \
	   rm -rf out/card-100d; exit 1; fi
	@echo "out/card-100d: $$(ls out/card-100d/ML/FILM | wc -l | tr -d ' ') films, soupeng.mo $$(stat -f%z ml/100d/build/soupeng.mo 2>/dev/null || stat -c%s ml/100d/build/soupeng.mo) bytes"
	@echo "copy the contents of out/card-100d onto the card; eject before removing it"

# Magic Lantern's source, at the commit the 2025-06-20 Magic Lantern builds
# was made from, with ml/100d/patches/ applied: three small fixes to their
# module build scripts for current Python and for a module living outside
# their tree. Nothing of theirs is committed here; this fetches it. Safe to
# run again: it does nothing once the checkout is at the right commit.
ML_100D_REPO   = https://github.com/reticulatedpines/magiclantern_simplified
ML_100D_COMMIT = ed3e7c0df
ML_100D_DIR    = vendor/magiclantern_simplified

vendor-100d:
	@if [ ! -d $(ML_100D_DIR)/.git ]; then \
	   echo "fetching Magic Lantern's source into $(ML_100D_DIR)"; \
	   git clone -q $(ML_100D_REPO) $(ML_100D_DIR); \
	 fi
	@cd $(ML_100D_DIR) && \
	 if ! git rev-parse -q --verify camera-build >/dev/null; then \
	   git checkout -q -b camera-build $(ML_100D_COMMIT) && \
	   git apply ../../ml/100d/patches/magiclantern-build-scripts.patch && \
	   git -c user.name=soup-engine -c user.email=soup@engine commit -qam \
	     "modules: build-script fixes from soup-engine/ml/100d/patches" && \
	   echo "Magic Lantern at $(ML_100D_COMMIT), patched"; \
	 else git checkout -q camera-build; fi

asan:
	@$(MAKE) -s -C host asan

# Confirm the engine still builds freestanding for the camera's core.
arm:
	@mkdir -p out/arm
	@for f in engine/*.c; do \
	  arm-none-eabi-gcc -c -std=c99 -Os -Wall -Wextra -Wno-unused-parameter \
	    -Wno-missing-field-initializers -mcpu=arm946e-s -marm -ffreestanding \
	    -fno-builtin -o out/arm/`basename $$f .c`.o $$f || exit 1; \
	done
	@arm-none-eabi-size -t out/arm/*.o | tail -1

contact: all
	@./filmdev contact $(firstword $(SAMPLES)) out/contact.jpg

clean:
	@$(MAKE) -s -C host clean
	@rm -rf out/arm
