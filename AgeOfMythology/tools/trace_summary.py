#!/usr/bin/env python3
"""Summarise a Wine +relay,+d3d8,+loaddll trace of the game.

  tools/trace_summary.py TRACE.log GAMEDIR OUTDIR

Keeps only the relay calls made by threads of the game process whose return
address lies in the game's own images (the EXE and the DLLs shipped in
GAMEDIR), and the +d3d8 method traces. Wine's helper processes (explorer,
services) also run an EXE at 0x400000; their threads are told apart because
they never attach a game DLL. TRACE may be zstd-compressed (.zst). Writes into OUTDIR:

  modules.txt   load address of every module (from the PE DLL attach lines)
  imports.txt   callee API, calling game module, call count, first line
  sequence.txt  each API's first call, in order (the start-up sequence)
  d3d8.txt      d3d8 method, count, first line
  d3d8-seq.txt  the first 4000 d3d8 calls in order
"""
import collections
import os
import re
import subprocess
import sys

import pefile

CALL = re.compile(rb"^([0-9a-f]{4}):Call ([\w.]+)\((.*)\) ret=([0-9a-f]{8})$")
ATTACH = re.compile(rb'Call PE DLL \(proc=[0-9A-F]+,module=([0-9A-F]+) L"([^"]+)",reason=PROCESS_ATTACH')
D3D = re.compile(rb"^([0-9a-f]{4}):trace:d3d8:(\w+) (.*)$")


def main(trace, gamedir, outdir):
    os.makedirs(outdir, exist_ok=True)
    sizes = {}
    for fn in os.listdir(gamedir):
        if fn.lower().endswith((".exe", ".dll", ".asi")):
            try:
                pe = pefile.PE(os.path.join(gamedir, fn), fast_load=True)
            except pefile.PEFormatError:
                continue
            sizes[fn.lower()] = (pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage)

    exe = "aomx.exe"
    ranges = [(sizes[exe][0], sizes[exe][0] + sizes[exe][1], exe)]
    modules = {}
    imports = collections.Counter()
    first = {}
    order = []
    d3d = collections.Counter()
    d3dfirst = {}
    d3dseq = []
    n = 0

    def owner(addr):
        for lo, hi, name in ranges:
            if lo <= addr < hi:
                return name
        return None

    def lines():
        if trace.endswith(".zst"):
            pr = subprocess.Popen(["zstdcat", trace], stdout=subprocess.PIPE, bufsize=1 << 20)
            yield from pr.stdout
            pr.wait()
        else:
            with open(trace, "rb") as f:
                yield from f

    # Pass 1: the game's threads are those that attach one of its DLLs.
    game_dlls = {b"granny.dll", b"mss32.dll", b"rockalldll.dll", b"mp3dec.asi", b"ebueulax.dll"}
    tids = set()
    att = re.compile(rb'^([0-9a-f]{4}):Call PE DLL \(proc=[0-9A-F]+,module=[0-9A-F]+ L"([^"]+)"')
    for line in lines():
        if b"Call PE DLL" in line[:30]:
            m = att.match(line)
            if m and m.group(2).lower() in game_dlls:
                tids.add(m.group(1))
    print("game threads: %s" % b" ".join(sorted(tids)).decode())

    if True:
        for line in lines():
            n += 1
            line = line.rstrip(b"\n")
            if b":Call " in line[:12]:
                m = CALL.match(line)
                if m and m.group(1) in tids:
                    mod = owner(int(m.group(4), 16))
                    if mod:
                        key = (m.group(2).decode(), mod)
                        imports[key] += 1
                        if key not in first:
                            first[key] = n
                            order.append((n, key, m.group(3)[:120].decode("latin-1")))
                    continue
                m = ATTACH.search(line)
                if m:
                    base, name = int(m.group(1), 16), m.group(2).decode()
                    modules.setdefault(name.lower(), base)
                    if name.lower() in sizes and name.lower() != exe:
                        r = (base, base + sizes[name.lower()][1], name.lower())
                        if r not in ranges:
                            ranges.append(r)
                continue
            if b":trace:d3d8:" in line[:20]:
                m = D3D.match(line)
                if m and m.group(1) in tids:
                    fn = m.group(2).decode()
                    d3d[fn] += 1
                    d3dfirst.setdefault(fn, n)
                    if len(d3dseq) < 4000:
                        d3dseq.append("%d %s %s" % (n, fn, m.group(3)[:140].decode("latin-1")))

    with open(os.path.join(outdir, "modules.txt"), "w") as o:
        for name, base in sorted(modules.items(), key=lambda kv: kv[1]):
            game = " (game)" if name in sizes else ""
            pref = " preferred %08x" % sizes[name][0] if name in sizes else ""
            o.write("%08x %s%s%s\n" % (base, name, game, pref))
    with open(os.path.join(outdir, "imports.txt"), "w") as o:
        o.write("# %d trace lines, %d distinct (api, caller) pairs\n" % (n, len(imports)))
        for (api, mod), c in sorted(imports.items(), key=lambda kv: (kv[0][0].lower(), kv[0][1])):
            o.write("%-48s %-16s %10d  first@%d\n" % (api, mod, c, first[(api, mod)]))
    with open(os.path.join(outdir, "sequence.txt"), "w") as o:
        for ln, (api, mod), args in order:
            o.write("%10d %-16s %s(%s)\n" % (ln, mod, api, args))
    with open(os.path.join(outdir, "d3d8.txt"), "w") as o:
        for fn, c in sorted(d3d.items(), key=lambda kv: -kv[1]):
            o.write("%-40s %10d  first@%d\n" % (fn, c, d3dfirst[fn]))
    with open(os.path.join(outdir, "d3d8-seq.txt"), "w") as o:
        o.write("\n".join(d3dseq) + "\n")
    print("%d lines; %d api/caller pairs; %d d3d8 methods" % (n, len(imports), len(d3d)))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
