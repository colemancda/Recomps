#!/usr/bin/env python3
"""Compare whole lifted leaf functions with Unicorn.

    LIBUNICORN_PATH=... PYTHONPATH=... python3 test/func_test.py [--count 400] [--states 6] [--seed 1]

Leaf functions are ones whose bodies make no calls, imports, tail calls or
indirect jumps: control flow stays inside the function until it returns. They
still exercise the emitted control flow (labels, conditional branches, loops,
jump tables, returns). Each runs from random register and flag states, with lr
set to a sentinel, in Unicorn until it reaches the sentinel and as lifted C;
registers, flags, VFP registers and stored bytes must match. Runs that take
more than --max-insns instructions in the emulator are skipped.

The lifted C is built for this Mac into build/func_test/lifted.dylib (all of
build/gen, so run lift/lift.py first).
"""
import argparse
import ctypes
import mmap
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "lift"))
import analyze  # noqa: E402
from insn_test import CPU, GLUE, Memory, PAGE, UREGS, UDREGS, random_state  # noqa: E402
from capstone import arm  # noqa: E402

import unicorn  # noqa: E402
from unicorn import arm_const as U  # noqa: E402

SENTINEL = 0xFFFF0000


def is_leaf(an, entry, body):
    addrs, tables, calls = body
    if calls or not addrs:
        return False
    mode = an.starts[entry]
    bodyset = set(addrs)
    for a in addrs:
        ins = an.decode(a, mode)
        i = ins.id
        if i in (arm.ARM_INS_BL, arm.ARM_INS_BLX):
            return False
        if i == arm.ARM_INS_B and ins.operands[0].imm not in bodyset:
            return False
        if an.writes_pc(ins) and i != arm.ARM_INS_B:
            if a in tables:
                if not all(t in bodyset for t in tables[a]):
                    return False
                continue
            text = "%s %s" % (ins.mnemonic, ins.op_str)
            is_ret = (i == arm.ARM_INS_BX and ins.reg_name(ins.operands[0].reg) == "lr") or \
                     (i in (arm.ARM_INS_POP,) ) or \
                     (i in (arm.ARM_INS_LDM,) and ins.reg_name(ins.operands[0].reg) == "sp") or \
                     (i == arm.ARM_INS_MOV and text.endswith("pc, lr"))
            if not is_ret:
                return False
    # nothing may fall through out of the body (into another function)
    for a in addrs:
        ins = an.decode(a, mode)
        cond = ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID)
        ends = (ins.id == arm.ARM_INS_B or an.writes_pc(ins)) and not cond
        if not ends and a + ins.size not in bodyset:
            return False
    return True


def build(out):
    os.makedirs(out, exist_ok=True)
    glue = os.path.join(out, "glue.c")
    with open(glue, "w") as f:
        f.write(GLUE.replace("GuestFn *g_ftab; uint32_t g_ftab_base, g_ftab_size;",
                             "GuestFn *g_ftab; uint32_t g_ftab_base = 0xffffffff, g_ftab_size;"))
    gen = os.path.join(ROOT, "build/gen")
    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.startswith("chunk_") and f.endswith(".c"))
    lib = os.path.join(out, "lifted.dylib")
    newest = max(os.path.getmtime(s) for s in srcs + [os.path.join(ROOT, "runtime/cpu.h")])
    if os.path.exists(lib) and os.path.getmtime(lib) > newest:
        return lib
    flags = ["-O1", "-fPIC", "-ffp-contract=off", "-fno-strict-aliasing", "-w", "-DRT_DYNAMIC_BASE",
             "-DRT_LOG_STORES", "-I", os.path.join(ROOT, "runtime"), "-I", gen]
    procs, objs = [], []
    for s in srcs + [glue]:
        o = os.path.join(out, os.path.basename(s)[:-2] + ".o")
        objs.append(o)
        procs.append(subprocess.Popen(["xcrun", "clang", "-c", s, "-o", o] + flags))
        if len(procs) >= os.cpu_count():
            assert procs.pop(0).wait() == 0
    for p in procs:
        assert p.wait() == 0
    subprocess.run(["xcrun", "clang", "-shared", "-o", lib] + objs, check=True)
    return lib


