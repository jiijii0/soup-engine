# Soup Engine

# Raw files to test against. They live in samples/fromJpeg and
# samples/fromRaw now, grouped by which develop path suits them, so this
# looks in both and keeps the old flat layout working. When the samples
# moved, this matched nothing and simtest failed with "no preview",
# which says nothing about a missing sample.
SAMPLES := $(wildcard samples/*.CR2) $(wildcard samples/*/*.CR2)

.PHONY: all test test-full test-camera simtest sim700d asan arm clean contact payload card-700d vendor-700d

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

# Run the real ml/700d module on the desktop, against a mock of Magic
# Lantern (tools/sim700d/ml.h stands in for ml/700d/ml.h) with a 3 MB DMA
# pool, short reads, a real worker thread and a real shutdown flag. It
# develops a frame and compares it with previewcheck byte for byte, then
# drives the watcher and the picker. See tools/sim700d/sim.c.
SIM700D_SRC = tools/sim700d/sim.c ml/700d/soupeng.c ml/700d/develop.c \
              ml/700d/watch.c ml/700d/screen.c ml/700d/ui.c ml/700d/prints.c
SIM700D_ENG = engine/se.c engine/wb.c engine/cr2.c engine/color.c engine/debayer.c \
              engine/lut.c engine/stock.c engine/grade.c engine/jpeg_enc.c \
              engine/jpeg_dec.c engine/pipeline.c engine/look.c

out/sim700d: $(SIM700D_SRC) tools/sim700d/ml.h ml/700d/soupeng.h $(SIM700D_ENG) $(wildcard engine/*.h)
	@mkdir -p out
	@# the module's sources see the mock through <ml.h>; the engine's see the host libc
	@for f in ml/700d/soupeng.c ml/700d/develop.c ml/700d/watch.c ml/700d/screen.c ml/700d/ui.c ml/700d/prints.c; do \
	   cc -std=gnu99 -O1 -g $(SDK) -Itools/sim700d -Iml/700d -Iengine -Wall -Wno-unused-function \
	      -c -o out/sim700d_`basename $$f .c`.o $$f || exit 1; done
	@cc -std=gnu99 -O1 -g $(SDK) -Itools/sim700d -Iengine -DSE_HOSTED -Wall -Wno-unused-function \
	   -c -o out/sim700d_sim.o tools/sim700d/sim.c
	@cc -std=c99 -O1 -g -DSE_HOSTED $(SDK) -Iengine -o $@ \
	   out/sim700d_sim.o out/sim700d_soupeng.o out/sim700d_develop.o out/sim700d_watch.o out/sim700d_screen.o out/sim700d_ui.o out/sim700d_prints.o \
	   $(SIM700D_ENG) -lm -lpthread 2>&1 | grep -v 'reducing alignment' || true
	@test -x $@

# With no raws in samples/ (which is every fresh clone: they are the owner's
# photos and are not committed) it runs on out/public.CR2, made from one frame
# of doc/stocks.jpg. SIM700D_SAMPLE=out/public.CR2 picks that one regardless,
# and it is the only sample whose SE_SIM_DUMP pictures are fit to publish.
SIM700D_SAMPLE ?= $(or $(firstword $(SAMPLES)),out/public.CR2)

sim700d: all out/previewcheck out/sim700d $(filter out/public.CR2,$(SIM700D_SAMPLE))
	@sh tools/sim700d.sh $(SIM700D_SAMPLE)

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

# The EOS 700D build: a Magic Lantern module, and everything the card
# needs beside it, in out/card-700d/. Copy that folder's contents onto a
# card that already has Magic Lantern's 700D build on it. Needs the
# pinned Magic Lantern checkout (vendor-700d below fetches it) and the
# Arm toolchain described in ml/700d/Makefile.
card-700d: vendor-700d
	@$(MAKE) -s -C ml/700d
	@rm -rf out/card-700d
	@mkdir -p out/card-700d/ML/modules out/card-700d/ML/FILM out/card-700d/ML/SETTINGS out/card-700d/ML/DATA
	@cp ml/700d/build/soupeng.mo out/card-700d/ML/modules/
	@cp ml/700d/data/SOUPUI.BIN out/card-700d/ML/DATA/
	@cp stocks/*.fs out/card-700d/ML/FILM/
	@touch out/card-700d/ML/SETTINGS/soupeng.en
	@# Magic Lantern's script writes user@host into a module, and these get
	@# handed around. The patch stops that; a checkout from before it does not.
	@if strings -a out/card-700d/ML/modules/soupeng.mo | grep -q "`whoami`@"; then \
	   echo "soupeng.mo carries your user name: delete vendor/magiclantern_simplified and run this again"; \
	   rm -rf out/card-700d; exit 1; fi
	@echo "out/card-700d: $$(ls out/card-700d/ML/FILM | wc -l | tr -d ' ') films, soupeng.mo $$(stat -f%z ml/700d/build/soupeng.mo 2>/dev/null || stat -c%s ml/700d/build/soupeng.mo) bytes"
	@echo "copy the contents of out/card-700d onto the card; eject before removing it"

# Magic Lantern's source, at the commit the camera's 2025-06-20 700D build
# was made from, with ml/700d/patches/ applied: three small fixes to their
# module build scripts for current Python and for a module living outside
# their tree. Nothing of theirs is committed here; this fetches it. Safe to
# run again: it does nothing once the checkout is at the right commit.
ML_700D_REPO   = https://github.com/reticulatedpines/magiclantern_simplified
ML_700D_COMMIT = ed3e7c0df
ML_700D_DIR    = vendor/magiclantern_simplified

vendor-700d:
	@if [ ! -d $(ML_700D_DIR)/.git ]; then \
	   echo "fetching Magic Lantern's source into $(ML_700D_DIR)"; \
	   git clone -q $(ML_700D_REPO) $(ML_700D_DIR); \
	 fi
	@cd $(ML_700D_DIR) && \
	 if ! git rev-parse -q --verify camera-build >/dev/null; then \
	   git checkout -q -b camera-build $(ML_700D_COMMIT) && \
	   git apply ../../ml/700d/patches/magiclantern-build-scripts.patch && \
	   git -c user.name=soup-engine -c user.email=soup@engine commit -qam \
	     "modules: build-script fixes from soup-engine/ml/700d/patches" && \
	   echo "Magic Lantern at $(ML_700D_COMMIT), patched"; \
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
