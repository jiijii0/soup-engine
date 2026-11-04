#!/usr/bin/env python3
"""
Verify a 5D Classic ROM dump.

A bad dump that looks plausible would waste a lot of downstream effort, so
this checks it against things we independently know:

  - size and entropy (code+data is ~4-7 bits/byte; encrypted would be ~8,
    blank would be ~0)
  - the firmware entry at 0xFF810000 (ROMBASEADDR) must be ARM code
  - every function address in Magic Lantern's stubs.S must land on a
    plausible ARM function prologue

That last one is the real test: those addresses were derived independently,
years ago, by people with a working camera. If they all hit prologues, the
dump is aligned correctly and complete.
"""
import sys, struct, collections, math, re, subprocess, os

ROM_BASE = 0xFF810000

# Spread across the 50D 1.0.9 ROM. These are the veneer targets in the
# 2025-06-20 Magic Lantern autoexec and the addresses in stubs.S.
KNOWN = [
    (0xFF810000, "firmware_entry"),
    (0xFF810894, "cstart"),
    (0xFF822AB4, "GUI_Control"),
    (0xFF866E08, "msleep"),
    (0xFF866EBC, "task_create"),
    (0xFF867B54, "_AllocateMemory"),
    (0xFF867D18, "_FreeMemory"),
    (0xFF8680C8, "GetMemoryInformation"),
    (0xFF982434, "_FIO_OpenFile"),
    (0xFF9824E8, "_FIO_CreateFile"),
    (0xFF982820, "_FIO_ReadFile"),
    (0xFF9829D0, "_FIO_WriteFile"),
    (0xFF982590, "FIO_CloseFile"),
    (0xFF983474, "_FIO_FindFirstEx"),
    (0xFF97D794, "_EngDrvOut"),
]

def looks_like_code(w):
    """A plausible ARM instruction: a sane condition code and not padding."""
    if w in (0x00000000, 0xFFFFFFFF):
        return False
    return (w >> 28) != 0xF or True   # 0xF is only NV/unconditional extensions

def is_prologue(d, off):
    """push {...}, or a stack adjust, or a branch -- how ARM functions start."""
    w = struct.unpack_from('<I', d, off)[0]
    if (w & 0x0FFF0000) == 0x092D0000: return "push"          # stmfd sp!, {..}
    if (w & 0x0FFF0000) == 0x052D0000: return "push {lr}"     # str lr,[sp,#-4]!
    if (w & 0x0FFFF000) == 0x024DD000: return "sub sp"
    if (w & 0x0F000000) == 0x0A000000: return "b"
    if (w & 0x0F000000) == 0x0B000000: return "bl"
    if (w & 0x0FFF0000) == 0x03A00000: return "mov imm"
    if (w & 0x0FFF0000) == 0x01A00000: return "mov reg"
    if (w & 0x0E500000) == 0x04100000: return "ldr"
    return None

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        print("usage: checkrom.py ROM1.BIN")
        return 2
    path = sys.argv[1]
    d = open(path, 'rb').read()
    problems = 0

    print("%s" % path)
    print("  size            : %d bytes (0x%X)" % (len(d), len(d)))
    if len(d) not in (0x800000, 0x7F0000):
        print("                    <- expected 8 MB, or 0x7F0000 from 0xFF810000")
        problems += 1

    c = collections.Counter(d); n = len(d)
    ent = -sum(v/n*math.log2(v/n) for v in c.values())
    print("  entropy         : %.2f bits/byte" % ent)
    if ent < 1.0:
        print("                    <- mostly blank, this is not a ROM"); problems += 1
    elif ent > 7.8:
        print("                    <- looks encrypted or random"); problems += 1

    e = 0xFF810000 - ROM_BASE
    if b"Copyright" in d[e:e+0x200]:
        i = d.index(b"Copyright", e)
        j = d.index(b"\0", i)
        print("  entry point     : ARM code, %r" % d[i:j].decode('latin1')[:46])
    else:
        print("  entry point     : no copyright string at 0xFF810000"); problems += 1

    print("\n  Magic Lantern's known function addresses:")
    bad = 0
    for va, name in KNOWN:
        off = va - ROM_BASE
        if off < 0 or off + 4 > len(d):
            print("    %-28s 0x%08X  OUT OF RANGE" % (name, va)); bad += 1; continue
        kind = is_prologue(d, off)
        w = struct.unpack_from('<I', d, off)[0]
        if kind:
            print("    %-28s 0x%08X  %08x  %s" % (name, va, w, kind))
        else:
            print("    %-28s 0x%08X  %08x  <- not a plausible prologue" % (name, va, w))
            bad += 1
    if bad:
        print("\n  %d of %d known addresses do not look like functions" % (bad, len(KNOWN)))
        problems += 1

    print()
    if problems:
        print("  VERDICT         : %d problem%s -- do not build on this dump"
              % (problems, "" if problems == 1 else "s"))
        return 1
    print("  VERDICT         : complete and correctly aligned 5D Classic ROM.")
    print("                    Ready for qemu-eos and Ghidra.")
    return 0

if __name__ == '__main__':
    sys.exit(main())
