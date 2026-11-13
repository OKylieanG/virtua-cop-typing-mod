"""Minimal WOW64 hardware-breakpoint tracer for PPJDD.EXE.

  python -I hwbp.py <hexaddr> <len 1|2|4> <w|rw> [seconds=4] [maxhits=400]

Attaches as a debugger, sets DR0 on every thread, logs each instruction that touches the address
(EIP after the access + registers), then clears the breakpoints and detaches. The game keeps running.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
from collections import Counter

sys.path.insert(0, __import__("os").path.dirname(__file__))
from vmem import find_pid  # noqa: E402

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.Wow64GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.Wow64SetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ContinueDebugEvent.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD]
k32.WaitForDebugEvent.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.DebugBreakProcess.argtypes = [wt.HANDLE]

WOW64_CONTEXT_SIZE = 716
CTX_FULL_DEBUG = 0x10000 | 0x1 | 0x2 | 0x10   # control | integer | debug registers
# WOW64_CONTEXT offsets
O_FLAGS, O_DR0, O_DR6, O_DR7 = 0, 4, 20, 24
O_EDI, O_ESI, O_EBX, O_EDX, O_ECX, O_EAX, O_EBP, O_EIP = 156, 160, 164, 168, 172, 176, 180, 184
O_EFLAGS, O_ESP = 192, 196
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]

DBG_CONTINUE, DBG_NOT_HANDLED = 0x00010002, 0x80010001
EXC_SINGLE_STEP = {0x80000004, 0x4000001E}
EXC_BREAKPOINT = {0x80000003, 0x4000001F}


def get_ctx(th):
    buf = ctypes.create_string_buffer(WOW64_CONTEXT_SIZE)
    struct.pack_into("<I", buf, O_FLAGS, CTX_FULL_DEBUG)
    if not k32.Wow64GetThreadContext(th, buf):
        return None
    return buf


def set_dr(th, addr, dr7):
    buf = get_ctx(th)
    if buf is None:
        return False
    struct.pack_into("<I", buf, O_DR0, addr)
    struct.pack_into("<I", buf, O_DR6, 0)
    struct.pack_into("<I", buf, O_DR7, dr7)
    return bool(k32.Wow64SetThreadContext(th, buf))


def main():
    addr = int(sys.argv[1], 16)
    length = int(sys.argv[2])
    mode = sys.argv[3]
    seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 4
    maxhits = int(sys.argv[5]) if len(sys.argv) > 5 else 400
    # optional 6th arg: comma-separated EIPs to ignore (counted, but not toward maxhits)
    ignore = {int(x, 16) for x in sys.argv[6].split(",")} if len(sys.argv) > 6 else set()
    # modes: w = write, rw = read/write, x = execute (logs the two stack args of the function)
    rw = {"w": 0b01, "rw": 0b11, "x": 0b00}[mode]
    ln = 0b00 if mode == "x" else {1: 0b00, 2: 0b01, 4: 0b11}[length]
    dr7 = 1 | (rw << 16) | (ln << 18)

    pid = find_pid()
    if not k32.DebugActiveProcess(pid):
        raise SystemExit("DebugActiveProcess failed: %d" % ctypes.get_last_error())
    k32.DebugSetProcessKillOnExit(False)
    hproc = k32.OpenProcess(0x1F0FFF, False, pid)

    threads = {}
    hits = []
    ignored = Counter()
    ev = ctypes.create_string_buffer(256)
    armed = False
    deadline = None
    stopping = False
    try:
        while True:
            if not k32.WaitForDebugEvent(ev, 200):
                if deadline and time.time() > deadline and not stopping:
                    stopping = True
                    k32.DebugBreakProcess(hproc)
                continue
            code, _, tid = struct.unpack_from("<III", ev, 0)
            status = DBG_CONTINUE
            if code == 3:  # CREATE_PROCESS
                threads[tid] = struct.unpack_from("<Q", ev, 32)[0]
            elif code == 2:  # CREATE_THREAD
                threads[tid] = struct.unpack_from("<Q", ev, 16)[0]
                if armed and not stopping:
                    set_dr(threads[tid], addr, dr7)
            elif code == 4:  # EXIT_THREAD
                threads.pop(tid, None)
            elif code == 5:  # EXIT_PROCESS
                print("game exited")
                return
            elif code == 1:  # EXCEPTION
                exc = struct.unpack_from("<I", ev, 16)[0]
                if exc in EXC_BREAKPOINT:
                    if not armed:
                        ok = sum(set_dr(th, addr, dr7) for th in threads.values())
                        print("attached; armed DR0=%08x on %d/%d threads" % (addr, ok, len(threads)))
                        armed = True
                        deadline = time.time() + seconds
                    elif stopping:
                        for th in threads.values():
                            set_dr(th, 0, 0)
                        k32.ContinueDebugEvent(struct.unpack_from("<I", ev, 4)[0], tid, DBG_CONTINUE)
                        break
                elif exc in EXC_SINGLE_STEP and tid in threads:
                    c = get_ctx(threads[tid])
                    if c is not None:
                        regs = struct.unpack_from("<IIIIIIII", c, O_EDI)
                        esp = struct.unpack_from("<I", c, O_ESP)[0]
                        if mode == "x":
                            # record the call's first two stack args, and set the resume flag so the
                            # instruction runs instead of re-triggering the breakpoint
                            args = ctypes.create_string_buffer(12)
                            k32.ReadProcessMemory(hproc, ctypes.c_void_p(esp), args, 12, None)
                            hits.append((regs[7], regs, esp, struct.unpack("<III", args.raw)))
                            eflags = struct.unpack_from("<I", c, O_EFLAGS)[0]
                            struct.pack_into("<I", c, O_EFLAGS, eflags | 0x10000)
                        elif regs[7] in ignore:
                            ignored[regs[7]] += 1
                        else:
                            hits.append((regs[7], regs, esp))
                        struct.pack_into("<I", c, O_DR6, 0)
                        k32.Wow64SetThreadContext(threads[tid], c)
                    if len(hits) >= maxhits and not stopping:
                        stopping = True
                        k32.DebugBreakProcess(hproc)
                else:
                    status = DBG_NOT_HANDLED
            k32.ContinueDebugEvent(struct.unpack_from("<I", ev, 4)[0], tid, status)
    finally:
        k32.DebugActiveProcessStop(pid)
        print("detached")

    if ignored:
        print("ignored:", " ".join("%08x x%d" % kv for kv in ignored.items()))
    if mode == "x":
        calls = Counter((h[3][0], h[3][1], h[3][2]) for h in hits)
        print("%d calls; distinct (return, arg1, arg2):" % len(hits))
        for (ret, a1, a2), n in sorted(calls.items(), key=lambda kv: (kv[0][1], kv[0][2])):
            print("  ret=%08x arg1=%08x arg2=%08x x%d" % (ret, a1, a2, n))
        return
    counts = Counter(h[0] for h in hits)
    print("%d hits, %d distinct EIPs (EIP is the instruction AFTER the access)" % (len(hits), len(counts)))
    seen = set()
    for eip, regs, esp in hits:
        if eip in seen:
            continue
        seen.add(eip)
        edi, esi, ebx, edx, ecx, eax, ebp, _ = regs
        print("  eip=%08x x%-4d eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x" % (
            eip, counts[eip], eax, ebx, ecx, edx, esi, edi, ebp, esp))


if __name__ == "__main__":
    main()
