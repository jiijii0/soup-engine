#!/usr/bin/env python3
"""
Add a ROM dump to the working 2012 Magic Lantern build for the 5D Classic,
by patching the binary rather than rebuilding it.

Why patch instead of build: the ML source tree no longer compiles for this
body, and a from-source build is a large unknown. This changes 24 bytes of
a known-good binary.

How it works
------------
autoexec.bin is a loader plus a payload. The payload starts at file offset
0x1000 and is relocated to RESTARTSTART (0x00980000), so

    virtual address = 0x00980000 + (file offset - 0x1000)

Two things already exist in that payload:

  dump_with_buffer(addr, len, name)   0x00980518
      The port's own memory dumper. It stages through a 64 KB RAM buffer
      rather than handing the card driver a source address directly, and
      blinks the blue LED as it writes. Identified by its calls to memcpy
      (0xFFB44664), FIO_WriteFile (0xFFAE5B6C) and FIO_CloseFile
      (0xFFAE5B78), all matching stubs.S.

  the "Don't click me!" task         0x00987A18
      Reached from the Debug menu: that entry's .select is
      run_in_separate_task (0x009876B4, confirmed by its task_create call
      at 0xFFB18B40) and its .priv points here. The function is 36 bytes
      including its literal pool, nothing else branches into it, and it
      does nothing useful.

So we overwrite that task function with a call to dump_with_buffer. The
menu table is not touched at all: the entry already points here.

The filename reuses an existing string, "A:/0x0.BIN" at 0x009A4088, so no
new data has to be placed anywhere. Rename the file afterwards.

Result: Debug -> "Don't click me!" writes ROM1 (8 MB from 0xF8000000,
which is the mirror containing the firmware at 0xFF810000) to the card.
"""
import struct, sys, shutil, subprocess, os, tempfile

PAYLOAD_OFF      = 0x1000
RESTART          = 0x00980000
DUMP_WITH_BUFFER = 0x00980518
TARGET           = 0x00987A18   # the "Don't click me!" task function
TARGET_LIMIT     = 0x00987A3C   # next function starts here
FILENAME_STR     = 0x009A4088   # "A:/0x0.BIN", already in the binary
ROM1_BASE        = 0xF8000000
ROM0_BASE        = 0xF0000000
ROM_SIZE         = 0x00800000   # 8 MB

def va2off(va): return va - RESTART + PAYLOAD_OFF

def make_stub(rom_base):
    return f"""
    .arm
    .section .text
    .global _stub
_stub:
    push    {{lr}}
    ldr     r0, [pc, #12]
    mov     r1, #{ROM_SIZE:#x}
    ldr     r2, [pc, #8]
    bl      {DUMP_WITH_BUFFER:#x}
    pop     {{pc}}
    .word   {rom_base:#x}
    .word   {FILENAME_STR:#x}
"""

def assemble(rom_base=None):
    STUB = make_stub(ROM1_BASE if rom_base is None else rom_base)
    d = tempfile.mkdtemp()
    s, o, e = (os.path.join(d, n) for n in ("stub.S", "stub.o", "stub.elf"))
    open(s, "w").write(STUB)
    subprocess.run(["arm-none-eabi-as", "-mcpu=arm946e-s", "-o", o, s], check=True)
    subprocess.run(["arm-none-eabi-ld", "-Ttext", hex(TARGET), "-o", e, o], check=True)
    b = os.path.join(d, "stub.bin")
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", e, b], check=True)
    return open(b, "rb").read()

# --- mode 2: point the menu entry at dump_edmac instead -----------------
#
# dump_edmac() already exists in the binary at 0x00980820. It walks all 16
# EDMAC channels at 0xC0F04000 (0x100 stride, +8 = destination buffer,
# +0xC = packed size) and writes each channel's buffer to A:/<addr>.DAT.
#
# Reverse engineering the ROM gave us what those channels actually are:
#
#   CH 0   0xC0F04008   IMAGE_WRITE    <- the raw sensor image
#   CH 1   0xC0F04108   WBINTG_WRITE
#   CH 4   0xC0F04408   JPEG_WRITE
#   CH 5   0xC0F04508   YUV_WRITE / TWAIN_WRITE
#   CH 8   0xC0F04808   HIVSHD_READ
#   CH 9   0xC0F04908   DEFM_READ
#   CH 10  0xC0F04A08   READ(MEM1)
#
# This mode changes four bytes: the menu entry's .priv pointer. No code is
# overwritten at all.
DUMP_EDMAC  = 0x00980820
PRIV_FIELD  = 0x32728       # file offset of the entry's .priv
OLD_PRIV    = 0x00987A18    # the "Don't click me!" task

def patch_edmac(d):
    cur = struct.unpack_from("<I", d, PRIV_FIELD)[0]
    if cur != OLD_PRIV:
        print("REFUSING: .priv at file 0x%05X is 0x%08X, expected 0x%08X"
              % (PRIV_FIELD, cur, OLD_PRIV))
        return None
    struct.pack_into("<I", d, PRIV_FIELD, DUMP_EDMAC)
    print("  .priv at file 0x%05X: 0x%08X -> 0x%08X (dump_edmac)"
          % (PRIV_FIELD, cur, DUMP_EDMAC))
    print()
    print("  Shoot a frame FIRST, then open the menu and select the entry.")
    print("  Each live EDMAC channel is written to A:/<address>.DAT.")
    print("  Channel 0's buffer is the raw sensor image.")
    return d

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("usage: patch-romdump.py [--edmac] <in autoexec.bin> <out autoexec.bin>")
        print("       default  : Don't click me! -> dump 8 MB of ROM")
        print("       --edmac  : Don't click me! -> dump every EDMAC buffer")
        return 2
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    edmac = "--edmac" in sys.argv
    rom_base = ROM0_BASE if "--rom0" in sys.argv else ROM1_BASE
    src, dst = args[0], args[1]
    if edmac:
        d = bytearray(open(src, "rb").read())
        print("patching %s -> %s  (EDMAC buffer dump)" % (src, dst))
        d = patch_edmac(d)
        if d is None: return 1
        open(dst, "wb").write(bytes(d))
        return 0
    d = bytearray(open(src, "rb").read())

    # Refuse to patch anything that is not the binary we analysed.
    off = va2off(TARGET)
    before = bytes(d[off:off + 8])
    expect = struct.pack("<II", 0xE52DE004, 0xE59F0014)   # push {lr}; ldr r0,[pc,#20]
    if before != expect:
        print("REFUSING: 0x%08X does not contain the expected function." % TARGET)
        print("  found   %s" % before.hex(" "))
        print("  expected %s" % expect.hex(" "))
        print("  This is not the 2012 5D Classic build this patch was written for.")
        return 1

    stub = assemble(rom_base)
    room = TARGET_LIMIT - TARGET
    if len(stub) > room:
        print("REFUSING: stub is %d bytes, only %d available" % (len(stub), room))
        return 1

    d[off:off + len(stub)] = stub
    # leave the rest of the old function as it was; it is unreachable now
    open(dst, "wb").write(bytes(d))

    print("patched %s -> %s" % (src, dst))
    print("  overwrote %d bytes at va 0x%08X (file 0x%05X), %d bytes of room"
          % (len(stub), TARGET, off, room))
    print("  Debug -> \"Don't click me!\" now writes 8 MB from 0x%08X to A:/0x0.BIN"
          % rom_base)
    print()
    print("  stub:")
    for i in range(0, len(stub), 4):
        print("    0x%08X  %s" % (TARGET + i, stub[i:i+4].hex(" ")))
    return 0

if __name__ == "__main__":
    sys.exit(main())
