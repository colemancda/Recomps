#!/usr/bin/env python3
"""Compare lifted instructions with Unicorn (ARM1176: armv6 + VFPv2).

    LIBUNICORN_PATH=<dir with libunicorn.dylib> PYTHONPATH=<unicorn python binding> \\
        python3 test/insn_test.py [--per-shape 3] [--states 20] [--seed 1]

Samples the game's own instructions by shape (mode, mnemonic, operand kinds,
shift, condition, flag setting, writeback), leaving out control flow. Each sample
is lifted alone into t_N(CPU *c), compiled into a shared library, and run from
random register, flag and VFP states. Unicorn runs the same instruction at its
original address over the same memory: the image where the address is inside it,
pseudo-random bytes elsewhere, paged in on demand. Registers, flags, VFP
registers, FPSCR flags and every byte either side stored must match.
"""
import argparse
import collections
import ctypes
import hashlib
import mmap
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "lift"))
import analyze  # noqa: E402
from translate import Translator, Unsupported  # noqa: E402
from capstone import arm  # noqa: E402

import unicorn  # noqa: E402
from unicorn import arm_const as U  # noqa: E402

PAGE = 0x1000
UREGS = [getattr(U, 'UC_ARM_REG_R%d' % k) for k in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
UDREGS = [getattr(U, 'UC_ARM_REG_D%d' % k) for k in range(16)]


class CPU(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16),
                ("n", ctypes.c_uint8), ("z", ctypes.c_uint8), ("cf", ctypes.c_uint8), ("v", ctypes.c_uint8),
                ("q", ctypes.c_uint8), ("pad", ctypes.c_uint8 * 3),
                ("ge", ctypes.c_uint32),
                ("f", ctypes.c_uint64 * 16),
                ("fn", ctypes.c_uint8), ("fz", ctypes.c_uint8), ("fc", ctypes.c_uint8), ("fv", ctypes.c_uint8),
                ("fpscr", ctypes.c_uint32), ("excl_addr", ctypes.c_uint32),
                ("thread", ctypes.c_void_p)]


