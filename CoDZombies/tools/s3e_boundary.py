#!/usr/bin/env python3
"""Map the boundary between the game (s3e_code) and Marmalade's iOS loader (__text).

    python3 -I tools/s3e_boundary.py [orig/Payload/kiwi.app/kiwi] > build/s3e_boundary.tsv

The game reaches the loader only through stubs at the end of s3e_code:

    stub:    ldr ip, [pc]        ; ip = implementation in the loader
             ldr pc, [pc]        ; pc = trampoline in the loader
             .word impl
             .word trampoline
             bx pc ; nop         ; Thumb entry of the next stub (stub + 0x14 - 4)

The trampoline says how the call is made (see TRAMPOLINES). A stub whose impl is
not code in __text is a data export: the game reads the word instead of calling.
A stub whose impl is a `b` into __symbol_stub4 passes straight through to an iOS
library function (kind "import"; the subsystem column is then that function's name).

One row per stub: stub address, implementation, trampoline, return width,
argument class, enter/leave hooks, kind, subsystem guess, number of call sites
in the game (ARM BL/B, Thumb BL/BLX, through the branch table at 0x65f80), and the
strings the implementation refers to. A summary goes to stderr.
"""
import collections
import struct
import sys

TRAMPOLINES = {
    # address: (return, arguments, enter/leave hooks)
    0x383EC: ("void", "r0-r3", "yes"),
    0x38460: ("i32", "r0-r3", "yes"),
    0x384D0: ("i64", "r0-r3", "yes"),
    0x380B8: ("i32", "r0-r3", "no"),
    0x38100: ("i64", "r0-r3", "no"),
    0x3865C: ("void", "stack<=8", "yes"),
    0x38678: ("i32", "stack<=8", "yes"),
    0x38690: ("i64", "stack<=8", "yes"),
}

