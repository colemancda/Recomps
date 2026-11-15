#!/usr/bin/env python3
"""Dump MapleStory.exe's image from memory once Themida has unpacked it.

  DISPLAY=:99 WINEPREFIX=... tools/dump_unpacked.py orig/MapleStory.exe OUTDIR [--oep 0x47c9df]

Starts the original (packed) client under Wine and polls the Windows process's
memory through /proc/PID/mem (Wine runs it as an ordinary Linux process). The
packed file holds encrypted code; the unpacker decrypts it in place before
jumping to the original entry point (OEP). As soon as the bytes at the OEP
decode as a normal function prologue, the whole image (ImageBase ..
ImageBase + SizeOfImage) is written to OUTDIR/image-N.bin, again a few times
over the next seconds (to see whether anything is still changing), and the
process is then killed. Nothing is patched or executed on the dump's behalf.
"""
import argparse
import os
import signal
import subprocess
import sys
import time

import pefile


def find_pid(name, after, base):
    """The Linux PID of the Wine process running `name` (started after `after`): the
    one with a PE image mapped at `base`, not the `wine` launcher."""
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            cmd = open(f"/proc/{d}/cmdline", "rb").read().replace(b"\0", b" ")
            if name.encode().lower() in cmd.lower() and os.stat(f"/proc/{d}").st_ctime >= after - 1:
                if b"dump_unpacked" not in cmd and read(int(d), base, 2) == b"MZ":
                    return int(d)
        except OSError:
            pass
    return None


def read(pid, addr, n):
    try:
        with open(f"/proc/{pid}/mem", "rb") as f:
            f.seek(addr)
            return f.read(n)
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--oep", type=lambda s: int(s, 0), default=0x47C9DF)
    ap.add_argument("--timeout", type=float, default=120)
    a = ap.parse_args()
    pe = pefile.PE(a.exe, fast_load=True)
    base, size = pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage
    oep = base + a.oep if a.oep < base else a.oep
    packed = pe.get_data(oep - base, 16)
    os.makedirs(a.out, exist_ok=True)
    print(f"image {base:#x}+{size:#x}, OEP {oep:#x}, packed bytes there: {packed.hex()}")

    t0 = time.time()
    exe = os.path.abspath(a.exe)
    proc = subprocess.Popen(["wine", exe], cwd=os.path.dirname(exe),
                            stdout=open(os.path.join(a.out, "wine.log"), "wb"), stderr=subprocess.STDOUT,
                            start_new_session=True)
    pid = None
    dumps = 0
    first = None
    try:
        while time.time() - t0 < a.timeout:
            if pid is None:
                pid = find_pid(os.path.basename(exe), t0, base)
                if pid:
                    print(f"process {pid} after {time.time() - t0:.1f} s")
            if pid:
                head = read(pid, oep, 16)
                if head is None:
                    if not os.path.exists(f"/proc/{pid}"):
                        print("the process ended")
                        break
                elif head != packed and head[:1] in (b"\x55", b"\x6a", b"\xe8", b"\x8b", b"\x83"):
                    if first is None:
                        first = time.time()
                        print(f"OEP decoded after {first - t0:.1f} s: {head.hex()}")
                    if time.time() - first >= dumps * 2.0:
                        img = bytearray()
                        for off in range(0, size, 0x1000):
                            page = read(pid, base + off, 0x1000)
                            img += page if page and len(page) == 0x1000 else b"\0" * 0x1000
                        path = os.path.join(a.out, f"image-{dumps}.bin")
                        open(path, "wb").write(img)
                        print(f"wrote {path} at +{time.time() - first:.1f} s")
                        dumps += 1
                        if dumps == 4:
                            break
            time.sleep(0.02)
    finally:
        os.killpg(proc.pid, signal.SIGKILL)
        subprocess.run(["wineserver", "-k"], check=False)
    if not dumps:
        print("no dump: the OEP never decoded")
        sys.exit(1)


if __name__ == "__main__":
    main()