GLUE = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
GuestFn *g_ftab; uint32_t g_ftab_base, g_ftab_size;
#define LOG_CAP (1 << 20)
uint32_t g_log[LOG_CAP][2]; int g_nlog;
void rt_log_store(uint32_t a, uint32_t n) { if (g_nlog < LOG_CAP) { g_log[g_nlog][0] = a; g_log[g_nlog][1] = n; g_nlog++; } }
static void die(const char *w) { fprintf(stderr, "test: unexpected %s\n", w); abort(); }
void rt_call_slow(CPU *c, uint32_t a) { (void)c; (void)a; die("call"); }
void rt_jmp_slow(CPU *c, uint32_t a, uint32_t f) { (void)c; (void)a; (void)f; die("jump"); }
void rt_import(CPU *c, uint32_t s) { (void)c; (void)s; die("import"); }
void rt_ret_mismatch(CPU *c, uint32_t e, uint32_t f) { (void)c; (void)e; (void)f; die("return"); }
void rt_unimplemented(CPU *c, uint32_t a, const char *t) { (void)c; (void)a; (void)t; die("unimplemented"); }
void rt_bad_code(CPU *c, uint32_t f, uint32_t t) { (void)c; (void)f; (void)t; die("bad code"); }
uint32_t rt_strex(CPU *c, uint32_t a, uint32_t v, int size) {
    if (c->excl_addr != a) return 1;
    if (size == 4) st32(a, v); else if (size == 2) st16(a, v); else st8(a, v);
    c->excl_addr = 1;
    return 0;
}
'''


class NoFlow:
    """Control flow isn't part of the sampled instructions."""
    def branch(self, t): raise Unsupported("branch")
    def call(self, *a): raise Unsupported("call")
    def indirect(self, *a): raise Unsupported("indirect")
    def icall(self, *a): raise Unsupported("icall")


def shape(ins, mode):
    ops = []
    for o in ins.operands:
        k = {arm.ARM_OP_REG: "r", arm.ARM_OP_IMM: "i", arm.ARM_OP_MEM: "m"}.get(o.type, "?")
        if o.type == arm.ARM_OP_REG and ins.reg_name(o.reg) == "pc":
            k = "pc"
        if o.type == arm.ARM_OP_MEM:
            k += "x" if o.mem.index else "d"
            if ins.reg_name(o.mem.base) == "pc":
                k += "pc"
        if o.shift.type:
            k += "s%d" % o.shift.type
        if o.subtracted:
            k += "-"
        ops.append(k)
    return (mode, ins.mnemonic, tuple(ops), ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID),
            bool(ins.update_flags), bool(ins.writeback))


def is_flow(an, ins):
    if ins.id in (arm.ARM_INS_B, arm.ARM_INS_BL, arm.ARM_INS_BLX, arm.ARM_INS_BX):
        return True
    return an.writes_pc(ins)


class Memory:
    """Page contents: the image where it has bytes, pseudo-random elsewhere."""
    def __init__(self, img):
        self.img = img
        self.cache = {}

    def page(self, p):
        if p in self.cache:
            return self.cache[p]
        out = bytearray()
        for k in range(0, PAGE, 4):
            w = self.img.word(p + k)
            if w is None:
                break
            out += w.to_bytes(4, "little")
        if len(out) != PAGE:
            out = bytearray()
            seed = hashlib.sha256(p.to_bytes(4, "little")).digest()
            while len(out) < PAGE:
                seed = hashlib.sha256(seed).digest()
                out += seed
            out = out[:PAGE]
        self.cache[p] = bytes(out)
        return self.cache[p]


def random_state(rng):
    regs = []
    for _ in range(15):
        k = rng.random()
        if k < 0.35:
            v = rng.randrange(0x10000000, 0x20000000) & ~3       # word-aligned pointer
        elif k < 0.6:
            v = rng.randrange(0, 64)                             # small
        elif k < 0.7:
            v = rng.choice([0, 1, 31, 32, 33, 0x7fffffff, 0x80000000, 0xffffffff, 0xff, 0x100])
        else:
            v = rng.getrandbits(32)
        regs.append(v)
    regs[13] = rng.randrange(0x30000000, 0x30100000) & ~7        # sp
    flags = [rng.getrandbits(1) for _ in range(4)]
    fregs = []
    for _ in range(16):
        k = rng.random()
        if k < 0.7:
            lo = struct.unpack("<I", struct.pack("<f", rng.uniform(-1e4, 1e4)))[0]
            hi = struct.unpack("<I", struct.pack("<f", rng.choice([rng.uniform(-100, 100), 0.0, 1.0, -2.5, 3e9])))[0]
            fregs.append(lo | (hi << 32))
        else:
            fregs.append(struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e6, 1e6)))[0])
    return regs, flags, fregs


def run_unicorn(mem, ins, mode, state):
    regs, flags, fregs = state
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(U.UC_CPU_ARM_1176)
    pages = set()
    writes = []

    def unmapped(uc_, access, addr, size, value, user):
        for p in {addr & ~(PAGE - 1), (addr + size - 1) & ~(PAGE - 1)}:
            if p not in pages:
                uc_.mem_map(p, PAGE)
                uc_.mem_write(p, mem.page(p))
                pages.add(p)
        return True

    def on_write(uc_, access, addr, size, value, user):
        writes.append((addr, size))

    uc.hook_add(unicorn.UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, on_write)
    unmapped(uc, 0, ins.address, ins.size, 0, None)
    # VFP on: CPACR full access for cp10/cp11, FPEXC.EN
    uc.reg_write(U.UC_ARM_REG_C1_C0_2, 0xF << 20)
    uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
    for k in range(15):
        uc.reg_write(UREGS[k], regs[k])
    cpsr = uc.reg_read(U.UC_ARM_REG_CPSR) & 0x0FFFFFFF
    cpsr |= (flags[0] << 31) | (flags[1] << 30) | (flags[2] << 29) | (flags[3] << 28)
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    for k in range(16):
        uc.reg_write(UDREGS[k], fregs[k])
    start = ins.address | (1 if mode == "thumb" else 0)
    uc.emu_start(start, ins.address + ins.size, count=1)
    out_regs = [uc.reg_read(UREGS[k]) for k in range(15)]
    cpsr = uc.reg_read(U.UC_ARM_REG_CPSR)
    out_flags = [(cpsr >> 31) & 1, (cpsr >> 30) & 1, (cpsr >> 29) & 1, (cpsr >> 28) & 1, (cpsr >> 27) & 1]
    out_f = [uc.reg_read(UDREGS[k]) for k in range(16)]
    fpscr = uc.reg_read(U.UC_ARM_REG_FPSCR)
    stored = {}
    for a, n in writes:
        for k in range(n):
            stored[(a + k) & 0xFFFFFFFF] = uc.mem_read((a + k) & 0xFFFFFFFF, 1)[0]
    return out_regs, out_flags, out_f, fpscr >> 28, stored, pages


def fmt_state(regs, flags):
    return " ".join("r%d=%08x" % (k, v) for k, v in enumerate(regs)) + " nzcv=%s" % "".join(map(str, flags))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "orig/Payload/kiwi.app/kiwi"))
    ap.add_argument("--funcs", default=os.path.join(ROOT, "build/s3e_funcs.tsv"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build/insn_test"))
    ap.add_argument("--per-shape", type=int, default=3)
    ap.add_argument("--states", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", help="only shapes whose mnemonic starts with this")
    ap.add_argument("--show", type=int, default=40, help="failures to print")
    args = ap.parse_args()

    an = analyze.Analysis(args.exe, args.funcs)
    bodies = an.all_functions()
    tr = Translator()
    ctx = NoFlow()

    samples = collections.defaultdict(list)
    unsupported = collections.Counter()
    for e, body in bodies.items():
        addrs = body[0]
        mode = an.starts[e]
        for a in addrs:
            ins = an.decode(a, mode)
            if ins is None or is_flow(an, ins):
                continue
            if args.only and not ins.mnemonic.startswith(args.only):
                continue
            key = shape(ins, mode)
            if len(samples[key]) < args.per_shape and all(s[0].address != a for s in samples[key]):
                samples[key].append((ins, mode))

    tests = []
    lines = ['#include "cpu.h"']
    for key, lst in sorted(samples.items(), key=lambda kv: str(kv[0])):
        for ins, mode in lst:
            try:
                body = tr.insn(ins, mode, ctx)
            except Unsupported as ex:
                unsupported[str(ex).split(" ")[0]] += 1
                continue
            n = len(tests)
            tests.append((ins, mode, key))
            lines.append("void t_%d(CPU *c) { /* %08x %s %s */" % (n, ins.address, ins.mnemonic, ins.op_str))
            lines += ["    " + s for s in body]
            lines.append("}")
    lines.append("GuestFn g_tests[] = {%s};" % ", ".join("t_%d" % k for k in range(len(tests))))
    os.makedirs(args.out, exist_ok=True)
    src = os.path.join(args.out, "tests.c")
    with open(src, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(args.out, "glue.c"), "w") as f:
        f.write(GLUE)
    lib = os.path.join(args.out, "tests.dylib")
    cmd = ["xcrun", "clang", "-O1", "-shared", "-ffp-contract=off", "-fno-strict-aliasing", "-w",
           "-DRT_DYNAMIC_BASE", "-DRT_LOG_STORES", "-I", os.path.join(ROOT, "runtime"),
           "-o", lib, src, os.path.join(args.out, "glue.c")]
    subprocess.run(cmd, check=True)
    print("shapes %d, tests %d, untranslated samples %d %s" % (
        len(samples), len(tests), sum(unsupported.values()), dict(unsupported.most_common(12))))

    so = ctypes.CDLL(lib)
    tfns = (ctypes.c_void_p * len(tests)).in_dll(so, "g_tests")
    proto = ctypes.CFUNCTYPE(None, ctypes.POINTER(CPU))
    nlog = ctypes.c_int.in_dll(so, "g_nlog")
    glog = ((ctypes.c_uint32 * 2) * (1 << 20)).in_dll(so, "g_log")
    space = mmap.mmap(-1, (1 << 32) + PAGE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANON, prot=mmap.PROT_READ | mmap.PROT_WRITE)
    base = ctypes.addressof(ctypes.c_char.from_buffer(space))
    ctypes.c_void_p.in_dll(so, "g_mem").value = base
    mem = Memory(an.img)
    rng = random.Random(args.seed)

    failures = collections.defaultdict(list)
    passed = skipped = 0
    for n, (ins, mode, key) in enumerate(tests):
        fn = proto(tfns[n])
        for _ in range(args.states):
            st = random_state(rng)
            try:
                u_regs, u_flags, u_f, u_fpf, u_stored, pages = run_unicorn(mem, ins, mode, st)
            except unicorn.UcError:
                skipped += 1
                continue
            for p in pages:
                space[p:p + PAGE] = mem.page(p)
            cpu = CPU()
            regs, flags, fregs = st
            for k in range(15):
                cpu.r[k] = regs[k]
            cpu.n, cpu.z, cpu.cf, cpu.v = flags
            for k in range(16):
                cpu.f[k] = fregs[k]
            cpu.excl_addr = 1
            nlog.value = 0
            fn(ctypes.byref(cpu))
            c_stored = {}
            for k in range(nlog.value):
                a, sz = glog[k][0], glog[k][1]
                for j in range(sz):
                    c_stored[(a + j) & 0xFFFFFFFF] = space[(a + j) & 0xFFFFFFFF]
            diffs = []
            if ins.id == arm.ARM_INS_MRS:  # A, I, F, T and mode bits: the emulator runs in SVC mode
                u_regs = [cpu.r[j] if (cpu.r[j] ^ v) & ~0x1FF == 0 else v for j, v in enumerate(u_regs)]
            for k in range(15):
                if cpu.r[k] != u_regs[k]:
                    diffs.append("r%d %08x != %08x" % (k, cpu.r[k], u_regs[k]))
            for k, nm in enumerate(("n", "z", "c", "v", "q")):
                cv = [cpu.n, cpu.z, cpu.cf, cpu.v, cpu.q][k]
                if cv != u_flags[k]:
                    diffs.append("%s %d != %d" % (nm, cv, u_flags[k]))
            for k in range(16):
                a, b = cpu.f[k], u_f[k]
                if a != b:
                    def nanish(x):
                        lo, hi = x & 0xFFFFFFFF, x >> 32
                        return (lo & 0x7F800000) == 0x7F800000 and lo & 0x7FFFFF, \
                               (hi & 0x7F800000) == 0x7F800000 and hi & 0x7FFFFF, \
                               (x >> 52 & 0x7FF) == 0x7FF and x & ((1 << 52) - 1)
                    if not any(nanish(a)[j] and nanish(b)[j] for j in range(3)):
                        diffs.append("d%d %016x != %016x" % (k, a, b))
            cf = (cpu.fn << 3) | (cpu.fz << 2) | (cpu.fc << 1) | cpu.fv
            if cf != u_fpf:
                diffs.append("fpscr nzcv %x != %x" % (cf, u_fpf))
            for a in sorted(set(c_stored) | set(u_stored)):
                if c_stored.get(a) != u_stored.get(a):
                    diffs.append("mem %08x %s != %s" % (a, c_stored.get(a), u_stored.get(a)))
                    break
            if diffs:
                failures[key].append((ins, mode, st, diffs))
            else:
                passed += 1
            # restore pages for the next run
            for p in pages:
                space[p:p + PAGE] = mem.page(p)

    print("runs passed %d, failed %d (in %d shapes), skipped (emulator fault) %d"
          % (passed, sum(len(v) for v in failures.values()), len(failures), skipped))
    shown = 0
    for key, lst in sorted(failures.items(), key=lambda kv: -len(kv[1])):
        if shown >= args.show:
            break
        ins, mode, st, diffs = lst[0]
        print("FAIL %-5s %08x %-8s %-30s x%d: %s" % (mode, ins.address, ins.mnemonic, ins.op_str, len(lst),
                                                     "; ".join(diffs[:4])))
        print("      in: " + fmt_state(st[0], st[1]))
        shown += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
