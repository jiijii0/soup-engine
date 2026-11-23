# soup.mo -- Soup Engine film looks as a Magic Lantern module (EOS 600D, fw 1.0.2).
#
# Build with src/tools/build-600d.sh, which copies this directory into a
# Magic Lantern tree as modules/soup and passes SOUP_ENGINE.

MODULE_NAME = soup

SOUP_ENGINE ?= ../../../soup-engine/src/engine
ENGINE_SRC  = se cr2 jpeg_dec jpeg_enc look stock wb color grade lut ui ui_font

MODULE_OBJS = $(BUILD_DIR)/soup.o $(BUILD_DIR)/soup_dev.o $(BUILD_DIR)/soup_ui.o $(addprefix $(BUILD_DIR)/se_,$(addsuffix .o,$(ENGINE_SRC)))

TOP_DIR = ../..

include $(TOP_DIR)/modules/Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/module_complete

CFLAGS += -I$(SOUP_ENGINE)

# Optional per-build switches for the 600D port, e.g.
#   make SOUP_EXTRA="-DSOUP_PLAY_PROBE"
#   make SOUP_EXTRA="-DSOUP_PLAY_FOLDER_FILE_ADDR=0x12345 -DSOUP_SHOOT_SET_OPENS_PICKER=0"
# (build-600d.sh passes SOUP_EXTRA through from the environment.)
SOUP_EXTRA ?=
CFLAGS += $(SOUP_EXTRA)

# The engine is built without ML's -ffast-math / -fsingle-precision-constant
# so its float setup code (curve baking) matches the desktop build that the
# looks are designed and checked on.
#
# The engine is freestanding C99. Its per-pixel files want -O2: at -Os gcc
# calls __aeabi_idiv even for constant divisors, and ARM946E-S has no
# divide instruction (see src/ml/payload/Makefile upstream).
$(BUILD_DIR)/se_%.o: $(SOUP_ENGINE)/%.c | $(BUILD_DIR)
	$(CC) $(filter-out -ffast-math -fsingle-precision-constant -Werror -Wdouble-promotion -Wextra,$(CFLAGS)) -std=gnu99 $(if $(filter look jpeg_dec jpeg_enc,$*),-O2) -c -o $@ $<
