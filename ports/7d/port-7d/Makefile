# Magic Lantern source at changeset 7e5067c0a8cf (lua_fix.2020Dec28.7D203).
# Only its headers are used: make ML=/path/to/magic-lantern
ML ?= ../../magic-lantern
ENGINE := ../src/engine
CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
LD := $(CROSS)ld
NM := $(CROSS)nm
CFLAGS := -std=gnu99 -O2 -march=armv5te -marm -msoft-float -ffreestanding -fno-builtin -fno-common -fno-strict-aliasing -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fstack-usage -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers -MMD -MP
MLFLAGS := -DMODULE -DMODULE_NAME=soup7d -DCONFIG_MAGICLANTERN -D__ARM__ -I. -I$(ML)/platform/all -I$(ML)/src -I$(ML)/modules/lua/dietlibc/include
NAMES := se cr2 jpeg_dec jpeg_enc look stock wb color grade lut ui ui_font
OBJS := build/soup7d.o $(addprefix build/,$(addsuffix .o,$(NAMES)))
all: build/soup7d.mo
build:
	mkdir -p $@
# The module glue (menus, GUI, file handling) is sized for resident memory,
# not speed: every hot loop is in the engine objects, which stay -O2.
build/soup7d.o: soup7d.c module_strings.h | build
	$(CC) $(CFLAGS) -Os $(MLFLAGS) -I$(ENGINE) -c $< -o $@
build/%.o: $(ENGINE)/%.c | build
	$(CC) $(CFLAGS) -I$(ENGINE) -c $< -o $@
build/soup7d.mo: $(OBJS)
	$(LD) -r -o $@ $(OBJS)
	$(NM) -u $@ > build/imports.txt
-include $(OBJS:.o=.d)
