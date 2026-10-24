# Soup Engine

# Which camera body the payload and simulator target. Platform headers and
# patch constants live under ml/platforms/$(PLATFORM)/.
PLATFORM ?= 50D.109
PLATDIR  = ml/platforms/$(PLATFORM)

# Raw files to test against. They live in samples/fromJpeg and
# samples/fromRaw now, grouped by which develop path suits them, so this
# looks in both and keeps the old flat layout working. When the samples
# moved, this matched nothing and simtest failed with "no preview",
# which says nothing about a missing sample.
SAMPLES := $(wildcard samples/*.CR2) $(wildcard samples/*/*.CR2)

.PHONY: all test test-full test-camera simtest asan arm clean contact payload

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
CAMTEST_CC = cc -std=c99 -O2 -DSE_HOSTED $(SDK) -I$(PLATDIR) -Iengine -Iml/payload

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
	@cc -std=c99 -O1 -g -DSE_HOSTED $(SDK) -Itools/camsim -I$(PLATDIR) -Iengine -o out/camsim \
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

# The camera build. Produces two files for the card:
#   ml/build/AUTOEXEC.BIN  Magic Lantern for $(PLATFORM), loader and hook patched in
#   ml/build/SE.BIN        the soup engine itself, loaded on demand
payload:
	@$(MAKE) -s -C ml/payload PLATFORM=$(PLATFORM)
	@python3 tools/patch-film.py --platform $(PLATFORM)

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