def run_unicorn(mem, entry, mode, state, max_insns):
    regs, flags, fregs = state
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(U.UC_CPU_ARM_1176)
    pages, writes = set(), []

    def unmapped(uc_, access, addr, size, value, user):
        for p in {addr & ~(PAGE - 1), (addr + size - 1) & ~(PAGE - 1)}:
            if p not in pages:
                uc_.mem_map(p, PAGE)
                uc_.mem_write(p, mem.page(p))
                pages.add(p)
        return True

    uc.hook_add(unicorn.UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, lambda u, ac, a, s, v, d: writes.append((a, s)))
    uc.reg_write(U.UC_ARM_REG_C1_C0_2, 0xF << 20)
    uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
    for k in range(15):
        uc.reg_write(UREGS[k], regs[k])
    uc.reg_write(U.UC_ARM_REG_LR, SENTINEL)
    cpsr = uc.reg_read(U.UC_ARM_REG_CPSR) & 0x0FFFFFFF
    cpsr |= (flags[0] << 31) | (flags[1] << 30) | (flags[2] << 29) | (flags[3] << 28)
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    for k in range(16):
        uc.reg_write(UDREGS[k], fregs[k])
    uc.emu_start(entry | (1 if mode == "thumb" else 0), SENTINEL, count=max_insns)
    if uc.reg_read(U.UC_ARM_REG_PC) & ~1 != SENTINEL:
        raise TimeoutError
    out_regs = [uc.reg_read(UREGS[k]) for k in range(15)]
    cpsr = uc.reg_read(U.UC_ARM_REG_CPSR)
    out_flags = [(cpsr >> s) & 1 for s in (31, 30, 29, 28, 27)]
    out_f = [uc.reg_read(UDREGS[k]) for k in range(16)]
    fpf = uc.reg_read(U.UC_ARM_REG_FPSCR) >> 28
    stored = {}
    for a, n in writes:
        for k in range(n):
            stored[(a + k) & 0xFFFFFFFF] = uc.mem_read((a + k) & 0xFFFFFFFF, 1)[0]
    return out_regs, out_flags, out_f, fpf, stored, pages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "orig/Payload/kiwi.app/kiwi"))
    ap.add_argument("--funcs", default=os.path.join(ROOT, "build/s3e_funcs.tsv"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build/func_test"))
    ap.add_argument("--count", type=int, default=400, help="leaf functions to test")
    ap.add_argument("--states", type=int, default=6)
    ap.add_argument("--max-insns", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=30)
    args = ap.parse_args()

    an = analyze.Analysis(args.exe, args.funcs)
    bodies = an.all_functions()
    leaves = sorted(e for e, b in bodies.items() if is_leaf(an, e, b))
    rng = random.Random(args.seed)
    picked = rng.sample(leaves, min(args.count, len(leaves)))
    modes = [an.starts[e] for e in picked]
    print("leaf functions %d of %d; testing %d (%d Thumb), %d instructions" % (
        len(leaves), len(bodies), len(picked), modes.count("thumb"), sum(len(bodies[e][0]) for e in picked)))

    so = ctypes.CDLL(build(args.out))
    proto = ctypes.CFUNCTYPE(None, ctypes.POINTER(CPU))
    nlog = ctypes.c_int.in_dll(so, "g_nlog")
    glog = ((ctypes.c_uint32 * 2) * 4096).in_dll(so, "g_log")
    space = mmap.mmap(-1, (1 << 32) + PAGE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANON,
                      prot=mmap.PROT_READ | mmap.PROT_WRITE)
    ctypes.c_void_p.in_dll(so, "g_mem").value = ctypes.addressof(ctypes.c_char.from_buffer(space))
    mem = Memory(an.img)

    passed, skipped, failures = 0, 0, []
    for e in picked:
        fn = proto(ctypes.cast(getattr(so, "f_%08x" % e), ctypes.c_void_p).value)
        for _ in range(args.states):
            st = random_state(rng)
            try:
                u = run_unicorn(mem, e, an.starts[e], st, args.max_insns)
            except (unicorn.UcError, TimeoutError):
                skipped += 1
                continue
            u_regs, u_flags, u_f, u_fpf, u_stored, pages = u
            for p in pages:
                space[p:p + PAGE] = mem.page(p)
            cpu = CPU()
            regs, flags, fregs = st
            for k in range(15):
                cpu.r[k] = regs[k]
            cpu.r[14] = SENTINEL
            cpu.n, cpu.z, cpu.cf, cpu.v = flags
            for k in range(16):
                cpu.f[k] = fregs[k]
            cpu.excl_addr = 1
            nlog.value = 0
            fn(ctypes.byref(cpu))
            diffs = []
            if cpu.r[15] & ~1 != SENTINEL:
                diffs.append("returned to %08x" % cpu.r[15])
            for k in range(15):
                if cpu.r[k] != u_regs[k]:
                    diffs.append("r%d %08x != %08x" % (k, cpu.r[k], u_regs[k]))
            for k, nm in enumerate("nzcvq"):
                cv = [cpu.n, cpu.z, cpu.cf, cpu.v, cpu.q][k]
                if cv != u_flags[k]:
                    diffs.append("%s %d != %d" % (nm, cv, u_flags[k]))
            for k in range(16):
                if cpu.f[k] != u_f[k]:
                    diffs.append("d%d %016x != %016x" % (k, cpu.f[k], u_f[k]))
            c_stored = {}
            for k in range(nlog.value):
                a, sz = glog[k][0], glog[k][1]
                for j in range(sz):
                    c_stored[(a + j) & 0xFFFFFFFF] = space[(a + j) & 0xFFFFFFFF]
            for a in sorted(set(c_stored) | set(u_stored)):
                if c_stored.get(a) != u_stored.get(a):
                    diffs.append("mem %08x %s != %s" % (a, c_stored.get(a), u_stored.get(a)))
                    break
            for p in pages:
                space[p:p + PAGE] = mem.page(p)
            if diffs:
                failures.append((e, diffs))
            else:
                passed += 1
    bad = sorted({e for e, _ in failures})
    print("runs passed %d, failed %d (%d functions), skipped %d (emulator fault or over --max-insns)"
          % (passed, len(failures), len(bad), skipped))
    seen = set()
    for e, diffs in failures:
        if e in seen or len(seen) >= args.show:
            continue
        seen.add(e)
        print("FAIL f_%08x (%s, %d insns): %s" % (e, an.starts[e], len(bodies[e][0]), "; ".join(diffs[:4])))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
