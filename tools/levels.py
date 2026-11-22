"""Dump Virtua Cop's scripted spawns from PPJDD.EXE (read-only).

  python levels.py G:\\Games\\VCOP\\PPJDD.EXE [stage]

The output is in the format of the mod's spawns.txt: copy a [scene] block there and edit it.

Level data: 0x4AE020[stage] -> area table -> scene table (8 bytes per scene) -> spawn records
(0x24 bytes each, list ends with 0xFF), loaded into the spawn table 0x5555A0 by 0x4424D0.
"""
import struct
import sys

LEVELS = 0x4AE020
REC = 0x24


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        off = struct.unpack_from("<I", self.d, 0x3C)[0]
        n = struct.unpack_from("<H", self.d, off + 6)[0]
        opt = struct.unpack_from("<H", self.d, off + 20)[0]
        self.secs = []
        for i in range(n):
            s = off + 24 + opt + 40 * i
            vsz, va, rsz, raw = struct.unpack_from("<IIII", self.d, s + 8)
            self.secs.append((0x400000 + va, max(vsz, rsz), rsz, raw))

    def ok(self, va):
        return any(b <= va < b + sz for b, sz, _, _ in self.secs)

    def rd(self, va, n):
        for b, sz, rsz, raw in self.secs:
            if b <= va < b + sz:
                o = va - b
                return self.d[raw + o:raw + o + n] if o + n <= rsz else bytes(n)
        raise ValueError("bad address %08x" % va)

    def u32(self, va):
        return struct.unpack("<I", self.rd(va, 4))[0]


def dump_scene(pe, recs, out):
    """One spawns.txt line per record: pasting a scene back unchanged reproduces it."""
    i = 0
    while pe.ok(recs + i * REC) and pe.rd(recs + i * REC, 1)[0] != 0xFF and i < 64:
        r = recs + i * REC
        b = pe.rd(r, REC)
        x, y, z = struct.unpack_from("<3f", b, 0x0C)
        out.append("like=%08x wave=%d model=%d type=%d diff=%d x=%.2f y=%.2f z=%.2f angle=%d script=%08x"
                   " next=%d wait=%d drop=%d death=%d face=%d floor=%.2f" % (
                       r, b[1], b[4], b[0], b[0x0A], x, y, z, struct.unpack_from("<H", b, 0x18)[0],
                       struct.unpack_from("<I", b, 0x1C)[0], b[2], b[3], b[5], b[8], b[9],
                       struct.unpack_from("<f", b, 0x20)[0]))
        i += 1
    return i


def all_records(pe):
    for st in range(8):
        areas = pe.u32(LEVELS + 4 * st)
        if not pe.ok(areas):
            return
        for ar in range(32):
            scenes = pe.u32(areas + 4 * ar)
            if not pe.ok(scenes):
                break
            for sc in range(64):
                recs = pe.u32(scenes + 8 * sc)
                if not pe.ok(recs):
                    break
                i = 0
                while pe.rd(recs + i * REC, 1)[0] != 0xFF:
                    yield st, ar, sc, pe.rd(recs + i * REC, REC)
                    i += 1


def script_steps(pe, s):
    """header (reaction table, weapon) and the list of (action, animation, param, turn)"""
    react, weapon = struct.unpack("<2H", pe.rd(s, 4))
    steps, p = [], s + 4
    while len(steps) < 64:
        w = struct.unpack("<4h", pe.rd(p, 8))
        if w[0] == -1:
            break
        steps.append(w)
        p += 8
    return react & 0xFF, weapon & 0xFF, steps


def fmt_steps(steps):
    return ",".join(":".join(str(v) for v in (w if w[3] else w[:3] if w[2] else w[:2])) for w in steps)


def dump_scripts(pe):
    uses = {}
    for st, ar, sc, b in all_records(pe):
        uses.setdefault(struct.unpack_from("<I", b, 0x1C)[0], []).append((st, ar, sc, b[0], b[4]))
    print("# Virtua Cop behaviour scripts: script=<address> or copy the steps into steps=")
    for s in sorted(uses):
        react, weapon, steps = script_steps(pe, s)
        u = uses[s]
        print("%08x react=%d weapon=%d steps=%s" % (s, react, weapon, fmt_steps(steps)))
        print("         used %d times, type %s, models %s, first in scene %d %d %d" % (
            len(u), sorted(set(x[3] for x in u)), sorted(set(x[4] for x in u)), *u[0][:3]))


def main():
    pe = PE(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == "scripts":
        dump_scripts(pe)
        return
    only = int(sys.argv[2]) if len(sys.argv) > 2 else None
    out, total = [], 0
    for st in range(8):
        areas = pe.u32(LEVELS + 4 * st)
        if not pe.ok(areas):
            break
        if only is not None and st != only:
            continue
        for ar in range(32):
            scenes = pe.u32(areas + 4 * ar)
            if not pe.ok(scenes):
                break
            for sc in range(64):
                recs, extra = struct.unpack("<II", pe.rd(scenes + 8 * sc, 8))
                if not pe.ok(recs):
                    break
                out.append("")
                out.append("[scene %d %d %d]   # records %08x, scene value %d" % (st, ar, sc, recs, extra))
                total += dump_scene(pe, recs, out)
    print("# Virtua Cop spawns, in tdmod's spawns.txt format (%d records)" % total)
    print("\n".join(out))


if __name__ == "__main__":
    main()
