# Host build of the Soup Engine. Same sources as the camera build.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DSE_HOSTED
LDFLAGS ?= -lm

# This machine ships an SDK (27.0) newer than its linker (ld-1267), which
# chokes on the arm64e.x1 slice in the new .tbd stubs. Pin to the newest SDK
# the installed ld can actually read.
UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
  SDKROOT ?= $(shell for s in MacOSX26.5.sdk MacOSX26.sdk MacOSX15.sdk; do \
                       p=/Library/Developer/CommandLineTools/SDKs/$$s; \
                       if [ -d $$p ]; then echo $$p; break; fi; done)
  ifneq ($(SDKROOT),)
    CFLAGS  += -isysroot $(SDKROOT)
    LDFLAGS += -isysroot $(SDKROOT)
  endif
endif

# Track header dependencies. Without this a struct change recompiles only
# some objects and the link silently produces a binary whose halves disagree
# about a layout -- which presents as a crash in unrelated code.
CFLAGS += -MMD -MP

ENGINE_SRC := $(wildcard ../engine/*.c)
HOST_SRC   := main.c
SRC        := $(HOST_SRC) $(ENGINE_SRC)
OBJ        := $(SRC:.c=.o)
DEP        := $(OBJ:.o=.d)

BIN := ../filmdev

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build everything under AddressSanitizer and UBSan. These parsers read
# files off a card, so out-of-bounds reads are the failure mode that
# matters -- and on the camera there is no MMU to catch them.
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	  -Wno-missing-field-initializers -DSE_HOSTED -isysroot $(SDKROOT) \
	  -fsanitize=address,undefined -fno-omit-frame-pointer -MMD -MP" \
	  LDFLAGS="-lm -isysroot $(SDKROOT) -fsanitize=address,undefined"

clean:
	rm -f $(OBJ) $(DEP) $(BIN)

-include $(DEP)

.PHONY: all clean asan
