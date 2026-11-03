#!/bin/bash
# build.sh <engine-dir> <out.elf>   same flags as the module's engine objects
export PATH=/opt/homebrew/bin:$PATH; E="$1"; O="$2"; D=$(mktemp -d)
CF="-g -std=gnu99 -O2 ${TUNE:--march=armv5te} -marm -msoft-float -ffreestanding -fno-builtin -fno-common -fno-strict-aliasing -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -w"
for f in se cr2 jpeg_dec jpeg_enc look stock wb color grade lut; do arm-none-eabi-gcc $CF -I"$E" -c "$E/$f.c" -o $D/$f.o || exit 1; done
arm-none-eabi-gcc $CF -I"$E" -c "$(dirname $0)/armbench.c" -o $D/bench.x || exit 1
arm-none-eabi-gcc -march=armv5te -msoft-float -nostdlib -T "$(dirname $0)/link.ld" -Wl,--gc-sections -Wl,--no-warn-rwx-segments -o "$O" $D/bench.x $D/*.o -lgcc || exit 1
arm-none-eabi-objdump -d "$O" > "$O.dis"
