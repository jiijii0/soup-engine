#!/usr/bin/env python3
"""
Merge several ROM dumps, resolving the corruption dump_with_buffer leaves
behind.

dump_with_buffer() copies the ROM through a single 64 KB staging buffer:

    memcpy(buf, address, 0x10000);
    FIO_WriteFile(f, buf, 0x10000);

If FIO_WriteFile returns before the card has actually consumed the buffer,
the next memcpy overwrites the start of it while it is still being written.
That corrupts the first few dozen bytes of some 64 KB chunks, and which
chunks are hit varies from run to run.

Two dumps of the same 5D Classic ROM taken minutes apart disagreed on 297
bytes, every one within 64 bytes of a 64 KB boundary. That is the
signature.

With two dumps the disagreements can only be flagged. With three or more,
a per-byte majority vote resolves them, because the corruption lands in
different places each time.
"""
import sys, collections

CHUNK = 0x10000

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        print("usage: mergedumps.py <out.bin> <dump1> <dump2> [dump3 ...]")
        return 2
    out, ins = sys.argv[1], sys.argv[2:]
    dumps = [open(p, 'rb').read() for p in ins]
    n = len(dumps[0])
    if any(len(d) != n for d in dumps):
        print("dumps differ in size"); return 1

    print("%d dumps of %d bytes" % (len(dumps), n))

    disagree = [i for i in range(n) if len({d[i] for d in dumps}) > 1]
    print("bytes where the dumps disagree: %d" % len(disagree))
    near = sum(1 for i in disagree if (i % CHUNK) < 64)
    print("  within 64 bytes of a 64 KB chunk start: %d (%.1f%%)"
          % (near, 100.0*near/len(disagree) if disagree else 0))

    # Only vote on the corruption signature. Some of the ROM genuinely
    # changes between dumps -- the property store at 0xFFFA0000-0xFFFC0000
    # is a flash log that the camera appends to -- and majority-voting
    # across dumps taken at different times would corrupt that for real.
    # The last dump listed is taken as authoritative for those bytes.
    merged = bytearray(dumps[-1])
    resolved = unresolved = skipped = 0
    for i in disagree:
        if (i % CHUNK) >= 64:
            skipped += 1          # a real change, not the staging-buffer race
            continue
        votes = collections.Counter(d[i] for d in dumps)
        best, count = votes.most_common(1)[0]
        if count > len(dumps) / 2:
            merged[i] = best
            resolved += 1
        else:
            unresolved += 1
    open(out, 'wb').write(bytes(merged))

    print("\nat chunk starts, resolved by majority : %d" % resolved)
    print("at chunk starts, still ambiguous      : %d" % unresolved)
    print("genuine changes, taken from %-13s: %d" % (ins[-1].split('/')[-1], skipped))
    if len(dumps) < 3:
        print("\n  Two dumps cannot vote. Take a third and run this again;")
        print("  the corruption lands in different chunks each time.")
    print("\nwrote %s" % out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
