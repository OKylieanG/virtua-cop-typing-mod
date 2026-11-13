"""Read-only memory snapshot/diff tool for PPJDD.EXE (Virtua Cop).

  python -I vmem.py snap <name>                 save a snapshot of the game's data area
  python -I vmem.py read <addr> [len]           hex dump live memory
  python -I vmem.py words <addr> <count>        dump live memory as signed 16-bit words
  python -I vmem.py diff <a> <b> [<c> ...]      addresses (word-aligned) whose 16-bit value differs between every
                                                consecutive pair of snapshots
  python -I vmem.py same <a> <b>                addresses equal in both snapshots but nonzero
  python -I vmem.py gun                         print P1 gun X/Y and buttons
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

LO, HI = 0x480000, 0x12F5000
HERE = os.path.dirname(os.path.abspath(__file__))
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def find_pid():
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq PPJDD.EXE", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if parts[0].upper() == "PPJDD.EXE":
            return int(parts[1])
    raise SystemExit("PPJDD.EXE is not running")


def open_game():
    h = k32.OpenProcess(0x0010 | 0x0400, False, find_pid())  # VM_READ | QUERY_INFORMATION
    if not h:
        raise SystemExit("OpenProcess failed: %d" % ctypes.get_last_error())
    return h


def read(h, addr, n):
    out = bytearray(n)
    view = (ctypes.c_char * n).from_buffer(out)
    got = ctypes.c_size_t()
    # try one big read; on failure fall back to 64 KB blocks, then pages, leaving unreadable parts zeroed
    if k32.ReadProcessMemory(h, addr, view, n, ctypes.byref(got)):
        return bytes(out)
    for step in (0x10000, 0x1000):
        failed = False
        pos = 0
        while pos < n:
            chunk = min(step - ((addr + pos) % step), n - pos)
            dst = ctypes.byref(view, pos)
            if not k32.ReadProcessMemory(h, addr + pos, dst, chunk, ctypes.byref(got)):
                failed = True
            pos += chunk
        if not failed:
            break
    return bytes(out)


def snap_path(name):
    return os.path.join(HERE, "snap_%s.bin" % name)


def load(name):
    with open(snap_path(name), "rb") as f:
        return f.read()


def words(data):
    return struct.unpack("<%dh" % (len(data) // 2), data[:len(data) // 2 * 2])


def main():
    cmd = sys.argv[1]
    if cmd == "snap":
        h = open_game()
        data = read(h, LO, HI - LO)
        with open(snap_path(sys.argv[2]), "wb") as f:
            f.write(data)
        print("saved", sys.argv[2], len(data), "bytes")
    elif cmd == "read":
        h = open_game()
        addr = int(sys.argv[2], 16)
        n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x100
        data = read(h, addr, n)
        for i in range(0, n, 16):
            row = data[i:i + 16]
            print("%08x  %-48s %s" % (addr + i, " ".join("%02x" % b for b in row),
                                      "".join(chr(b) if 32 <= b < 127 else "." for b in row)))
    elif cmd == "words":
        h = open_game()
        addr = int(sys.argv[2], 16)
        n = int(sys.argv[3], 0)
        w = words(read(h, addr, n * 2))
        for i in range(0, n, 8):
            print("%08x  %s" % (addr + i * 2, " ".join("%6d" % v for v in w[i:i + 8])))
    elif cmd == "diff":
        snaps = [words(load(n)) for n in sys.argv[2:]]
        hits = [i for i in range(len(snaps[0]))
                if all(snaps[k][i] != snaps[k + 1][i] for k in range(len(snaps) - 1))]
        print(len(hits), "addresses changed across every snapshot")
        for i in hits[:200]:
            print("%08x  %s" % (LO + i * 2, " ".join("%6d" % s[i] for s in snaps)))
    elif cmd == "same":
        a, b = words(load(sys.argv[2])), words(load(sys.argv[3]))
        hits = [i for i in range(len(a)) if a[i] == b[i] and a[i] != 0]
        print(len(hits))
    elif cmd == "gun":
        h = open_game()
        rec = read(h, 0x5551C0, 0x18)
        btn, edge1, edge2, gx, gy = struct.unpack_from("<HHHhh", rec, 2)
        print("P1 gun x=%d y=%d buttons=%04x edges=%04x %04x" % (gx, gy, btn, edge1, edge2))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
