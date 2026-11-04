#!/usr/bin/env python3
"""
Render a dump of the camera's 4-bit overlay buffer as a PNG.

The buffer is 360 bytes x 240 rows (720x240 pixels, two per byte). On the
real screen those pixels are twice as tall as they are wide, so this
stretches vertically by 2 to show what the camera actually displays.
"""
import sys, zlib, struct

# Approximate Canon overlay palette. Index order follows Magic Lantern's
# colour constants; exact RGB should be confirmed against the real screen.
PAL = {
    # The sixteen colours the panel can show, so a preview rendered with
    # them is what the camera will actually display. These are the RGB each
    # word in ml/payload/film.c's se_palette[] was generated from -- see the
    # long note in engine/ui.h for the YUV conversion and why entry 15 is
    # pinned to Magic Lantern's marker value.
    #
    # Index 0 is transparent; the 5D Classic has no live view, so what shows
    # through it is the unlit panel, which is near black.
    0x00: ( 20,  18,  16),   # CLEAR   transparent -> the dark panel
    0x01: ( 30,  26,  23),   # INK     #1E1A17
    0x02: ( 61,  54,  47),   # SHADOW  #3D362F
    0x03: ( 94,  85,  74),   # SLATE   #5E554A
    0x04: (133, 122, 107),   # MID     #857A6B
    0x05: (171, 160, 146),   # EDGE    #ABA092
    0x06: (226, 217, 196),   # PAPER   #E2D9C4
    0x07: (242, 235, 218),   # HILITE  #F2EBDA
    0x08: (138,  59,  34),   # RUST    #8A3B22
    0x09: (192,  74,  46),   # RED     #C04A2E
    0x0A: (220, 138,  50),   # AMBER   #DC8A32
    0x0B: (217, 190,  74),   # GOLD    #D9BE4A
    0x0C: (107, 138,  78),   # OLIVE   #6B8A4E
    0x0D: ( 53, 115, 128),   # TEAL    #357380
    0x0E: (142,  86, 112),   # PLUM    #8E5670
    0x0F: (235, 235, 235),   # WHITE   Y=235, ML's marker word
}

W, H, PITCH = 720, 240, 360

def render(path, out, zoom=2, vstretch=2):
    d = open(path, 'rb').read()
    # 50D overlay: 960x480, one byte per pixel, already square.
    # 5D Classic UI buffer: 360x240, two 4-bit pixels per byte, double-tall.
    if len(d) >= 960 * 480:
        w, h, pitch, eight, stretch = 720, 480, 960, True, 1
    else:
        assert len(d) >= 360 * 240, f"short buffer: {len(d)}"
        w, h, pitch, eight, stretch = 720, 240, 360, False, vstretch
    ow, oh = w * zoom, h * stretch * zoom
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            if eight:
                c = d[x + y * pitch] & 0x0F
            else:
                b = d[(x >> 1) + y * pitch]
                c = (b >> 4) if (x & 1) else (b & 0x0F)
            r, g, bl = PAL[c]
            line += bytes((r, g, bl)) * zoom
        for _ in range(stretch * zoom):
            rows.append(bytes(line))
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, dd):
        c = t + dd
        return struct.pack('>I', len(dd)) + c + struct.pack('>I', zlib.crc32(c))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(out, 'wb').write(png)
    print(f"{path} -> {out}  {ow}x{oh}")

if __name__ == '__main__':
    render(sys.argv[1], sys.argv[2],
           int(sys.argv[3]) if len(sys.argv) > 3 else 1)
