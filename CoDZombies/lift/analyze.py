"""Function bodies of the game code (s3e_code), for the lifter and the tests.

Function starts come from tools/s3e_survey.py (build/s3e_funcs.tsv); call
targets found while walking bodies are added as further starts. A body is every
instruction reachable from the entry without a call: fall-through, B, and the
targets of ARM jump tables. A branch or fall-through to another function's
start ends the walk there (a tail call).
"""
import os
import re
import sys

import capstone
from capstone import arm

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from s3e_boundary import Image  # noqa: E402

PC, LR, SP = arm.ARM_REG_PC, arm.ARM_REG_LR, arm.ARM_REG_SP
MEDIA = re.compile(r"^(u|s|q|uq|uh|sh)(add|sub)(8|16)|^(u|s|q|uq|uh|sh)(asx|sax)|"
                   r"^(u|s)xta?b16|^usada?8|^sel|^(s|u)sat16|^pkh")
BRANCH_TABLE = 0x65F80


class Analysis:
    def __init__(self, exe, funcs_tsv, extra_tsv=None):
        self.img = img = Image(exe)
        self.lo, size = img.sections["s3e_code"]
        self.hi = self.lo + size
        self.stubs_lo = None
        for a in range(self.lo, self.hi - 8, 4):
            if img.word(a) == 0xE59FC000 and img.word(a + 4) == 0xE59FF000:
                self.stubs_lo = a - 4
                break
        # Branch table: entry address -> stub address
        self.table = {}
        a = BRANCH_TABLE
        while (img.word(a) >> 24) == 0xEA:
            imm = img.word(a) & 0xFFFFFF
            imm -= (1 << 24) if imm & 0x800000 else 0
            self.table[a] = a + 8 + imm * 4
            a += 4
        self.md = {}
        for mode, flag in (("arm", capstone.CS_MODE_ARM), ("thumb", capstone.CS_MODE_THUMB)):
            m = capstone.Cs(capstone.CS_ARCH_ARM, flag)
            m.detail = True
            self.md[mode] = m
        self.cache = {}
        self.starts = {}
        with open(funcs_tsv) as f:
            next(f)
            for line in f:
                addr, mode, _ = line.split("\t")
                a = int(addr, 16)
                if a not in self.table:
                    self.starts[a] = mode
        # Function starts the runtime reached but the survey didn't find
        # (runtime/dispatch.c appends them: address and mode).
        if extra_tsv and os.path.exists(extra_tsv):
            with open(extra_tsv) as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 2 and not parts[0].startswith("#"):
                        a = int(parts[0], 16)
                        if self.in_code(a) and a not in self.table:
                            self.starts.setdefault(a, parts[1])

    # ------------------------------------------------------------ queries
    def in_code(self, a):
        return self.lo <= a < self.stubs_lo

    def stub_of(self, t):
        """Stub address if t is an import: a stub, its Thumb entry, or a branch-table slot."""
        if t in self.table:
            return self.table[t]
        if self.stubs_lo <= t < self.hi:
            off = (t - self.stubs_lo) % 0x14
            return t + 4 if off == 0 else t - off + 4
        return None

    def decode(self, a, mode):
        key = (a, mode)
        if key in self.cache:
            return self.cache[key]
        img = self.img
        w0 = img.word(a & ~3)
        w1 = img.word((a & ~3) + 4)
        if w0 is None:
            self.cache[key] = None
            return None
        buf = w0.to_bytes(4, "little") + (w1 or 0).to_bytes(4, "little")
        buf = buf[a & 2:] if mode == "thumb" else buf[:4]
        ins = next(self.md[mode].disasm(buf, a, 1), None)
        if ins is not None and not self.plausible(ins, mode):
            ins = None
        self.cache[key] = ins
        return ins

    @staticmethod
    def plausible(ins, mode):
        """False for encodings the game's armv6 user code can't contain."""
        if mode == "thumb" and ins.size == 4 and ins.id not in (arm.ARM_INS_BL, arm.ARM_INS_BLX):
            return False  # Thumb-2
        if "^" in ins.op_str:
            return False  # ldm/stm of user-bank registers, exception return
        if ins.id in (arm.ARM_INS_SVC, arm.ARM_INS_BKPT, arm.ARM_INS_UDF, arm.ARM_INS_CBZ, arm.ARM_INS_CBNZ,
                      arm.ARM_INS_BXNS, arm.ARM_INS_BLXNS):
            return False  # also not armv6: cbz/cbnz (Thumb-2), bxns/blxns (v8-M)
        if MEDIA.match(ins.mnemonic):
            return False  # armv6 SIMD media instructions: not in the game's code
        return True

    @staticmethod
    def writes_pc(ins):
        try:
            return PC in ins.regs_access()[1]
        except capstone.CsError:
            return False

    def is_indirect_call(self, ins, mode):
        """A write to pc right after `mov lr, pc`: an ARM indirect call that
        returns to the next instruction."""
        if mode != "arm" or ins.id in (arm.ARM_INS_BL, arm.ARM_INS_BLX):
            return False
        p = self.decode(ins.address - 4, mode)
        return (p is not None and p.id == arm.ARM_INS_MOV and len(p.operands) == 2
                and p.operands[0].type == arm.ARM_OP_REG and p.operands[0].reg == LR
                and p.operands[1].type == arm.ARM_OP_REG and p.operands[1].reg == PC
                and p.cc in (arm.ARM_CC_AL, arm.ARM_CC_INVALID))

    def jump_table(self, ins, mode, body_addrs=None):
        """Targets of an ARM jump table at ins, or None if ins isn't one."""
        if mode != "arm":
            return None
        ops = ins.operands
        bound = None
        prev = self.decode(ins.address - 4, mode)
        if prev is not None and prev.id == arm.ARM_INS_CMP and prev.operands[1].type == arm.ARM_OP_IMM:
            bound = prev.operands[1].imm + 1
        if ins.id == arm.ARM_INS_LDR and len(ops) >= 2 and ops[1].type == arm.ARM_OP_MEM \
                and ops[1].mem.base == PC and ops[1].mem.index != 0:
            t = ins.address + 8
            out = []
            while len(out) < (bound or 1024):
                v = self.img.word(t + 4 * len(out))
                if v is None or v & 3 or not self.in_code(v):
                    break
                out.append(v)
            return out
        if ins.id == arm.ARM_INS_ADD and len(ops) >= 3 and ops[1].type == arm.ARM_OP_REG and ops[1].reg == PC \
                and ops[2].type == arm.ARM_OP_REG:
            t = ins.address + 8
            out = []
            while len(out) < (bound or 1024):
                w = self.img.word(t + 4 * len(out)) or 0
                if (w & 0x0F000000) != 0x0A000000:
                    break
                out.append(t + 4 * len(out))
            return out
        return None

    # --------------------------------------------------------------- body
    def track(self, ins, mode, known):
        """Constants in registers along straight-line code: literal-pool loads from
        the (read-only) image and pc-relative adds, as in PIC veneers
        (ldr r3, [pc, #4]; add r3, pc, r3; bx r3) and interworking veneers
        (add ip, pc, #1; bx ip)."""
        ops = ins.operands
        try:
            written = ins.regs_access()[1]
        except Exception:
            written = []
        val = None
        if ins.id == arm.ARM_INS_LDR and len(ops) == 2 and ops[1].type == arm.ARM_OP_MEM \
                and ops[1].mem.base == PC and ops[1].mem.index == 0 and ins.cc in (arm.ARM_CC_AL, arm.ARM_CC_INVALID):
            disp = -ops[1].mem.disp if ops[1].subtracted else ops[1].mem.disp
            lit = (ins.address + 8 if mode == "arm" else (ins.address + 4) & ~3) + disp
            if self.lo <= lit < self.hi:
                val = self.img.word(lit)
        elif ins.id == arm.ARM_INS_ADD and len(ops) == 3 and ops[1].type == arm.ARM_OP_REG \
                and ops[1].reg == PC and ops[2].type == arm.ARM_OP_IMM and ins.cc in (arm.ARM_CC_AL, arm.ARM_CC_INVALID):
            # add ip, pc, #1; bx ip: an ARM-to-Thumb veneer continuing at the next instruction
            base = ins.address + 8 if mode == "arm" else (ins.address + 4) & ~3
            val = (base + ops[2].imm) & 0xFFFFFFFF
        elif ins.id == arm.ARM_INS_ADD and len(ops) == 3 and ops[1].type == arm.ARM_OP_REG \
                and ops[2].type == arm.ARM_OP_REG and not ops[2].shift.type and ins.cc in (arm.ARM_CC_AL, arm.ARM_CC_INVALID):
            pcv = ins.address + (8 if mode == "arm" else 4)
            a, b = ops[1].reg, ops[2].reg
            va = pcv if a == PC else known.get(a)
            vb = pcv if b == PC else known.get(b)
            if va is not None and vb is not None and (a == PC or b == PC):
                val = (va + vb) & 0xFFFFFFFF
        for r in written:
            known.pop(r, None)
        if val is not None and ops and ops[0].type == arm.ARM_OP_REG:
            known[ops[0].reg] = val

    def body(self, entry):
        """(sorted instruction addresses, {addr: jump-table targets}, new call
        targets, {addr: constant target of a bx/blx through a register})."""
        mode = self.starts[entry]
        seen = set()
        tables = {}
        calls = {}
        consts = {}
        work = [entry]
        while work:
            a = work.pop()
            known = {}
            while self.in_code(a) and a not in seen:
                if a != entry and a in self.starts:
                    break  # fall-through or branch into another function: tail call
                ins = self.decode(a, mode)
                if ins is None:
                    break
                seen.add(a)
                nxt = a + ins.size
                i, ops = ins.id, ins.operands
                cond = ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID)
                if i in (arm.ARM_INS_BX, arm.ARM_INS_BLX) and ops and ops[0].type == arm.ARM_OP_REG \
                        and ops[0].reg in known:
                    t = known[ops[0].reg]
                    if self.in_code(t & ~1) and self.stub_of(t & ~1) is None:
                        consts[a] = t
                        calls[t & ~1] = "thumb" if t & 1 else "arm"
                self.track(ins, mode, known)
                if i == arm.ARM_INS_B:
                    t = ops[0].imm
                    if self.in_code(t) and not (t in self.starts and t != entry):
                        work.append(t)
                    if cond:
                        a = nxt
                        continue
                    break
                if i in (arm.ARM_INS_BL, arm.ARM_INS_BLX) and ops and ops[0].type == arm.ARM_OP_IMM:
                    known.clear()
                    t = ops[0].imm
                    tmode = mode if i == arm.ARM_INS_BL else ("thumb" if mode == "arm" else "arm")
                    if self.in_code(t & ~1) and self.stub_of(t) is None:
                        calls[t & ~1] = tmode
                    a = nxt
                    continue
                if self.writes_pc(ins) and i != arm.ARM_INS_BLX:
                    if self.is_indirect_call(ins, mode):
                        a = nxt
                        continue
                    jt = self.jump_table(ins, mode)
                    if jt:
                        tables[a] = jt
                        work.extend(t for t in jt if not (t in self.starts and t != entry))
                    if cond:
                        a = nxt
                        continue
                    break
                a = nxt
        return sorted(seen), tables, calls, consts

    def all_functions(self):
        """Walk every function, adding call targets as new starts until none are new."""
        bodies = {}
        work = list(self.starts)
        while work:
            e = work.pop()
            if e in bodies:
                continue
            b = self.body(e)
            bodies[e] = b
            for t, m in b[2].items():
                if t not in self.starts:
                    self.starts[t] = m
                    work.append(t)
        return bodies
