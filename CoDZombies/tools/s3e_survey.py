#!/usr/bin/env python3
"""Recursive-descent survey of the game's code (s3e_code), ARM and Thumb.

    python3 -I tools/s3e_survey.py [orig/Payload/kiwi.app/kiwi] [--funcs build/s3e_funcs.tsv]

Roots: the image's start routine (0x65f00, ARM), the branch table after it, and
Thumb code pointers (odd words pointing into s3e_code) found anywhere in
s3e_code, s3e_data and __DATA, such as vtables and registered callbacks. From
there it follows B/BL/BLX and conditional branches in both instruction sets and
ARM jump tables (`ldr pc, [pc, rX, lsl #2]` and `add pc, pc, rX, lsl #2`).

Reports instruction counts per mode, coverage, function count, indirect
branches, VFP use, SVC, and every instruction that reads or writes r9, so we
know whether the game uses r9 as a static base.
"""
import argparse
import collections
import re
import sys
import os

import capstone
from capstone import arm

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from s3e_boundary import Image  # noqa: E402

R9 = arm.ARM_REG_R9
PC, LR, SP = arm.ARM_REG_PC, arm.ARM_REG_LR, arm.ARM_REG_SP


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", nargs="?", default="orig/Payload/kiwi.app/kiwi")
    ap.add_argument("--funcs", help="write function starts (address, mode, source) here")
    args = ap.parse_args()

    img = Image(args.exe)
    lo, size = img.sections["s3e_code"]
    hi = lo + size
    stubs_lo = None
    for a in range(lo, hi - 8, 4):
        if img.word(a) == 0xE59FC000 and img.word(a + 4) == 0xE59FF000:
            stubs_lo = a - 4  # Thumb entry of the first stub
            break
    code_hi = stubs_lo  # game code ends where the import stubs begin

    def in_code(a):
        return lo <= a < code_hi

    def is_stub(a):
        return stubs_lo <= a < hi

    md = {
        "arm": capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM),
        "thumb": capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB),
    }
    for m in md.values():
        m.detail = True

    def raw(a, n):
        out = b""
        for k in range(0, n, 4):
            w = img.word(a + k)
            if w is None:
                break
            out += w.to_bytes(4, "little")
        return out

    seen = {}  # instruction address -> mode ("table" for jump-table words)
    stats = collections.Counter()
    funcs = {}  # address -> (mode, source)
    work = []

    def add_func(a, mode, src):
        if not in_code(a) or (mode == "arm" and a & 3) or a & 1:
            return
        if a in seen and seen[a] != mode:
            stats["mode-conflict"] += 1
            return
        if a not in funcs:
            funcs[a] = (mode, src)
            work.append((a, mode))

    # Roots.
    add_func(0x65F00, "arm", "entry")
    a = 0x65F80
    while (img.word(a) >> 24) == 0xEA:
        add_func(a, "arm", "branch-table")
        a += 4
    data_ranges = [img.sections["s3e_code"], img.sections["s3e_data"]]
    for name in ("__data", "__const", "__common"):
        if name in img.sections:
            data_ranges.append(img.sections[name])
    def code_ptr(v):
        """(address, mode) if v looks like a pointer to a function, else None."""
        if not v:
            return None
        if v & 1 and in_code(v - 1):
            return (v - 1, "thumb")
        if not v & 3 and in_code(v) and (img.word(v) >> 28) == 0xE:
            return (v, "arm")  # ARM functions start with an unconditional instruction
        return None

    # A Thumb pointer to a function starting with push {..., lr} counts on its own;
    # anything else only in a run of two or more code pointers (vtables, tables of
    # callbacks), so that stray constants aren't taken for code.
    thumb_ptrs = table_ptrs = 0
    for base, sz in data_ranges:
        cands = [(p, code_ptr(img.word(p))) for p in range(base, base + sz - 3, 4)]
        for k, (p, c) in enumerate(cands):
            if not c:
                continue
            in_run = (k > 0 and cands[k - 1][1]) or (k + 1 < len(cands) and cands[k + 1][1])
            pushes = c[1] == "thumb" and ((img.half(c[0]) or 0) & 0xFF00) == 0xB500
            if in_run:
                add_func(c[0], c[1], "table")
                table_ptrs += 1
            elif pushes:
                add_func(c[0], c[1], "pointer")
                thumb_ptrs += 1

    r9_users = []
    indirect = collections.Counter()
    vfp = collections.Counter()
    rejected = collections.Counter()
    stub_calls = 0

    media = re.compile(r"^(u|s|q|uq|uh|sh)(add|sub)(8|16)|^(u|s|q|uq|uh|sh)(asx|sax)$|"
                       r"^(u|s)xta?b16|^usada?8|^sel$|^(s|u)sat16|^pkh")
    coproc = ("ldc", "stc", "mcr", "mrc", "cdp", "mcrr", "mrrc")

    def bad(ins, mode):
        """Signs that a walk has left code: these don't occur in the game's armv6 code."""
        mn = ins.mnemonic
        if ins.id == arm.ARM_INS_SVC or ins.id == arm.ARM_INS_BKPT or mn.startswith("udf"):
            return "svc/bkpt/udf"
        if mode == "thumb" and ins.size == 4 and ins.id not in (arm.ARM_INS_BL, arm.ARM_INS_BLX):
            return "thumb2"  # armv6 Thumb has no 32-bit instructions besides BL/BLX
        if media.match(mn):
            return "media"
        if mn.startswith(coproc) and "p10" not in ins.op_str and "p11" not in ins.op_str:
            return "coprocessor"
        if mode == "arm" and (int.from_bytes(ins.bytes, "little") >> 28) == 0xF and ins.id != arm.ARM_INS_BLX:
            return "cond-nv"
        return None

    def walk(start, mode):
        """Walk one function into scratch state; None if it runs into non-code."""
        r = {"seen": {}, "stats": collections.Counter(), "r9": [], "indirect": collections.Counter(),
             "vfp": 0, "stubs": 0, "funcs": []}
        loc = r["seen"]
        pending = [start]
        while pending:
            a = pending.pop()
            while in_code(a):
                if a in seen or a in loc:
                    break
                buf = raw(a & ~3, 8)[a & 2:] if mode == "thumb" else raw(a, 4)
                ins = next(md[mode].disasm(buf, a, 1), None)
                if ins is None:
                    rejected["undecodable"] += 1
                    return None
                why = bad(ins, mode)
                if why:
                    rejected[why] += 1
                    return None
                for k in range(ins.size):
                    loc[a + k] = mode
                r["stats"][mode] += 1
                nxt = a + ins.size
                mn = ins.mnemonic
                cc = ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID)
                if mn.startswith("v") or mn.startswith(("fmrx", "fmxr")):
                    r["vfp"] += 1
                try:
                    rd, wr = ins.regs_access()
                except capstone.CsError:
                    rd, wr = [], []
                if R9 in rd or R9 in wr:
                    r["r9"].append((a, mode, f"{mn} {ins.op_str}", "w" if R9 in wr else "r"))

                ops = ins.operands
                # A literal-pool load of a code pointer (ldr rX, =func) is a function start.
                if ins.id == arm.ARM_INS_LDR and len(ops) == 2 and ops[1].type == arm.ARM_OP_MEM \
                        and ops[1].mem.base == PC and ops[1].mem.index == 0 and PC not in wr:
                    disp = -ops[1].mem.disp if ops[1].subtracted else ops[1].mem.disp
                    lit = (a + 8 if mode == "arm" else (a + 4) & ~3) + disp
                    c = code_ptr(img.word(lit))
                    if c:
                        r["funcs"].append((c[0], c[1], "literal"))
                if ins.id in (arm.ARM_INS_BL, arm.ARM_INS_BLX) and ops and ops[0].type == arm.ARM_OP_IMM:
                    t = ops[0].imm
                    tmode = mode if ins.id == arm.ARM_INS_BL else ("thumb" if mode == "arm" else "arm")
                    if is_stub(t):
                        r["stubs"] += 1
                    elif in_code(t):
                        r["funcs"].append((t & ~1, tmode, "call"))
                    else:
                        rejected["call-outside"] += 1
                        return None
                    a = nxt
                    continue
                if ins.id == arm.ARM_INS_BLX:  # blx reg
                    r["indirect"]["call"] += 1
                    a = nxt
                    continue
                if ins.id == arm.ARM_INS_B and ops and ops[0].type == arm.ARM_OP_IMM:
                    t = ops[0].imm
                    if is_stub(t):
                        r["stubs"] += 1
                    elif in_code(t):
                        pending.append(t)
                    else:
                        rejected["branch-outside"] += 1
                        return None
                    if cc:
                        a = nxt
                        continue
                    break
                if PC in wr:
                    # ARM jump tables
                    if mode == "arm" and ins.id == arm.ARM_INS_LDR and ops[1].type == arm.ARM_OP_MEM \
                            and ops[1].mem.base == PC and ops[1].mem.index != 0:
                        t = a + 8
                        n = 0
                        while n < 512:
                            v = img.word(t + 4 * n)
                            if v is None or not in_code(v & ~1):
                                break
                            if v & 1:
                                r["funcs"].append((v - 1, "thumb", "jump-table"))
                            else:
                                pending.append(v)
                            n += 1
                        for k in range(4 * n):
                            loc[t + k] = "table"
                        r["stats"]["jump-tables"] += 1
                        if cc:
                            a = nxt
                            continue
                        break
                    if mode == "arm" and ins.id == arm.ARM_INS_ADD and len(ops) >= 2 \
                            and ops[1].type == arm.ARM_OP_REG and ops[1].reg == PC:
                        # add pc, pc, rX, lsl #2: a default branch, then a table of B
                        t = a + 8
                        n = 0
                        while n < 512 and (img.word(t + 4 * n) or 0) & 0x0F000000 == 0x0A000000:
                            pending.append(t + 4 * n)
                            n += 1
                        r["stats"]["jump-tables"] += 1
                        if cc:
                            a = nxt
                            continue
                        break
                    is_return = (ins.id == arm.ARM_INS_BX and ops[0].reg == LR) or \
                        (ins.id in (arm.ARM_INS_POP, arm.ARM_INS_LDM) and PC in wr) or \
                        (ins.id == arm.ARM_INS_MOV and ops[1].type == arm.ARM_OP_REG and ops[1].reg == LR)
                    if not is_return:
                        r["indirect"]["jump"] += 1
                    if cc:
                        a = nxt
                        continue
                    break
                a = nxt
        return r

    while work:
        start, mode = work.pop()
        r = walk(start, mode)
        if r is None:
            src = funcs.pop(start)[1]
            stats[f"rejected-{src}"] += 1
            continue
        seen.update(r["seen"])
        stats.update(r["stats"])
        r9_users += r["r9"]
        indirect.update(r["indirect"])
        vfp[mode] += r["vfp"]
        stub_calls += r["stubs"]
        for f in r["funcs"]:
            add_func(*f)

    code_bytes = code_hi - lo
    covered = sum(1 for v in seen.values() if v != "table")
    modes = collections.Counter(funcs[f][0] for f in funcs)
    srcs = collections.Counter(funcs[f][1] for f in funcs)
    print(f"game code {lo:#x}-{code_hi:#x} ({code_bytes} bytes); stubs from {stubs_lo:#x}")
    print(f"functions {len(funcs)}: {dict(modes)}; found by {dict(srcs)} ({table_ptrs} table words, {thumb_ptrs} lone Thumb pointers)")
    print(f"instructions: arm {stats['arm']}, thumb {stats['thumb']}; bytes covered {covered} "
          f"({100 * covered / code_bytes:.1f}% of game code)")
    rej = {k[9:]: v for k, v in stats.items() if k.startswith("rejected-")}
    print(f"roots rejected (walk ran into non-code): {rej}; reasons {dict(rejected)}")
    print(f"branches straight into import stubs {stub_calls} (game calls reach them through the branch table at 0x65f80); jump tables {stats['jump-tables']}; "
          f"indirect calls {indirect['call']}, other indirect jumps {indirect['jump']}")
    print(f"VFP instructions {dict(vfp)}")
    w = [u for u in r9_users if u[3] == "w"]
    print(f"instructions touching r9: {len(r9_users)} ({len(w)} write it)")
    for u in r9_users[:12]:
        print(f"  {u[0]:#x} {u[1]:5} {u[2]} [{u[3]}]")

    if args.funcs:
        with open(args.funcs, "w") as f:
            f.write("address\tmode\tsource\n")
            for a in sorted(funcs):
                f.write(f"{a:08x}\t{funcs[a][0]}\t{funcs[a][1]}\n")


if __name__ == "__main__":
    main()
