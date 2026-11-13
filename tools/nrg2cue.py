"""Convert a Nero .nrg image to .cue + per-track .bin files, plus a plain .iso of the data track.

Usage: python -I nrg2cue.py <image.nrg> <out_dir> [--info]
"""
import os
import struct
import sys

SUBCHANNEL = 96


def read_chunks(f, size):
    f.seek(size - 12)
    tail = f.read(12)
    if tail[:4] == b"NER5":
        pos = struct.unpack(">Q", tail[4:])[0]
        v2 = True
    else:
        f.seek(size - 8)
        tail = f.read(8)
        if tail[:4] != b"NERO":
            raise SystemExit("Not an NRG image: no NER5/NERO footer (file may be truncated or a renamed ISO).")
        pos = struct.unpack(">I", tail[4:])[0]
        v2 = False
    chunks = []
    f.seek(pos)
    while True:
        hdr = f.read(8)
        if len(hdr) < 8:
            break
        cid, clen = hdr[:4], struct.unpack(">I", hdr[4:])[0]
        if cid == b"END!":
            break
        chunks.append((cid, f.read(clen)))
    return v2, chunks


def parse_dao(data, v2):
    first, last = data[20], data[21]
    entry = 42 if v2 else 30
    tracks = []
    for i in range(last - first + 1):
        e = data[22 + i * entry: 22 + (i + 1) * entry]
        sector_size = struct.unpack(">H", e[12:14])[0]
        mode = e[14]
        if v2:
            pregap, start, end = struct.unpack(">QQQ", e[18:42])
        else:
            pregap, start, end = struct.unpack(">III", e[18:30])
        tracks.append(dict(num=first + i, sector_size=sector_size, mode=mode,
                           pregap=pregap, start=start, end=end))
    return tracks


def parse_tao(data, v2):
    # ETNF/ETN2: one entry per track: offset, length, mode, start_lba, unknown
    entry = 32 if v2 else 20
    tracks = []
    for i in range(len(data) // entry):
        e = data[i * entry:(i + 1) * entry]
        if v2:
            off, ln = struct.unpack(">QQ", e[:16])
            mode = struct.unpack(">I", e[16:20])[0]
        else:
            off, ln, mode = struct.unpack(">III", e[:12])
        sector_size = {0: 2048, 1: 2048, 3: 2336, 6: 2352, 7: 2352}.get(mode, 2352)
        tracks.append(dict(num=i + 1, sector_size=sector_size, mode=mode,
                           pregap=off, start=off, end=off + ln))
    return tracks


def is_audio(t):
    return t["mode"] in (0x07, 0x10)


def data_cue_mode(f, t):
    ss = t["sector_size"]
    if ss == 2048:
        return "MODE1/2048"
    if ss == 2336:
        return "MODE2/2336"
    # raw sector: byte 15 of the header is the mode
    f.seek(t["start"] + 15)
    return "MODE2/2352" if f.read(1) == b"\x02" else "MODE1/2352"


def msf(frames):
    return "%02d:%02d:%02d" % (frames // 4500, frames // 75 % 60, frames % 75)


def copy_sectors(f, out, begin, end, ss, strip_sub):
    f.seek(begin)
    remaining = end - begin
    keep = ss - SUBCHANNEL if strip_sub else ss
    sectors_per_block = 2048
    while remaining > 0:
        n = min(remaining, ss * sectors_per_block)
        buf = f.read(n)
        if strip_sub:
            buf = b"".join(buf[i:i + keep] for i in range(0, len(buf), ss))
        out.write(buf)
        remaining -= n


def main():
    src, out_dir = sys.argv[1], sys.argv[2]
    info_only = "--info" in sys.argv
    size = os.path.getsize(src)
    base = os.path.splitext(os.path.basename(src))[0]
    with open(src, "rb") as f:
        v2, chunks = read_chunks(f, size)
        print("NRG v%d, %d bytes, chunks: %s" % (2 if v2 else 1, size, " ".join(c[0].decode("latin1") for c in chunks)))
        tracks = []
        for cid, data in chunks:
            if cid in (b"DAOX", b"DAOI"):
                tracks += parse_dao(data, cid == b"DAOX")
            elif cid in (b"ETN2", b"ETNF") and not tracks:
                tracks += parse_tao(data, cid == b"ETN2")
        if not tracks:
            raise SystemExit("No track table (DAOX/DAOI/ETN2/ETNF) found.")
        for t in tracks:
            print("  track %2d  %s  sector=%d mode=0x%02x  pregap=%d start=%d end=%d  (%d sectors)" % (
                t["num"], "AUDIO" if is_audio(t) else "DATA ", t["sector_size"], t["mode"],
                t["pregap"], t["start"], t["end"], (t["end"] - t["pregap"]) // t["sector_size"]))
        if info_only:
            return

        os.makedirs(out_dir, exist_ok=True)
        cue = []
        for t in tracks:
            ss = t["sector_size"]
            strip = ss in (2448, 2432)
            out_ss = ss - SUBCHANNEL if strip else ss
            name = "%s (Track %02d).bin" % (base, t["num"])
            with open(os.path.join(out_dir, name), "wb") as out:
                copy_sectors(f, out, t["pregap"], t["end"], ss, strip)
            pregap_frames = (t["start"] - t["pregap"]) // ss
            if is_audio(t):
                mode = "AUDIO"
            else:
                tt = dict(t, sector_size=out_ss) if strip else t
                mode = data_cue_mode(f, tt) if not strip else ("MODE1/2352" if ss == 2448 else "MODE2/2352")
            cue.append('FILE "%s" BINARY' % name)
            cue.append("  TRACK %02d %s" % (t["num"], mode))
            if pregap_frames:
                cue.append("    INDEX 00 00:00:00")
            cue.append("    INDEX 01 %s" % msf(pregap_frames))

            if not is_audio(t) and t["num"] == tracks[0]["num"]:
                # plain 2048-byte ISO of the first data track, for mounters that only take .iso
                iso = os.path.join(out_dir, base + ".iso")
                with open(iso, "wb") as out:
                    if out_ss == 2048:
                        copy_sectors(f, out, t["start"], t["end"], ss, strip)
                    else:
                        user_off = 24 if mode.startswith("MODE2") else 16
                        f.seek(t["start"])
                        for _ in range((t["end"] - t["start"]) // ss):
                            sec = f.read(ss)
                            out.write(sec[user_off:user_off + 2048])
                print("wrote", iso)

        with open(os.path.join(out_dir, base + ".cue"), "w", newline="\r\n") as c:
            c.write("\n".join(cue) + "\n")
        print("wrote", os.path.join(out_dir, base + ".cue"), "+", len(tracks), "track .bin files")


if __name__ == "__main__":
    main()
