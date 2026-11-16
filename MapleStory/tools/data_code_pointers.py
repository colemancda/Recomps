#!/usr/bin/env python3
"""Find code-pointer targets in a PE's data sections that a function list misses.

  tools/data_code_pointers.py GMSv62.exe build/survey/GMSv62.functions.csv > extra.csv

MapleStory's client (MSVC, C++) keeps large tables of function pointers in
.data and .rdata: the CRT static-initialiser array and the engine's vtables.
Entries in these are only ever called indirectly, so a plain disassembly (and
even Ghidra's analysis) leaves some unrecovered, and the lifter then has no entry
for them. This scans the data sections for aligned 32-bit words that point into
.text and begin with a plausible function prologue, and emits those not already
in the function list as extra entries (addr,size,name,is_thunk,xrefs), the same
format the lifter reads. Adding real entry points is always safe: at worst the
lifter emits a few functions that are never reached.
"""
import struct
import sys

import capstone
import pefile

# push reg prologues: 0x50-0x57 are push eax/ecx/edx/ebx/esp/ebp/esi/edi. MSVC
# emits `push ecx` (0x51) to reserve a single 4-byte local, a very common start
# for small __thiscall vtable methods; without the whole push-reg range these
# vtable-only functions (reached only indirectly) are missed and calling them faults.
PROLOGUES = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x8B, 0x83, 0x81, 0x6A, 0x68, 0xE9, 0xE8, 0xB8, 0xFF, 0x33, 0xA1, 0x64}


def main():
    pe = pefile.PE(sys.argv[1])
    have = set()
    if len(sys.argv) > 2:
        for line in open(sys.argv[2]):
            if line.startswith("0x"):
                have.add(int(line.split(",")[0], 16))
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    tlo = text.VirtualAddress + base
    thi = tlo + text.Misc_VirtualSize
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    # Pointers from a table embedded in .text are MSVC C++ exception-handling
    # FuncInfo/handler tables; the catch/cleanup funclets they point at start with
    # a normal prologue or a bare `ret`/`ret n` (an empty handler). Accepting only
    # these avoids lifting the many coincidental pointer-like words in .text.
    EH_EXTRA = {0xC3, 0xC2}

    def plausible(va, from_text):
        off = va - base
        ok = PROLOGUES | EH_EXTRA if from_text else PROLOGUES
        if img[off] not in ok:
            return False
        try:
            next(md.disasm(img[off:off + 8], va))
            return True
        except StopIteration:
            return False

    found = {}  # target -> strict (True if from a data section, False if only from .text)
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        if name not in (".data", ".rdata", ".text"):
            continue
        strict = name != ".text"
        raw = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
        for off in range(0, len(raw) - 4, 4):
            v = struct.unpack_from("<I", raw, off)[0]
            if tlo <= v < thi and v not in have:
                # a data-section hit is authoritative; keep the strict flag unless
                # a data section also points here
                found[v] = found.get(v, True) and strict
    for v in sorted(k for k, strict in found.items() if plausible(k, not strict)):
        print("0x%08x,0,ptr_%08x,0,0" % (v, v))


if __name__ == "__main__":
    main()
