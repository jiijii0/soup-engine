#!/usr/bin/env python3
"""
Catch the raw sensor buffer while the DMA is still live.

Dumping from the menu after a shot gets stale memory: Canon tears the EDMAC
descriptor down long before the menu is reachable. The address register
still holds a value, which is why a file appears, but the size register has
been reset and the memory no longer holds a frame.

So instead of dumping on demand, this installs a POLLING TASK. You start it
from the menu, it runs in the background while you shoot, and it dumps the
moment the DMA descriptor is configured for a real frame.

Where the numbers come from
---------------------------
Reverse engineering the ROM gave the EDMAC layout (see docs/rom-findings.md):

    channel 0 = IMAGE_WRITE, the sensor image
    0xC0F04008  destination address
    0xC0F0400C  size, packed as (height-1) << 16 | width_in_bytes

A real 5D frame is 2954 lines. Nothing else in the pipeline is anywhere
near that tall, so "height-1 > 1024" is an unambiguous trigger.

What it patches
---------------
Two things, both in dead code:

  0x00981C94  a 92-byte function whose ONLY caller is run_test, which this
              patch makes unreachable. The polling routine goes here.
  file 0x32728  the "Don't click me!" menu entry's .priv pointer, repointed
              from run_test to the new routine.

run_test itself is left untouched; it simply stops being reachable.
"""
import struct, sys, subprocess, os, tempfile

RESTART          = 0x00980000
PAYLOAD_OFF      = 0x1000
DEAD_FUNC        = 0x00981C94   # dead once run_test is unreachable
DEAD_LIMIT       = 0x00981CF0   # next function starts here
DEAD2_FUNC       = 0x00987A18   # run_test itself, now unreachable
DEAD2_LIMIT      = 0x00987A3C
PRIV_FIELD       = 0x32728      # file offset of the menu entry's .priv
OLD_PRIV         = 0x00987A18   # run_test
DUMP_WITH_BUFFER = 0x00980518
MSLEEP           = 0xFFB187F4
EDMAC_CH0_ADDR   = 0xC0F04008
FILENAME_STR     = 0x009A4088   # "A:/0x0.BIN", already in the binary
DUMP_BYTES       = 0x00100000   # 1 MB: ~117 frame lines, enough to prove it
SETTLE_MS        = 4096         # let Canon finish writing the CR2 first

def va2off(va): return va - RESTART + PAYLOAD_OFF

# The routine is split across the two dead regions. The polling loop and
# the settle delay live in the 92-byte one; the dump lives in run_test's
# 36 bytes, reached by a branch.
STUB_A = f"""
    .arm
    .section .text
    .global _stub
_stub:
    push    {{r4, r5, r6, r7, lr}}
    ldr     r4, lit_reg             @ 0xC0F04008, channel 0 address register
    mov     r5, #2048               @ give up after ~30 s
poll:
    ldr     r6, [r4, #4]            @ size register at 0xC0F0400C
    mov     r7, r6, lsr #16         @ height - 1
    cmp     r7, #1024               @ a real frame is 2954 lines
    bhi     grab
    mov     r0, #10
    bl      {MSLEEP:#x}
    subs    r5, r5, #1
    bne     poll
    pop     {{r4, r5, r6, r7, pc}}  @ timed out, give up quietly
grab:
    ldr     r4, [r4]                @ latch the live buffer address NOW
    mov     r0, #{SETTLE_MS:#x}     @ ...then stay off the card while Canon
    bl      {MSLEEP:#x}             @    finishes writing the CR2
    b       {DEAD2_FUNC:#x}         @ continue in the other dead region
lit_reg:
    .word   {EDMAC_CH0_ADDR:#x}
"""

STUB_B = f"""
    .arm
    .section .text
    .global _stub_b
_stub_b:
    mov     r0, r4                  @ the address latched during capture
    mov     r1, #{DUMP_BYTES:#x}
    ldr     r2, lit_name
    bl      {DUMP_WITH_BUFFER:#x}
    pop     {{r4, r5, r6, r7, pc}}  @ returns through _stub's frame
lit_name:
    .word   {FILENAME_STR:#x}
"""

def assemble(src, at):
    d = tempfile.mkdtemp()
    s, o, e, b = (os.path.join(d, n) for n in ("s.S", "s.o", "s.elf", "s.bin"))
    open(s, "w").write(src)
    subprocess.run(["arm-none-eabi-as", "-mcpu=arm946e-s", "-o", o, s], check=True)
    subprocess.run(["arm-none-eabi-ld", "-Ttext", hex(at), "-o", e, o], check=True)
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", e, b], check=True)
    return open(b, "rb").read()

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("usage: patch-capture.py <in autoexec.bin> <out autoexec.bin>")
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, "rb").read())

    cur = struct.unpack_from("<I", d, PRIV_FIELD)[0]
    if cur != OLD_PRIV:
        print("REFUSING: .priv at file 0x%05X is 0x%08X, expected 0x%08X"
              % (PRIV_FIELD, cur, OLD_PRIV))
        return 1
    off = va2off(DEAD_FUNC)
    head = struct.unpack_from("<I", d, off)[0]
    if head != 0xE92D4030:      # push {r4, r5, lr}
        print("REFUSING: 0x%08X starts with %08x, expected e92d4030"
              % (DEAD_FUNC, head))
        return 1

    a = assemble(STUB_A, DEAD_FUNC)
    b = assemble(STUB_B, DEAD2_FUNC)
    for blob, at, lim, nm in ((a, DEAD_FUNC, DEAD_LIMIT, "poll+settle"),
                              (b, DEAD2_FUNC, DEAD2_LIMIT, "dump")):
        if len(blob) > lim - at:
            print("REFUSING: %s is %d bytes, only %d available at 0x%08X"
                  % (nm, len(blob), lim - at, at))
            return 1
    d[va2off(DEAD_FUNC):va2off(DEAD_FUNC) + len(a)] = a
    d[va2off(DEAD2_FUNC):va2off(DEAD2_FUNC) + len(b)] = b
    struct.pack_into("<I", d, PRIV_FIELD, DEAD_FUNC)
    open(dst, "wb").write(bytes(d))

    print("patched %s -> %s" % (src, dst))
    print("  poll+settle: %d bytes at 0x%08X (room %d)" % (len(a), DEAD_FUNC, DEAD_LIMIT-DEAD_FUNC))
    print("  dump       : %d bytes at 0x%08X (room %d)" % (len(b), DEAD2_FUNC, DEAD2_LIMIT-DEAD2_FUNC))
    print("  .priv at file 0x%05X: 0x%08X -> 0x%08X" % (PRIV_FIELD, cur, DEAD_FUNC))
    print()
    print("  Select the menu entry, then SHOOT within ~30 seconds.")
    print("  It latches the buffer address during capture, waits %d ms for" % SETTLE_MS)
    print("  Canon to finish writing the CR2, then writes 1 MB to A:/0x0.BIN.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
