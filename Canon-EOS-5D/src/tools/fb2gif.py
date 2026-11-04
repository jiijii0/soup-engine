#!/usr/bin/env python3
"""
Turn a sequence of overlay framebuffers into an animated GIF.

The panel's buffer is already 4-bit indexed into a 16-colour palette,
which is exactly what GIF wants -- so the pixels go in untouched and the
palette goes in the global colour table. Nothing is resampled and no
colour is invented; this is literally what the camera puts on screen.

The buffer is 720x240 presented over 480 lines, so every row is emitted
twice. That is the panel's own geometry, not a scaling choice.

No PIL, no ffmpeg: a GIF89a writer with its own LZW, because this machine
has none of those and a README animation is not worth a dependency.
"""
import sys, struct

W, H, PITCH = 720, 240, 360

# The same sixteen colours tools/fb2png.py uses; see engine/ui.h for where
# they came from and why entry 15 is Magic Lantern's marker.
PAL = [(20,18,16),(30,26,23),(61,54,47),(94,85,74),(133,122,107),
       (171,160,146),(226,217,196),(242,235,218),(138,59,34),(192,74,46),
       (220,138,50),(217,190,74),(107,138,78),(53,115,128),(142,86,112),
       (235,235,235)]


def lzw(pixels, min_code_size):
    """GIF's variable-width LZW. Returns the packed code stream."""
    clear, eoi = 1 << min_code_size, (1 << min_code_size) + 1
    width = min_code_size + 1
    table = {bytes([i]): i for i in range(1 << min_code_size)}
    nxt = eoi + 1

    out = bytearray()
    bits = 0
    nbits = 0

    def emit(code, w):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += w
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8

    emit(clear, width)
    cur = b''
    for px in pixels:
        nx = cur + bytes([px])
        if nx in table:
            cur = nx
            continue
        emit(table[cur], width)
        table[nx] = nxt
        nxt += 1
        if nxt > (1 << width):
            width += 1
        if nxt == 4096:                      # the table is full: start over
            emit(clear, width)
            table = {bytes([i]): i for i in range(1 << min_code_size)}
            nxt = eoi + 1
            width = min_code_size + 1
        cur = bytes([px])
    if cur:
        emit(table[cur], width)
    emit(eoi, width)
    if nbits:
        out.append(bits & 0xFF)
    return bytes(out)


def blocks(data):
    """GIF carries image data in sub-blocks of at most 255 bytes."""
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


# ---------------------------------------------------------- the camera
#
# A schematic camera back to sit the screen in, drawn rather than
# photographed: it costs nothing, it needs no one else's picture, and it
# uses the panel's own palette so the whole frame stays 16 colours and
# GIF stays the right format for it.
CAM_W, CAM_H = 1060, 700
SCR_X, SCR_Y = 130, 130

BODY, DARK, EDGE, LABEL = 2, 1, 3, 4


def rect(cv, x, y, w, h, c):
    for yy in range(max(0, y), min(CAM_H, y + h)):
        row = yy * CAM_W
        for xx in range(max(0, x), min(CAM_W, x + w)):
            cv[row + xx] = c


def disc(cv, cx, cy, r, c):
    for yy in range(max(0, cy - r), min(CAM_H, cy + r + 1)):
        dy = yy - cy
        row = yy * CAM_W
        for xx in range(max(0, cx - r), min(CAM_W, cx + r + 1)):
            dx = xx - cx
            if dx * dx + dy * dy <= r * r:
                cv[row + xx] = c


def camera_back(screen):
    """screen is 720x480 bytes; returns the full CAM_W x CAM_H canvas."""
    cv = bytearray([BODY]) * (CAM_W * CAM_H)

    rect(cv, 0, 0, CAM_W, 6, DARK)                     # top edge
    rect(cv, 0, CAM_H - 10, CAM_W, 10, DARK)           # bottom edge
    rect(cv, 340, 10, 380, 56, DARK)                   # viewfinder hump
    rect(cv, 366, 24, 328, 30, EDGE)                   # eyepiece

    # the screen, with a bezel
    rect(cv, SCR_X - 14, SCR_Y - 14, 720 + 28, 480 + 28, DARK)
    rect(cv, SCR_X - 4, SCR_Y - 4, 720 + 8, 480 + 8, EDGE)

    for i in range(4):                                  # buttons, left
        rect(cv, 28, 200 + i * 66, 72, 34, DARK)
        rect(cv, 32, 204 + i * 66, 64, 26, EDGE)

    disc(cv, 960, 220, 62, DARK)                        # the wheel
    disc(cv, 960, 220, 50, BODY)
    disc(cv, 960, 220, 16, DARK)
    for i in range(3):                                  # buttons, right
        rect(cv, 924, 376 + i * 62, 72, 34, DARK)
        rect(cv, 928, 380 + i * 62, 64, 26, EDGE)

    rect(cv, SCR_X, SCR_Y + 500, 150, 10, LABEL)        # a hint of branding

    for y in range(480):                                # drop the screen in
        src = y * 720
        dst = (SCR_Y + y) * CAM_W + SCR_X
        cv[dst:dst + 720] = screen[src:src + 720]
    return bytes(cv)


def unpack(fb):
    """4bpp buffer -> one byte per pixel, each row doubled for the panel."""
    rows = []
    for y in range(H):
        row = bytearray(W)
        base = y * PITCH
        for x in range(0, W, 2):
            b = fb[base + (x >> 1)]
            row[x] = b & 0x0F
            row[x + 1] = b >> 4
        rows.append(bytes(row))
        rows.append(bytes(row))          # 720x240 is shown over 480 lines
    return b''.join(rows)


def write_gif(paths, out_path, delay_cs, in_camera=False):
    ow, oh = (CAM_W, CAM_H) if in_camera else (W, H * 2)
    g = bytearray(b'GIF89a')
    g += struct.pack('<HH', ow, oh)
    g += bytes([0xF3, 0, 0])             # global table, 16 entries, 4 bpp
    for r, gr, b in PAL:
        g += bytes([r, gr, b])
    g += b'\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00'   # loop forever

    for p in paths:
        fb = open(p, 'rb').read()
        if len(fb) < PITCH * H:
            sys.exit('%s is %d bytes, expected %d' % (p, len(fb), PITCH * H))
        px = unpack(fb)
        if in_camera:
            px = camera_back(px)
        g += b'\x21\xF9\x04\x00' + struct.pack('<H', delay_cs) + b'\x00\x00'
        g += b'\x2C' + struct.pack('<HHHH', 0, 0, ow, oh) + b'\x00'
        g += bytes([4]) + blocks(lzw(px, 4))

    g += b'\x3B'
    open(out_path, 'wb').write(g)
    return len(g)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit('usage: fb2gif.py <out.gif> <delay_cs> <frame.fb> ...')
    cam = '--camera' in sys.argv
    argv = [a for a in sys.argv if a != '--camera']
    out, delay, frames = argv[1], int(argv[2]), argv[3:]
    n = write_gif(frames, out, delay, cam)
    ow, oh = (CAM_W, CAM_H) if cam else (W, H * 2)
    print('wrote %s  %d frames, %dx%d, %d KB' % (out, len(frames), ow, oh, n >> 10))
