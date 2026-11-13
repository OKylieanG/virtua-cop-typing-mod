"""Dump live Virtua Cop object table (read-only)."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from vmem import open_game, read  # noqa: E402

POOL, STRIDE, COUNT = 0x12C7890, 0x4AC, 0x12CE409


def xf(m, p):
    """Apply a game 3x4 matrix (same layout as 0x41AC60) to point p."""
    x, y, z = p
    return (m[0] * x + m[3] * y + m[6] * z + m[9],
            m[1] * x + m[4] * y + m[7] * z + m[10],
            m[2] * x + m[5] * y + m[8] * z + m[11])


def main():
    h = open_game()
    n = read(h, COUNT, 1)[0]
    f = struct.unpack("<f", read(h, 0x55C4D4, 4))[0]
    cam_ptr = struct.unpack("<I", read(h, 0x549030, 4))[0]
    cam = struct.unpack("<12f", read(h, 0x549050, 48))
    print("objects=%d focal=%.3f stacktop@%08x camera rot=%s t=(%.1f %.1f %.1f)" % (
        n, f, cam_ptr, " ".join("%.3f" % v for v in cam[:9]), *cam[9:]))
    gun = struct.unpack("<hh", read(h, 0x556234, 4))
    print("P1 aim from center: (%d,%d)" % gun)
    for i in range(n):
        base = POOL + i * STRIDE
        o = read(h, base, STRIDE)
        state = struct.unpack_from("<I", o, 0)[0]
        if state == 0:
            continue
        cb = struct.unpack_from("<I", o, 0x480)[0]
        mask = struct.unpack_from("<I", o, 0x484)[0]
        nbox = struct.unpack_from("<b", o, 0x4A0)[0]
        flags = o[0x4A9]
        skip = o[0x47C]
        who = o[0x4A5]
        m = struct.unpack_from("<12f", o, 0x29C)
        tx, ty, tz = xf(cam, m[9:])
        scr = ("(%6.1f,%6.1f)" % (tx * f / tz, ty * f / tz)) if tz > 0.1 else "   behind   "
        print("#%2d st=%08x %08x cb=%06x flags=%02x skip=%d boxes=%d mask=%08x hitby=%d bone0 t=(%8.1f %8.1f %8.1f) scr=%s" % (
            i, state, base, cb, flags, skip, nbox, mask, who, tx, ty, tz, scr))


if __name__ == "__main__":
    main()