# Implementation address ranges in __text, by subsystem. Marmalade's sources are
# linked in alphabetical order (the Disable<Subsystem> config keys list them all:
# Accelerometer Audio Callbacks Camera Compression Config Crypto Debug Device
# Error Ext Fibre File GL Keyboard License Memory Pointer SecureStorage Socket
# Sound Surface Timer Vibra Video); each range is anchored by the strings its
# functions use (assertion texts, config keys). Boundaries between anchors, and
# names ending in "?", are guesses.
SUBSYSTEMS = [
    (0x11400, "s3eAccelerometer"),
    (0x11A00, "s3eAudio"),
    (0x12000, "bigint (internal)"),
    (0x16F00, "s3eCallbacks"),
    (0x18400, "s3eCompression"),
    (0x19800, "s3eConfig"),
    (0x1C000, "s3eCrypto"),
    (0x1DEA8, "s3eDebug"),
    (0x1FD00, "s3eDevice/s3eError"),  # no string marks where Device ends
    (0x21D00, "s3eExt"),
    (0x23600, "s3eFile"),
    (0x29A00, "s3eGL?"),
    (0x2AC00, "s3eKeyboard"),
    (0x2D700, "s3eLicense"),
    (0x2DA00, "s3eMemory"),
    (0x2FE00, "s3ePointer"),
    (0x30800, "s3eSecureStorage?"),
    (0x30C00, "s3eSocket"),
    (0x34700, "s3eSound"),
    (0x39C00, "s3eSurface"),
    (0x3AA00, "s3eTimer"),
    (0x3B100, "s3eVideo"),
    (0x3C000, "?"),
    (0x46100, "s3eGL/EGL"),
    (0x46700, "?"),
]


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.sections = {}
        self.segments = []
        self.reserved = {}
        symtab = dysymtab = None
        d = self.data
        ncmds = struct.unpack_from("<I", d, 16)[0]
        off = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            if cmd == 1:  # LC_SEGMENT
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4I", d, off + 24)
                self.segments.append((vmaddr, filesize, fileoff))
                nsect = struct.unpack_from("<I", d, off + 48)[0]
                p = off + 56
                for _ in range(nsect):
                    name = d[p:p + 16].rstrip(b"\0").decode()
                    addr, sz = struct.unpack_from("<II", d, p + 32)
                    reserved1, reserved2 = struct.unpack_from("<II", d, p + 60)
                    self.sections[name] = (addr, sz)
                    self.reserved[name] = (reserved1, reserved2)
                    p += 68
            elif cmd == 2:  # LC_SYMTAB
                symtab = struct.unpack_from("<4I", d, off + 8)
            elif cmd == 0xB:  # LC_DYSYMTAB
                dysymtab = struct.unpack_from("<18I", d, off + 8)
            off += size
        # Imported functions by __symbol_stub4 address.
        self.imports = {}
        symoff, nsyms, stroff, _ = symtab
        indoff = dysymtab[12]
        addr, sz = self.sections["__symbol_stub4"]
        first, stubsize = self.reserved["__symbol_stub4"]
        for k in range(sz // stubsize):
            idx = struct.unpack_from("<I", d, indoff + 4 * (first + k))[0]
            if idx < nsyms:
                sx = struct.unpack_from("<I", d, symoff + 12 * idx)[0]
                name = d[stroff + sx:d.index(b"\0", stroff + sx)].decode()
                self.imports[addr + k * stubsize] = name.lstrip("_")

    def import_name(self, impl):
        """Name of the iOS import when impl is `b <__symbol_stub4 entry>`."""
        w = self.word(impl)
        if w is None or (w >> 24) != 0xEA:
            return None
        imm = w & 0xFFFFFF
        imm -= (1 << 24) if imm & 0x800000 else 0
        return self.imports.get(impl + 8 + imm * 4)

    def word(self, a):
        for vm, sz, fo in self.segments:
            if vm <= a < vm + sz - 3:
                return struct.unpack_from("<I", self.data, fo + a - vm)[0]
        return None

    def half(self, a):
        w = self.word(a & ~3)
        return None if w is None else (w >> (16 * ((a >> 1) & 1))) & 0xFFFF

    def cstring(self, a):
        for vm, sz, fo in self.segments:
            if vm <= a < vm + sz:
                o = fo + a - vm
                return self.data[o:self.data.index(b"\0", o)].decode("latin1")


def subsystem(impl):
    name = "?"
    for start, sub in SUBSYSTEMS:
        if impl >= start:
            name = sub
    return name


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "orig/Payload/kiwi.app/kiwi"
    img = Image(path)
    code_lo, code_sz = img.sections["s3e_code"]
    code_hi = code_lo + code_sz
    text_lo, text_sz = img.sections["__text"]
    cs_lo, cs_sz = img.sections["__cstring"]

    stubs = []
    for a in range(code_lo, code_hi - 16, 4):
        if img.word(a) == 0xE59FC000 and img.word(a + 4) == 0xE59FF000:
            stubs.append((a, img.word(a + 8), img.word(a + 12)))
    stub_at = {s[0]: s for s in stubs}
    thumb_entry = {a - 4: a for a, _, _ in stubs if img.half(a - 4) == 0x4778}

    # Branch table of ARM B instructions at the start of the image.
    table = {}
    a = 0x65F80
    while (img.word(a) >> 24) == 0xEA:
        imm = img.word(a) & 0xFFFFFF
        imm -= (1 << 24) if imm & 0x800000 else 0
        table[a] = a + 8 + imm * 4
        a += 4

    def resolve(t):
        if t in table:
            t = table[t]
        if t in thumb_entry:
            t = thumb_entry[t]
        return t if t in stub_at else None

    refs = collections.Counter()
    stub_lo = stubs[0][0] - 4
    for a in range(code_lo, stub_lo, 2):
        h1 = img.half(a)
        # ARM BL / B (only at word alignment)
        if a % 4 == 0:
            w = img.word(a)
            if (w & 0x0E000000) == 0x0A000000 and (w >> 28) != 0xF:
                imm = w & 0xFFFFFF
                imm -= (1 << 24) if imm & 0x800000 else 0
                t = resolve(a + 8 + imm * 4)
                if t:
                    refs[t] += 1
        # Thumb BL / BLX pair
        if h1 is not None and (h1 & 0xF800) == 0xF000:
            h2 = img.half(a + 2)
            if h2 is not None and (h2 & 0xE800) == 0xE800:
                off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
                off -= (1 << 23) if off & (1 << 22) else 0
                t = a + 4 + off
                if (h2 & 0xF800) == 0xE800:  # BLX: to ARM, word aligned
                    t &= ~3
                t = resolve(t)
                if t:
                    refs[t] += 1

    def strings(impl):
        out = []
        known = {s[1] for s in stubs}
        for k in range(0, 600, 4):
            a = impl + k
            w = img.word(a)
            if w is None or (k and a in known):
                break
            if (w & 0x0F7F0000) == 0x051F0000:  # ldr rX, [pc, #imm]
                imm = w & 0xFFF
                v = img.word(a + 8 + (imm if w & (1 << 23) else -imm))
                if v is not None and cs_lo <= v < cs_lo + cs_sz:
                    s = img.cstring(v).replace("\t", " ").replace("\n", "\\n")
                    if s and s not in out:
                        out.append(s)
        return out

    print("stub\timpl\ttrampoline\treturn\targs\thooks\tkind\tsubsystem\tcalls\tstrings")
    kinds = collections.Counter()
    for a, impl, tr in stubs:
        is_code = text_lo <= impl < text_lo + text_sz and impl % 4 == 0
        kind = "call" if is_code else "data"
        if not is_code:
            kinds[kind] += 1
        ret, args, hooks = TRAMPOLINES.get(tr, ("?", "?", "?")) if is_code else ("-", "-", "-")
        imp = img.import_name(impl) if is_code else None
        if imp:
            kind, sub, strs = "import", imp, ""
        else:
            sub = subsystem(impl) if is_code else "-"
            strs = " | ".join(strings(impl)[:4]) if is_code else ""
        if is_code:
            kinds[kind] += 1
        print(f"{a:08x}\t{impl:08x}\t{tr:08x}\t{ret}\t{args}\t{hooks}\t{kind}\t{sub}\t{refs[a]}\t{strs}")

    impls = {i for _, i, _ in stubs}
    called = sum(1 for a, _, _ in stubs if refs[a])
    print(f"stubs {len(stubs)} ({stubs[0][0]:#x}-{stubs[-1][0]:#x}), {dict(kinds)}, "
          f"distinct impls {len(impls)}, branch table {len(table)} entries, "
          f"stubs with direct call sites {called}, call sites {sum(refs.values())}", file=sys.stderr)
    tr = collections.Counter(t for _, i, t in stubs if text_lo <= i < text_lo + text_sz)
    for t, n in tr.most_common():
        print(f"  trampoline {t:#x} {TRAMPOLINES.get(t, ('?',))}: {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
