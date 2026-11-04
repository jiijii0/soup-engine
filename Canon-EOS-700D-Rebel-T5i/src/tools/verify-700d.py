#!/usr/bin/env python3
"""
verify-700d.py - is what the camera wrote what the desktop computes?

    tools/verify-700d.py samples/t5i/IMG_3999.CR2 out/t5i/camera/FLM_3999.JPG
    tools/verify-700d.py --all samples/t5i out/t5i/camera

Each FLM_ file the 700D writes carries what made it: the film's name in
its EXIF Software tag, the size in its frame header, and the chroma
sampling in its SOF0. So a camera file can be re-rendered from the raw
it came from with tools/previewcheck, with no record of which menu
settings were in force, and compared byte for byte. That comparison is
the port's whole claim -- the camera writes what the desktop computes
-- and this is the script that makes it, for every frame kept.

Needs out/previewcheck (make out/previewcheck) and the films in stocks/.
"""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
PREVIEWCHECK = os.path.join(SRC, "out", "previewcheck")
STOCKS = os.path.join(SRC, "stocks")
PREVIEW_W = 5184          # the 700D's embedded preview; scale = this / width


def jpeg_facts(path):
    """(software, width, height, chroma420) from the file's own headers."""
    d = open(path, "rb").read()
    software, width, height, c420 = None, None, None, None
    i = 2
    while i + 4 <= len(d) and d[i] == 0xFF:
        marker, n = d[i + 1], struct.unpack(">H", d[i + 2:i + 4])[0]
        seg = d[i + 4:i + 2 + n]
        if marker == 0xE1 and seg.startswith(b"Exif\0\0"):
            software = exif_software(seg[6:])
        elif marker == 0xC0:
            height, width = struct.unpack(">HH", seg[1:5])
            ncomp = seg[5]
            samp = [seg[6 + 3 * k + 1] for k in range(ncomp)]
            c420 = samp[0] == 0x22
        if marker == 0xDA:
            break
        i += 2 + n
    return software, width, height, c420


def exif_software(t):
    """Walk IFD0 for tag 0x0131 (Software), either byte order."""
    le = t[:2] == b"II"
    u16 = lambda o: struct.unpack("<H" if le else ">H", t[o:o + 2])[0]
    u32 = lambda o: struct.unpack("<I" if le else ">I", t[o:o + 4])[0]
    ifd = u32(4)
    for k in range(u16(ifd)):
        e = ifd + 2 + 12 * k
        if u16(e) == 0x0131:
            n, off = u32(e + 4), u32(e + 8)
            s = t[e + 8:e + 8 + n] if n <= 4 else t[off:off + n]
            return s.rstrip(b"\0").decode("ascii", "replace")
    return None


def stock_file(name):
    for f in sorted(os.listdir(STOCKS)):
        if not f.endswith(".fs"):
            continue
        for line in open(os.path.join(STOCKS, f), errors="replace"):
            if line.startswith("NAME"):
                if line.split(None, 1)[1].strip() == name:
                    return os.path.join(STOCKS, f)
                break
    return None


def verify(cr2, flm):
    software, w, h, c420 = jpeg_facts(flm)
    if not software or not w:
        return False, "no Software tag or frame header"
    fs = stock_file(software)
    if not fs:
        return False, "no film named %r in stocks/" % software
    scale = PREVIEW_W // w
    if scale * w != PREVIEW_W and scale * w + 1 != PREVIEW_W:
        return False, "width %d is not a scale of %d" % (w, PREVIEW_W)
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as tmp:
        ref = tmp.name
    try:
        args = [PREVIEWCHECK, cr2, fs, str(scale), ref] + (["420"] if c420 else [])
        out = subprocess.run(args, capture_output=True, text=True)
        if out.returncode != 0:
            return False, "previewcheck failed: %s" % out.stdout.strip()
        same = open(ref, "rb").read() == open(flm, "rb").read()
        m = re.search(r"look hash ([0-9A-F]+)", out.stdout)
        detail = "%s, %dx%d, %s, hash %s" % (software, w, h, "4:2:0" if c420 else "4:4:4",
                                             m.group(1) if m else "?")
        return same, detail + ("" if same else " -- FILES DIFFER")
    finally:
        os.unlink(ref)


def main(argv):
    if not os.path.exists(PREVIEWCHECK):
        sys.exit("build it first: make out/previewcheck")
    pairs = []
    if argv[:1] == ["--all"] and len(argv) == 3:
        samples, camera = argv[1], argv[2]
        for f in sorted(os.listdir(camera)):
            m = re.match(r"FLM_(\d{4})\.JPG$", f, re.I)
            if not m:
                continue
            cr2 = os.path.join(samples, "IMG_%s.CR2" % m.group(1))
            if os.path.exists(cr2):
                pairs.append((cr2, os.path.join(camera, f)))
    elif len(argv) == 2:
        pairs.append((argv[0], argv[1]))
    else:
        sys.exit(__doc__)
    bad = 0
    for cr2, flm in pairs:
        ok, detail = verify(cr2, flm)
        print("%s  %-14s %s" % ("ok  " if ok else "FAIL", os.path.basename(flm), detail))
        bad += not ok
    print("%d checked, %d differ" % (len(pairs), bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main(sys.argv[1:])
