#!/usr/bin/env python3
"""Host-side xor_chk. Same footer fix as build_tools/xor_chk.c."""
import struct
import sys

FOOTER = 0xCCCCCCCCE12FFF13


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("Invalid parameter count\n")
        return 1
    path = sys.argv[1]
    with open(path, "rb+") as f:
        data = f.read()
    if len(data) < 8 or (len(data) % 4) != 0:
        sys.stderr.write("xor_chk: bad length %d\n" % len(data))
        return 1
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    checksum = 0
    for w in words:
        checksum ^= w
    footer = struct.unpack_from("<Q", data, len(data) - 8)[0]
    if footer != FOOTER:
        sys.stderr.write("Footer magic error (expected 0x%X, got 0x%X)\n" % (FOOTER, footer))
        return 1
    words[-1] ^= checksum
    with open(path, "wb") as f:
        f.write(struct.pack("<%dI" % len(words), *words))
    return 0


if __name__ == "__main__":
    sys.exit(main())
