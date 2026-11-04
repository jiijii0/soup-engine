#!/usr/bin/env python3
"""Downscale a binary PPM and write a PNG. Avoids any colour management
that an OS image tool might silently apply, so what you see is the bytes
the engine actually produced."""
import sys, zlib, struct

def read_ppm(p):
    f = open(p, 'rb')
    assert f.readline().strip() == b'P6', "expected binary PPM"
    l = f.readline()
    while l.startswith(b'#'):
        l = f.readline()
    w, h = map(int, l.split())
    int(f.readline())
    return w, h, f.read()

def box_downscale(d, w, h, tw):
    s = max(1, w // tw)
    ow, oh = w // s, h // s
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        for x in range(ow):
            acc = [0, 0, 0]
            for yy in range(s):
                base = ((y * s + yy) * w + x * s) * 3
                for xx in range(s):
                    i = base + xx * 3
                    acc[0] += d[i]; acc[1] += d[i+1]; acc[2] += d[i+2]
            n = s * s
            o = (y * ow + x) * 3
            out[o]   = acc[0] // n
            out[o+1] = acc[1] // n
            out[o+2] = acc[2] // n
    return ow, oh, bytes(out)

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    target = int(sys.argv[3]) if len(sys.argv) > 3 else 700
    w, h, d = read_ppm(src)
    ow, oh, od = box_downscale(d, w, h, target)
    write_png(dst, ow, oh, od)
    print(f"{src} {w}x{h} -> {dst} {ow}x{oh}")
