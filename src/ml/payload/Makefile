# The soup engine payload: a freestanding ARM blob for one camera body.
#
# This does not build Magic Lantern. It builds a standalone image that is
# spliced into a known-good AUTOEXEC.BIN by tools/patch-film.py. Camera-
# specific addresses live under ml/platforms/$(PLATFORM)/.

PLATFORM ?= 50D.109
PLATDIR  = ../platforms/$(PLATFORM)

CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
LD       = $(CROSS)ld
OBJCOPY  = $(CROSS)objcopy
OBJDUMP  = $(CROSS)objdump
NM       = $(CROSS)nm

ENGINE  = ../../engine

# -mcpu=arm946e-s: DIGIC II / DIGIC 4. No FPU and no hardware divide, so the
# engine is fixed-point throughout; -mno-unaligned-access because this core
# faults on unaligned word loads rather than fixing them up.
CFLAGS  = -std=c99 -Os -marm -mcpu=arm946e-s -mno-unaligned-access \
          -ffreestanding -fno-builtin -fno-common -fomit-frame-pointer \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -I$(PLATDIR) -I. -I$(ENGINE)

# Homebrew's arm-none-eabi-gcc ships no libc headers at all, so the few the
# engine needs (stdint.h, stddef.h) come from the compiler itself -- those are
# freestanding headers and always present. Nothing here includes a hosted one.

# Header dependencies, which this file went without for the whole project.
#
# `clean` has always deleted *.d, which says someone meant to do this;
# nothing ever generated them. So a change to engine/look.h or stock.h
# rebuilt only the .c files whose own timestamps moved, and every other
# object kept compiling against the PREVIOUS layout of structs it shares.
#
# That is not a slow build, it is a wrong one: adding hue_rot[6] and
# hue_lum[6] to se_look and se_stock shifted every field after them, and
# a stale object reading those structs reads the wrong offsets. The
# payload built that way was 36 bytes different from a clean build of the
# same source, and it went on a card before anyone noticed.
#
# The host build has had this since the beginning (host/Makefile), which
# is why the desktop suite stayed honest while this one quietly did not.
CFLAGS += -MMD -MP

LDFLAGS = -T $(PLATDIR)/payload.ld --gc-sections -nostdlib --no-warn-rwx-segments

# ARM946E-S has no FPU and no hardware divide instruction. The engine's
# per-pixel paths are all fixed-point, but its *setup* paths legitimately use
# float (baking a tone curve, deriving white-balance gains) and plain integer
# division, so the compiler emits calls to __aeabi_fmul, __aeabi_idiv and
# friends. Those live in libgcc, which is freestanding and self-contained --
# linking it pulls in a couple of KB of soft-float helpers and nothing else.
LIBGCC := $(shell $(CC) $(CFLAGS) -print-libgcc-file-name)

ENGINE_SRC = se.c wb.c cr2.c color.c debayer.c lut.c stock.c grade.c jpeg_enc.c jpeg_dec.c pipeline.c look.c ui.c ui_font.c
LOCAL_SRC  = film.c libc.c

OBJ = start.o $(LOCAL_SRC:.c=.o) $(ENGINE_SRC:.c=.o)
DEP = $(OBJ:.o=.d)

all: soupeng.bin

start.o: start.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# look.c is the per-pixel path, and -Os is the wrong trade for it.
#
# At -Os gcc emits a call to __aeabi_idiv for division even by a
# COMPILE-TIME CONSTANT, because the call is two instructions and the
# multiply-shift is six. ARM946E-S has no divide instruction, so that call
# is tens of cycles -- and the look divides several times per pixel over a
# million pixels. The build had four such calls in grade_row alone, for
# `/150`, `/100` and `/SE_HUE_FULL`.
#
# -O2 turns every constant divisor into a multiply-shift. The results are
# identical by construction, and tools/lookcheck.c proves it byte for byte
# against a reference built with completely different flags.
look.o: $(ENGINE)/look.c
	$(CC) $(CFLAGS) -O2 -c -o $@ $<

%.o: $(ENGINE)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

soupeng.elf: $(OBJ) $(PLATDIR)/payload.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJ) $(LIBGCC)
	@# A libgcc helper the engine did not expect would be an undefined
	@# symbol here; -nostdlib means the link would have failed already.
	@$(NM) $@ | grep -q ' U ' && { echo "undefined symbols:"; $(NM) $@ | grep ' U '; exit 1; } || true

soupeng.bin: soupeng.elf
	$(OBJCOPY) -O binary $< $@
	@echo "payload ($(PLATFORM)): $$(stat -f%z $@ 2>/dev/null || stat -c%s $@) bytes"

# What the blob costs, section by section. Worth watching: the budget is
# 160 KB of image plus a 32 KB stack above it.
size: soupeng.elf
	@$(CROSS)size -A $<
	@$(NM) -S --size-sort $< | tail -12

dis: soupeng.elf
	$(OBJDUMP) -d $<

clean:
	rm -f *.o *.d soupeng.elf soupeng.bin

.PHONY: all clean size dis

-include $(DEP)
