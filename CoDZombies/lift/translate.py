"""ARMv6 (A32 + Thumb-1) and VFPv2 instruction semantics as C.

`Translator.insn(ins, mode, ctx)` returns the C statements for one capstone
instruction. Control flow is delegated to `ctx`, which the emitter (or the
instruction tests) provides:

    ctx.branch(target)            -> statement for a direct branch
    ctx.call(target, ret, thumb)  -> statement for a direct call (bl/blx)
    ctx.indirect(ins, expr, kind) -> statement for a write to pc; kind is
                                     "lr" (bx lr / mov pc, lr), "pop" (a load
                                     from the stack), "reg", "load" or "table"
    ctx.icall(expr, ret, thumb)   -> statement for blx reg

Everything is written against the names in runtime/cpu.h. Anything not handled
raises Unsupported.
"""
import capstone
from capstone import arm


class Unsupported(Exception):
    pass


COND = {
    arm.ARM_CC_EQ: "c->z",
    arm.ARM_CC_NE: "!c->z",
    arm.ARM_CC_HS: "c->cf",
    arm.ARM_CC_LO: "!c->cf",
    arm.ARM_CC_MI: "c->n",
    arm.ARM_CC_PL: "!c->n",
    arm.ARM_CC_VS: "c->v",
    arm.ARM_CC_VC: "!c->v",
    arm.ARM_CC_HI: "(c->cf && !c->z)",
    arm.ARM_CC_LS: "(!c->cf || c->z)",
    arm.ARM_CC_GE: "(c->n == c->v)",
    arm.ARM_CC_LT: "(c->n != c->v)",
    arm.ARM_CC_GT: "(!c->z && c->n == c->v)",
    arm.ARM_CC_LE: "(c->z || c->n != c->v)",
}

GPR = {"r%d" % i: i for i in range(13)}
GPR.update({"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15})

SFT_IMM = {arm.ARM_SFT_LSL: "lsl", arm.ARM_SFT_LSR: "lsr", arm.ARM_SFT_ASR: "asr",
           arm.ARM_SFT_ROR: "ror", arm.ARM_SFT_RRX: "rrx"}
SFT_REG = {arm.ARM_SFT_LSL_REG: "lsl", arm.ARM_SFT_LSR_REG: "lsr", arm.ARM_SFT_ASR_REG: "asr",
           arm.ARM_SFT_ROR_REG: "ror", arm.ARM_SFT_RRX_REG: "rrx"}

I = arm  # instruction ids

LOGICAL = {I.ARM_INS_AND: "&", I.ARM_INS_EOR: "^", I.ARM_INS_ORR: "|", I.ARM_INS_BIC: "& ~"}
ARITH = {I.ARM_INS_ADD, I.ARM_INS_ADC, I.ARM_INS_SUB, I.ARM_INS_SBC, I.ARM_INS_RSB, I.ARM_INS_RSC}
COMPARE = {I.ARM_INS_CMP: "sub", I.ARM_INS_CMN: "add", I.ARM_INS_TST: "&", I.ARM_INS_TEQ: "^"}
SHIFT_INS = {I.ARM_INS_LSL: "lsl", I.ARM_INS_LSR: "lsr", I.ARM_INS_ASR: "asr", I.ARM_INS_ROR: "ror",
             I.ARM_INS_RRX: "rrx"}

LOADS = {I.ARM_INS_LDR: ("ld32", 4, None), I.ARM_INS_LDRB: ("ld8", 1, None),
         I.ARM_INS_LDRH: ("ld16", 2, None), I.ARM_INS_LDRSB: ("ld8", 1, "int8_t"),
         I.ARM_INS_LDRSH: ("ld16", 2, "int16_t"), I.ARM_INS_LDRT: ("ld32", 4, None),
         I.ARM_INS_LDRBT: ("ld8", 1, None)}
STORES = {I.ARM_INS_STR: ("st32", 4), I.ARM_INS_STRB: ("st8", 1), I.ARM_INS_STRH: ("st16", 2),
          I.ARM_INS_STRT: ("st32", 4), I.ARM_INS_STRBT: ("st8", 1)}

LDM = {I.ARM_INS_LDM: "ia", I.ARM_INS_LDMIB: "ib", I.ARM_INS_LDMDA: "da", I.ARM_INS_LDMDB: "db",
       I.ARM_INS_POP: "ia"}
STM = {I.ARM_INS_STM: "ia", I.ARM_INS_STMIB: "ib", I.ARM_INS_STMDA: "da", I.ARM_INS_STMDB: "db",
       I.ARM_INS_PUSH: "db"}


CC_SUFFIX = {"eq", "ne", "hs", "cs", "lo", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"}


class Translator:
    def __init__(self):
        self.reg_cache = {}

    # ----------------------------------------------------------- operands
    @staticmethod
    def sflag(ins, mode):
        """Whether the instruction sets flags. Capstone reports update_flags for
        adc/sbc/rsc even without S, so ARM uses the encoding's S bit (bit 20)."""
        if mode == "arm":
            return bool((int.from_bytes(ins.bytes[:4], "little") >> 20) & 1)
        base = ins.mnemonic.split(".")[0]
        return base.endswith("s") and base != "mrs"

    def rname(self, ins, reg):
        return ins.reg_name(reg)

    def gpr(self, ins, reg):
        name = ins.reg_name(reg)
        if name not in GPR:
            raise Unsupported("register %s" % name)
        return GPR[name]

    def pc_value(self, ins, mode):
        return (ins.address + 8) if mode == "arm" else (ins.address + 4)

    def R(self, ins, mode, reg):
        """C expression reading a core register (pc reads as the address + 8/4)."""
        n = self.gpr(ins, reg)
        if n == 15:
            return "0x%08xu" % self.pc_value(ins, mode)
        return "c->r[%d]" % n

    def op2(self, ins, mode, op, want_carry):
        """Value of a flexible second operand; with want_carry, also sets local sc."""
        if op.type == arm.ARM_OP_IMM:
            v = op.imm & 0xFFFFFFFF
            if want_carry and mode == "arm":
                # A rotated 8-bit immediate sets C to bit 31 when the rotation is non-zero.
                enc = int.from_bytes(ins.bytes[:4], "little")
                if (enc >> 25) & 1 and (enc >> 8) & 0xF:
                    return "0x%08xu" % v, ["sc = %d;" % (v >> 31)]
            return "0x%08xu" % v, []
        if op.type != arm.ARM_OP_REG:
            raise Unsupported("operand type %d" % op.type)
        src = self.R(ins, mode, op.reg)
        st = op.shift.type
        if st == arm.ARM_SFT_INVALID or (st in SFT_IMM and op.shift.value == 0 and st != arm.ARM_SFT_RRX):
            return src, []
        if st in SFT_IMM:
            kind, amt = SFT_IMM[st], op.shift.value
            if kind == "rrx":
                return None, ["t2 = rrx_c(%s, c->cf, &sc);" % src]
            return None, ["t2 = %s_c(%s, %du, &sc);" % (kind, src, amt)]
        if st in SFT_REG:
            kind = SFT_REG[st]
            amt = "(%s & 0xff)" % self.R(ins, mode, op.shift.value)
            if kind == "ror":
                # ror by register: amount 0 leaves value and C; else ror by amt % 32
                return None, ["{ uint32_t a_ = %s; t2 = a_ ? ror_c(%s, a_, &sc) : %s; }" % (amt, src, src)]
            return None, ["t2 = %s_c(%s, %s, &sc);" % (kind, src, amt)]
        raise Unsupported("shift type %d" % st)

    def ea(self, ins, mode, mem_op, extra=None):
        """(address expression, writeback expression or None) for a memory operand."""
        m = mem_op.mem
        base_n = self.gpr(ins, m.base)
        if base_n == 15:
            base = "0x%08xu" % ((self.pc_value(ins, mode) & ~3) if mode == "thumb" else self.pc_value(ins, mode))
        else:
            base = "c->r[%d]" % base_n
        neg = mem_op.subtracted
        off = None
        if m.index:
            idx = self.R(ins, mode, m.index)
            st = mem_op.shift.type
            if st in SFT_IMM and (mem_op.shift.value or st == arm.ARM_SFT_RRX):
                kind = SFT_IMM[st]
                if kind == "lsl":
                    idx = "(%s << %d)" % (idx, mem_op.shift.value)
                elif kind == "lsr":
                    idx = "(%s >> %d)" % (idx, mem_op.shift.value) if mem_op.shift.value < 32 else "0u"
                elif kind == "asr":
                    s = min(mem_op.shift.value, 31)
                    idx = "(uint32_t)((int32_t)%s >> %d)" % (idx, s)
                elif kind == "ror":
                    idx = "ror32(%s, %d)" % (idx, mem_op.shift.value)
                else:
                    idx = "((%s >> 1) | ((uint32_t)c->cf << 31))" % idx
            elif m.lshift:
                idx = "(%s << %d)" % (idx, m.lshift)
            off = ("- " if neg else "+ ") + idx
        elif m.disp:
            d = m.disp
            if neg and d > 0:
                d = -d
            off = ("- 0x%xu" % -d) if d < 0 else ("+ 0x%xu" % d)
        post = ins.post_index if hasattr(ins, "post_index") else False
        if extra is not None:
            # post-indexed: [rn], #imm / [rn], rm
            post = True
            if extra.type == arm.ARM_OP_IMM:
                d = extra.imm
                if extra.subtracted and d > 0:
                    d = -d
                off = ("- 0x%xu" % (-d & 0xFFFFFFFF)) if d < 0 else ("+ 0x%xu" % d)
            elif extra.type == arm.ARM_OP_REG:
                idx = self.R(ins, mode, extra.reg)
                st = extra.shift.type
                if st == arm.ARM_SFT_LSL and extra.shift.value:
                    idx = "(%s << %d)" % (idx, extra.shift.value)
                elif st not in (arm.ARM_SFT_INVALID,) and not (st == arm.ARM_SFT_LSL):
                    raise Unsupported("post-index shift")
                off = ("- " if extra.subtracted else "+ ") + idx
            else:
                raise Unsupported("post-index operand")
        if post:
            return base, "%s %s" % (base, off) if off else None, base_n
        addr = "(%s %s)" % (base, off) if off else base
        if ins.writeback:
            return addr, addr, base_n
        return addr, None, base_n

    # ------------------------------------------------------------- driver
    def insn(self, ins, mode, ctx):
        body = self._insn(ins, mode, ctx)
        if mode == "arm" or ins.id == I.ARM_INS_B:
            cc = ins.cc
            if cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID) and ins.id != I.ARM_INS_B:
                return ["if (%s) {" % COND[cc]] + ["    " + s for s in body] + ["}"]
        return body

    def _insn(self, ins, mode, ctx):
        i = ins.id
        ops = ins.operands
        if mode == "thumb" and ins.size == 4 and i not in (I.ARM_INS_BL, I.ARM_INS_BLX):
            raise Unsupported("thumb2 %s" % ins.mnemonic)  # armv6 has no 32-bit Thumb besides BL/BLX
        S = self.sflag(ins, mode)
        name = ins.mnemonic

        # ---------------------------------------------------- branches
        if i == I.ARM_INS_B:
            t = ops[0].imm
            stmt = ctx.branch(t)
            cc = ins.cc
            if cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID):
                return ["if (%s) %s" % (COND[cc], stmt)]
            return [stmt]
        if i == I.ARM_INS_BL or (i == I.ARM_INS_BLX and ops[0].type == arm.ARM_OP_IMM):
            t = ops[0].imm
            ret = ins.address + ins.size
            tmode = mode if i == I.ARM_INS_BL else ("thumb" if mode == "arm" else "arm")
            return [ctx.call(t, ret, mode == "thumb", tmode)]
        if i == I.ARM_INS_BLX:
            ret = ins.address + ins.size
            return ["{ uint32_t t_ = %s;" % self.R(ins, mode, ops[0].reg),
                    "  " + ctx.icall("t_", ret, mode == "thumb") + " }"]
        if i == I.ARM_INS_BX:
            n = self.gpr(ins, ops[0].reg)
            kind = "lr" if n == 14 else "reg"
            return ["{ uint32_t t_ = %s; %s }" % (self.R(ins, mode, ops[0].reg), ctx.indirect(ins, "t_", kind))]

        # ------------------------------------------- data processing
        if i in LOGICAL or i in ARITH or i in (I.ARM_INS_MOV, I.ARM_INS_MOVS, I.ARM_INS_MVN) or i in COMPARE:
            return self.data_processing(ins, mode, ctx)
        if i in SHIFT_INS:
            return self.shift_insn(ins, mode, ctx)
        if i == I.ARM_INS_ADR:
            rd = self.gpr(ins, ops[0].reg)
            base = self.pc_value(ins, mode) & (~3 if mode == "thumb" else ~0)
            v = (base + ops[1].imm) & 0xFFFFFFFF
            if rd == 15:
                raise Unsupported("adr pc")
            return ["c->r[%d] = 0x%08xu;" % (rd, v)]

        # ------------------------------------------------ multiplies
        if i in (I.ARM_INS_MUL, I.ARM_INS_MLA, I.ARM_INS_MLS):
            rd = self.gpr(ins, ops[0].reg)
            a, b = self.R(ins, mode, ops[1].reg), self.R(ins, mode, ops[2].reg if len(ops) > 2 else ops[0].reg)
            if len(ops) == 2:  # thumb muls rd, rm
                a, b = self.R(ins, mode, ops[1].reg), "c->r[%d]" % rd
            expr = "%s * %s" % (a, b)
            if i == I.ARM_INS_MLA:
                expr = "%s * %s + %s" % (a, b, self.R(ins, mode, ops[3].reg))
            elif i == I.ARM_INS_MLS:
                expr = "%s - %s * %s" % (self.R(ins, mode, ops[3].reg), a, b)
            s = "c->r[%d] = %s;" % (rd, expr)
            if S:
                s = "c->r[%d] = nz(c, %s);" % (rd, expr)
            return [s]
        if i in (I.ARM_INS_UMULL, I.ARM_INS_SMULL, I.ARM_INS_UMLAL, I.ARM_INS_SMLAL, I.ARM_INS_UMAAL):
            lo, hi = self.gpr(ins, ops[0].reg), self.gpr(ins, ops[1].reg)
            a, b = self.R(ins, mode, ops[2].reg), self.R(ins, mode, ops[3].reg)
            if i in (I.ARM_INS_UMULL, I.ARM_INS_UMLAL, I.ARM_INS_UMAAL):
                prod = "(uint64_t)%s * %s" % (a, b)
            else:
                prod = "(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s)" % (a, b)
            if i in (I.ARM_INS_UMLAL, I.ARM_INS_SMLAL):
                prod = "%s + (((uint64_t)c->r[%d] << 32) | c->r[%d])" % (prod, hi, lo)
            elif i == I.ARM_INS_UMAAL:
                prod = "%s + c->r[%d] + c->r[%d]" % (prod, hi, lo)
            out = ["{ uint64_t p_ = %s;" % prod, "  c->r[%d] = (uint32_t)p_; c->r[%d] = (uint32_t)(p_ >> 32);" % (lo, hi)]
            if S:
                out.append("  c->n = (uint8_t)(p_ >> 63); c->z = p_ == 0;")
            out.append("}")
            return out
        if name[:4] in ("smul", "smla") and name[4:6] in ("bb", "bt", "tb", "tt", "wb", "wt"):
            return self.dsp_mul(ins, mode)
        if i in (I.ARM_INS_SMMUL, I.ARM_INS_SMMLA, I.ARM_INS_SMMLS):
            rd = self.gpr(ins, ops[0].reg)
            a, b = self.R(ins, mode, ops[1].reg), self.R(ins, mode, ops[2].reg)
            p = "(int64_t)(int32_t)%s * (int32_t)%s" % (a, b)
            if i == I.ARM_INS_SMMLA:
                p = "((int64_t)(int32_t)%s << 32) + %s" % (self.R(ins, mode, ops[3].reg), p)
            elif i == I.ARM_INS_SMMLS:
                p = "((int64_t)(int32_t)%s << 32) - %s" % (self.R(ins, mode, ops[3].reg), p)
            return ["c->r[%d] = (uint32_t)((uint64_t)(%s) >> 32);" % (rd, p)]

        # ------------------------------------------------- misc ALU
        if i == I.ARM_INS_CLZ:
            return ["c->r[%d] = clz32(%s);" % (self.gpr(ins, ops[0].reg), self.R(ins, mode, ops[1].reg))]
        if i in (I.ARM_INS_REV, I.ARM_INS_REV16, I.ARM_INS_REVSH):
            rd, v = self.gpr(ins, ops[0].reg), self.R(ins, mode, ops[1].reg)
            e = {I.ARM_INS_REV: "__builtin_bswap32(%s)" % v,
                 I.ARM_INS_REV16: "(((%s >> 8) & 0x00ff00ffu) | ((%s << 8) & 0xff00ff00u))" % (v, v),
                 I.ARM_INS_REVSH: "(uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)%s)" % v}[i]
            return ["c->r[%d] = %s;" % (rd, e)]
        if i in (I.ARM_INS_SXTB, I.ARM_INS_SXTH, I.ARM_INS_UXTB, I.ARM_INS_UXTH,
                 I.ARM_INS_SXTAB, I.ARM_INS_SXTAH, I.ARM_INS_UXTAB, I.ARM_INS_UXTAH):
            rd = self.gpr(ins, ops[0].reg)
            acc = None
            src_op = ops[1]
            if i in (I.ARM_INS_SXTAB, I.ARM_INS_SXTAH, I.ARM_INS_UXTAB, I.ARM_INS_UXTAH):
                acc, src_op = self.R(ins, mode, ops[1].reg), ops[2]
            v = self.R(ins, mode, src_op.reg)
            rot = src_op.shift.value if src_op.shift.type == arm.ARM_SFT_ROR else 0
            if rot:
                v = "ror32(%s, %d)" % (v, rot)
            cast = {I.ARM_INS_SXTB: "(uint32_t)(int32_t)(int8_t)", I.ARM_INS_SXTAB: "(uint32_t)(int32_t)(int8_t)",
                    I.ARM_INS_SXTH: "(uint32_t)(int32_t)(int16_t)", I.ARM_INS_SXTAH: "(uint32_t)(int32_t)(int16_t)",
                    I.ARM_INS_UXTB: "(uint32_t)(uint8_t)", I.ARM_INS_UXTAB: "(uint32_t)(uint8_t)",
                    I.ARM_INS_UXTH: "(uint32_t)(uint16_t)", I.ARM_INS_UXTAH: "(uint32_t)(uint16_t)"}[i]
            e = "%s(%s)" % (cast, v)
            if acc:
                e = "%s + %s" % (acc, e)
            return ["c->r[%d] = %s;" % (rd, e)]
        if i in (I.ARM_INS_SSAT, I.ARM_INS_USAT):
            rd = self.gpr(ins, ops[0].reg)
            bits = ops[1].imm
            src = ops[2]
            v = self.R(ins, mode, src.reg)
            if src.shift.type == arm.ARM_SFT_LSL and src.shift.value:
                v = "(int64_t)(int32_t)(%s << %d)" % (v, src.shift.value)
            elif src.shift.type == arm.ARM_SFT_ASR and src.shift.value:
                v = "(int64_t)((int32_t)%s >> %d)" % (v, min(src.shift.value, 31))
            else:
                v = "(int64_t)(int32_t)%s" % v
            if i == I.ARM_INS_SSAT:
                return ["c->r[%d] = (uint32_t)ssat(%s, %d, &c->q);" % (rd, v, bits)]
            return ["c->r[%d] = usat(%s, %d, &c->q);" % (rd, v, bits)]
        if i in (I.ARM_INS_QADD, I.ARM_INS_QSUB, I.ARM_INS_QDADD, I.ARM_INS_QDSUB):
            rd = self.gpr(ins, ops[0].reg)
            a, b = self.R(ins, mode, ops[1].reg), self.R(ins, mode, ops[2].reg)
            if i in (I.ARM_INS_QDADD, I.ARM_INS_QDSUB):
                b = "ssat(2 * (int64_t)(int32_t)%s, 32, &c->q)" % b
            else:
                b = "(int32_t)%s" % b
            op = "+" if i in (I.ARM_INS_QADD, I.ARM_INS_QDADD) else "-"
            return ["c->r[%d] = (uint32_t)ssat((int64_t)(int32_t)%s %s (int64_t)%s, 32, &c->q);" % (rd, a, op, b)]

        # ------------------------------------------- loads and stores
        if i in LOADS or i in STORES:
            return self.load_store(ins, mode, ctx)
        if i in (I.ARM_INS_LDRD, I.ARM_INS_STRD):
            rt, rt2 = self.gpr(ins, ops[0].reg), self.gpr(ins, ops[1].reg)
            addr, wb, bn = self.ea(ins, mode, ops[2], ops[3] if len(ops) > 3 else None)
            out = ["{ uint32_t a_ = %s;" % addr]
            if i == I.ARM_INS_LDRD:
                out.append("  uint32_t v0_ = ld32(a_), v1_ = ld32(a_ + 4);")
                if wb:
                    out.append("  c->r[%d] = %s;" % (bn, wb))
                out.append("  c->r[%d] = v0_; c->r[%d] = v1_; }" % (rt, rt2))
            else:
                out.append("  st32(a_, c->r[%d]); st32(a_ + 4, c->r[%d]);" % (rt, rt2))
                if wb:
                    out.append("  c->r[%d] = %s;" % (bn, wb))
                out.append("}")
            return out
        if i in LDM or i in STM:
            return self.ldm_stm(ins, mode, ctx)
        if i in (I.ARM_INS_SWP, I.ARM_INS_SWPB):
            rt, rt2 = self.gpr(ins, ops[0].reg), self.gpr(ins, ops[1].reg)
            base = self.gpr(ins, ops[2].mem.base)
            ld, st = ("ld32", "st32") if i == I.ARM_INS_SWP else ("ld8", "st8")
            return ["{ uint32_t a_ = c->r[%d], o_ = %s(a_); %s(a_, c->r[%d]); c->r[%d] = o_; }"
                    % (base, ld, st, rt2, rt)]
        if i in (I.ARM_INS_LDREX, I.ARM_INS_LDREXB, I.ARM_INS_LDREXH, I.ARM_INS_LDREXD):
            rt = self.gpr(ins, ops[0].reg)
            base = self.gpr(ins, ops[-1].mem.base)
            if i == I.ARM_INS_LDREXD:
                rt2 = self.gpr(ins, ops[1].reg)
                return ["c->excl_addr = c->r[%d]; { uint32_t a_ = c->r[%d]; c->r[%d] = ld32(a_); c->r[%d] = ld32(a_ + 4); }"
                        % (base, base, rt, rt2)]
            ld = {I.ARM_INS_LDREX: "ld32", I.ARM_INS_LDREXB: "ld8", I.ARM_INS_LDREXH: "ld16"}[i]
            return ["c->excl_addr = c->r[%d]; c->r[%d] = %s(c->r[%d]);" % (base, rt, ld, base)]
        if i in (I.ARM_INS_STREX, I.ARM_INS_STREXB, I.ARM_INS_STREXH):
            rd, rt = self.gpr(ins, ops[0].reg), self.gpr(ins, ops[1].reg)
            base = self.gpr(ins, ops[2].mem.base)
            size = {I.ARM_INS_STREX: 4, I.ARM_INS_STREXB: 1, I.ARM_INS_STREXH: 2}[i]
            return ["c->r[%d] = rt_strex(c, c->r[%d], c->r[%d], %d);" % (rd, base, rt, size)]
        if i == I.ARM_INS_CLREX:
            return ["c->excl_addr = 1;"]

        # ---------------------------------------------- status regs
        if i == I.ARM_INS_MRS:
            return ["c->r[%d] = get_apsr(c);" % self.gpr(ins, ops[0].reg)]
        if i == I.ARM_INS_MSR:
            src = ops[1]
            v = "0x%08xu" % (src.imm & 0xFFFFFFFF) if src.type == arm.ARM_OP_IMM else self.R(ins, mode, src.reg)
            field = ins.op_str.split(",")[0].strip().lower()
            mask = 0
            if any(f in field for f in ("_f", "nzcvq", "apsr_nzcv", "_nzcv", "fs", "fsxc", "fc")) or field in ("apsr", "cpsr"):
                mask |= 8
            if "g" in field.split("_")[-1] or "fs" in field:
                mask |= 4
            if mask == 0:
                return ["/* %s %s: control bits ignored */" % (ins.mnemonic, ins.op_str)]
            return ["set_apsr(c, %s, %d);" % (v, mask)]
        if i in (I.ARM_INS_NOP, I.ARM_INS_PLD, I.ARM_INS_YIELD, I.ARM_INS_WFE, I.ARM_INS_WFI, I.ARM_INS_SEV,
                 I.ARM_INS_DMB, I.ARM_INS_DSB, I.ARM_INS_ISB):
            return ["/* %s */" % ins.mnemonic]
        if i == I.ARM_INS_MCR and "p15" in ins.op_str and "c7" in ins.op_str:
            return ["__atomic_thread_fence(__ATOMIC_SEQ_CST); /* %s %s */" % (ins.mnemonic, ins.op_str)]
        if i == I.ARM_INS_BKPT or i == I.ARM_INS_UDF or i == I.ARM_INS_SVC:
            return ['rt_unimplemented(c, 0x%08xu, "%s %s");' % (ins.address, ins.mnemonic, ins.op_str)]

        # ---------------------------------------------------- VFP
        if name.startswith(("v", "fmrx", "fmxr", "fldm", "fstm")):
            return self.vfp(ins, mode, ctx)

        raise Unsupported("%s %s" % (ins.mnemonic, ins.op_str))

    # ------------------------------------------------- data processing
    def data_processing(self, ins, mode, ctx):
        i, ops, S = ins.id, ins.operands, self.sflag(ins, mode)
        if i in COMPARE:
            S = True
            rn, src2 = ops[0], ops[1]
            rd = None
        elif i in (I.ARM_INS_MOV, I.ARM_INS_MOVS, I.ARM_INS_MVN):
            rd, src2 = ops[0], ops[1]
            rn = None
            if i == I.ARM_INS_MOVS:
                i, S = I.ARM_INS_MOV, True  # Thumb movs rd, rm (lsl #0)
        elif len(ops) == 2:  # Thumb two-operand form: rd = rd op rm
            rd, rn, src2 = ops[0], ops[0], ops[1]
        else:
            rd, rn, src2 = ops[0], ops[1], ops[2]
        logical = i in LOGICAL or i in (I.ARM_INS_MOV, I.ARM_INS_MVN, I.ARM_INS_TST, I.ARM_INS_TEQ)
        want_carry = S and logical
        b, pre = self.op2(ins, mode, src2, want_carry)
        out = ["{ uint32_t t2; uint8_t sc = c->cf; (void)sc;"] + ["  " + p for p in pre]
        if b is not None:
            out.append("  t2 = %s;" % b)
        a = self.R(ins, mode, rn.reg) if rn is not None else None
        if mode == "thumb" and rn is not None and self.gpr(ins, rn.reg) == 15 and src2.type == arm.ARM_OP_IMM:
            a = "0x%08xu" % ((ins.address + 4) & ~3)  # Thumb add rd, pc, #imm (ADR) aligns pc
        if i in LOGICAL:
            e = "%s %s t2" % (a, LOGICAL[i])
        elif i == I.ARM_INS_MOV:
            e = "t2"
        elif i == I.ARM_INS_MVN:
            e = "~t2"
        elif i in (I.ARM_INS_TST, I.ARM_INS_TEQ):
            e = "%s %s t2" % (a, COMPARE[i])
        else:
            e = None
        dest = None if rd is None else self.gpr(ins, rd.reg)
        if e is not None:
            if S and dest != 15:
                out.append("  uint32_t r_ = nz(c, %s); c->cf = sc;" % e)
            else:
                out.append("  uint32_t r_ = %s;" % e)
        else:
            if i in (I.ARM_INS_ADD, I.ARM_INS_CMN):
                f, x, y, cin = "add", a, "t2", "0"
            elif i == I.ARM_INS_ADC:
                f, x, y, cin = "add", a, "t2", "c->cf"
            elif i in (I.ARM_INS_SUB, I.ARM_INS_CMP):
                f, x, y, cin = "sub", a, "t2", "1"
            elif i == I.ARM_INS_SBC:
                f, x, y, cin = "sub", a, "t2", "c->cf"
            elif i == I.ARM_INS_RSB:
                f, x, y, cin = "sub", "t2", a, "1"
            elif i == I.ARM_INS_RSC:
                f, x, y, cin = "sub", "t2", a, "c->cf"
            else:
                raise Unsupported(ins.mnemonic)
            if S and dest != 15:
                out.append("  uint32_t r_ = %s_nzcv(c, %s, %s, %s);" % (f, x, y, cin))
            else:
                op = "+" if f == "add" else "-"
                carry = "" if cin in ("0", "1") else (" + %s" % cin if f == "add" else " - !%s" % cin)
                out.append("  uint32_t r_ = %s %s %s%s;" % (x, op, y, carry))
        if dest is None:
            out.append("  (void)r_; }")
            return out
        if dest == 15:
            if S:
                raise Unsupported("flag-setting write to pc")
            kind = "lr" if (i == I.ARM_INS_MOV and src2.type == arm.ARM_OP_REG and self.gpr(ins, src2.reg) == 14
                            and src2.shift.type == arm.ARM_SFT_INVALID) else (
                "table" if (i == I.ARM_INS_ADD and rn is not None and self.gpr(ins, rn.reg) == 15) else "reg")
            out.append("  " + ctx.indirect(ins, "r_", kind) + " }")
            return out
        out.append("  c->r[%d] = r_; }" % dest)
        return out

    def shift_insn(self, ins, mode, ctx):
        """UAL shift aliases: lsl/lsr/asr/ror rd, rm, #imm|rs and Thumb lsls rd, rs; rrx rd, rm."""
        i, ops, S = ins.id, ins.operands, self.sflag(ins, mode)
        kind = SHIFT_INS[i]
        rd = self.gpr(ins, ops[0].reg)
        if rd == 15:
            raise Unsupported("shift to pc")
        if kind == "rrx":
            src, amt = self.R(ins, mode, ops[1].reg), None
        elif len(ops) == 2 and ops[1].shift.type != arm.ARM_SFT_INVALID:
            # ARM: capstone puts the shift on the source operand (lsl r0, r3, #29)
            src = self.R(ins, mode, ops[1].reg)
            st = ops[1].shift.type
            if st in SFT_REG:
                amt = "(%s & 0xff)" % self.R(ins, mode, ops[1].shift.value)
            else:
                amt = "%du" % ops[1].shift.value
        elif len(ops) == 2:
            src = "c->r[%d]" % rd
            amt = ("(%s & 0xff)" % self.R(ins, mode, ops[1].reg)) if ops[1].type == arm.ARM_OP_REG else "%du" % ops[1].imm
        else:
            src = self.R(ins, mode, ops[1].reg)
            amt = ("(%s & 0xff)" % self.R(ins, mode, ops[2].reg)) if ops[2].type == arm.ARM_OP_REG else "%du" % ops[2].imm
        by_reg = amt is not None and not amt.endswith("u")
        if kind == "rrx":
            e = "rrx_c(%s, c->cf, &sc)" % src
        elif kind == "ror" and by_reg:
            e = "(a_ ? ror_c(%s, a_, &sc) : %s)" % (src, src)
        else:
            e = "%s_c(%s, %s, &sc)" % (kind, src, "a_")
        out = ["{ uint8_t sc = c->cf; uint32_t a_ = %s; (void)a_;" % (amt or "0")]
        if S:
            out.append("  c->r[%d] = nz(c, %s); c->cf = sc; }" % (rd, e))
        else:
            out.append("  c->r[%d] = %s; }" % (rd, e))
        return out

    def dsp_mul(self, ins, mode):
        name, ops = ins.mnemonic, ins.operands
        half = lambda r, sel: "(int32_t)(int16_t)(%s%s)" % (r, " >> 16" if sel == "t" else "")
        rd = self.gpr(ins, ops[0].reg)
        a, b = self.R(ins, mode, ops[1].reg), self.R(ins, mode, ops[2].reg)
        x, y = name[4], name[5]
        if name.startswith("smlal"):
            raise Unsupported(name)
        if x == "w":  # smulw<y>/smlaw<y>: (rn * half(rm)) >> 16
            p = "(int32_t)(((int64_t)(int32_t)%s * %s) >> 16)" % (a, half(b, y))
        else:
            p = "(%s * %s)" % (half(a, x), half(b, y))
        if name.startswith("smul"):
            return ["c->r[%d] = (uint32_t)%s;" % (rd, p)]
        acc = self.R(ins, mode, ops[3].reg)
        return ["{ int64_t s_ = (int64_t)%s + (int64_t)(int32_t)%s; c->r[%d] = (uint32_t)s_;"
                " if (s_ != (int32_t)s_) c->q = 1; }" % (p, acc, rd)]

    # --------------------------------------------------- memory
    def load_store(self, ins, mode, ctx):
        i, ops = ins.id, ins.operands
        rt = self.gpr(ins, ops[0].reg)
        mem = ops[1]
        extra = ops[2] if len(ops) > 2 else None
        if mem.type != arm.ARM_OP_MEM:
            raise Unsupported("load/store operand")
        addr, wb, bn = self.ea(ins, mode, mem, extra)
        if i in LOADS:
            fn, size, cast = LOADS[i]
            v = "%s(a_)" % fn
            if cast:
                v = "(uint32_t)(int32_t)(%s)%s" % (cast, v)
            out = ["{ uint32_t a_ = %s; uint32_t v_ = %s;" % (addr, v)]
            if wb and bn != rt:
                out.append("  c->r[%d] = %s;" % (bn, wb.replace(addr, "a_") if wb == addr else wb))
            if rt == 15:
                kind = "pop" if bn == 13 and (wb is not None) else (
                    "table" if bn == 15 or (mem.mem.base and mem.mem.index and self.gpr(ins, mem.mem.base) == 15) else "load")
                out.append("  " + ctx.indirect(ins, "v_", kind) + " }")
                return out
            out.append("  c->r[%d] = v_; }" % rt)
            return out
        fn, size = STORES[i]
        val = self.R(ins, mode, ops[0].reg)
        if rt == 15:
            val = "0x%08xu" % (ins.address + 8 if mode == "arm" else ins.address + 4)
        out = ["{ uint32_t a_ = %s; %s(a_, %s);" % (addr, fn, val)]
        if wb:
            out.append("  c->r[%d] = %s;" % (bn, wb))
        out.append("}")
        return out

    def ldm_stm(self, ins, mode, ctx):
        i, ops = ins.id, ins.operands
        if i in (I.ARM_INS_PUSH, I.ARM_INS_POP):
            base, regs, wb = 13, ops, True
        else:
            base, regs, wb = self.gpr(ins, ops[0].reg), ops[1:], ins.writeback
        nums = [self.gpr(ins, r.reg) for r in regs]
        n = len(nums)
        kind = LDM.get(i) or STM.get(i)
        start = {"ia": "c->r[%d]" % base, "ib": "c->r[%d] + 4" % base,
                 "da": "c->r[%d] - %d" % (base, 4 * n - 4), "db": "c->r[%d] - %d" % (base, 4 * n)}[kind]
        new_base = {"ia": "c->r[%d] + %d", "ib": "c->r[%d] + %d", "da": "c->r[%d] - %d", "db": "c->r[%d] - %d"}[kind] % (base, 4 * n)
        out = ["{ uint32_t a_ = %s;" % start]
        if i in LDM:
            loads = ["  uint32_t v%d_ = ld32(a_ + %d);" % (k, 4 * k) for k in range(n)]
            out += loads
            if wb and base not in nums:
                out.append("  c->r[%d] = %s;" % (base, new_base))
            elif wb:
                out.append("  c->r[%d] = %s;" % (base, new_base))
            for k, r in enumerate(nums):
                if r != 15:
                    out.append("  c->r[%d] = v%d_;" % (r, k))
            if 15 in nums:
                k = nums.index(15)
                kind_ = "pop" if base == 13 else "load"
                out.append("  " + ctx.indirect(ins, "v%d_" % k, kind_))
            out.append("}")
            return out
        for k, r in enumerate(nums):
            val = "0x%08xu" % (ins.address + 8) if r == 15 else "c->r[%d]" % r
            out.append("  st32(a_ + %d, %s);" % (4 * k, val))
        if wb:
            out.append("  c->r[%d] = %s;" % (base, new_base))
        out.append("}")
        return out

    # ------------------------------------------------------------ VFP
    def freg(self, ins, reg):
        name = ins.reg_name(reg)
        if name[0] == "s":
            return "c->f.s[%d]" % int(name[1:]), "s", int(name[1:])
        if name[0] == "d":
            return "c->f.d[%d]" % int(name[1:]), "d", int(name[1:])
        raise Unsupported("vfp register %s" % name)

    def vfp(self, ins, mode, ctx):
        i, ops, name = ins.id, ins.operands, ins.mnemonic
        base = name.split(".")[0]
        if ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID) and base[-2:] in CC_SUFFIX:
            base = base[:-2]  # vaddeq.f32 -> vadd (the condition is applied by insn())
        fstmx = base in ("fldmiax", "fstmiax", "fldmdbx", "fstmdbx")  # FSTMX format: one extra word
        base = {"fldmiax": "vldmia", "fstmiax": "vstmia", "fldmdbx": "vldmdb", "fstmdbx": "vstmdb"}.get(base, base)
        if base in ("vldr", "vstr"):
            r, kind, _ = self.freg(ins, ops[0].reg)
            m = ops[1]
            addr, wb, _ = self.ea(ins, mode, m)
            if wb:
                raise Unsupported("vldr writeback")
            if base == "vldr":
                return ["c->f.%s[%s] = %s;" % (("su" if kind == "s" else "du"), r.split("[")[1][:-1],
                                                ("ld32(%s)" if kind == "s" else "ld64(%s)") % addr)]
            return ["%s(%s, c->f.%s[%s]);" % ("st32" if kind == "s" else "st64", addr,
                                               "su" if kind == "s" else "du", r.split("[")[1][:-1])]
        if base in ("vpush", "vpop", "vldmia", "vldmdb", "vstmia", "vstmdb"):
            if base in ("vpush", "vpop"):
                bn, regs, wb = 13, ops, True
                kind = "db" if base == "vpush" else "ia"
                load = base == "vpop"
            else:
                bn, regs, wb = self.gpr(ins, ops[0].reg), ops[1:], ins.writeback
                kind = base[-2:]
                load = base.startswith("vld")
            fr = [self.freg(ins, r.reg) for r in regs]
            size = 4 if fr[0][1] == "s" else 8
            total = size * len(fr) + (4 if fstmx else 0)
            start = "c->r[%d]" % bn if kind == "ia" else "c->r[%d] - %d" % (bn, total)
            out = ["{ uint32_t a_ = %s;" % start]
            for k, (_, t, idx) in enumerate(fr):
                arr = "su" if t == "s" else "du"
                if load:
                    out.append("  c->f.%s[%d] = %s(a_ + %d);" % (arr, idx, "ld32" if t == "s" else "ld64", k * size))
                else:
                    out.append("  %s(a_ + %d, c->f.%s[%d]);" % ("st32" if t == "s" else "st64", k * size, arr, idx))
            if wb:
                out.append("  c->r[%d] = c->r[%d] %s %d;" % (bn, bn, "+" if kind == "ia" else "-", total))
            out.append("}")
            return out
        if base == "vmov":
            return self.vmov(ins, mode)
        if base in ("vmrs", "fmrx"):
            dst = ins.reg_name(ops[0].reg)
            if dst in ("apsr_nzcv", "APSR_nzcv") or "apsr" in ins.op_str.lower():
                return ["c->n = c->fn; c->z = c->fz; c->cf = c->fc; c->v = c->fv;"]
            if "fpscr" in ins.op_str:
                return ["c->r[%d] = get_fpscr(c);" % self.gpr(ins, ops[0].reg)]
            raise Unsupported("%s %s" % (name, ins.op_str))
        if base in ("vmsr", "fmxr"):
            if "fpscr" in ins.op_str:
                return ["set_fpscr(c, %s);" % self.R(ins, mode, ops[-1].reg)]
            raise Unsupported("%s %s" % (name, ins.op_str))
        if base in ("vcmp", "vcmpe"):
            a = self.freg(ins, ops[0].reg)[0]
            b = self.freg(ins, ops[1].reg)[0] if len(ops) > 1 and ops[1].type == arm.ARM_OP_REG else "0.0"
            return ["fcmp(c, %s, %s);" % (a, b)]
        if base == "vcvt" or base == "vcvtr":
            return self.vcvt(ins, mode)
        two = {"vneg": "-%s", "vabs": "fabs%s(%s)", "vsqrt": "sqrt%s(%s)"}
        if base in two:
            d, t, _ = self.freg(ins, ops[0].reg)
            a = self.freg(ins, ops[1].reg)[0]
            suffix = "f" if t == "s" else ""
            e = ("-%s" % a) if base == "vneg" else two[base] % (suffix, a)
            return ["%s = %s;" % (d, e)]
        three = {"vadd": "%s + %s", "vsub": "%s - %s", "vmul": "%s * %s", "vdiv": "%s / %s",
                 "vnmul": "-(%s * %s)"}
        if base in three:
            d = self.freg(ins, ops[0].reg)[0]
            if len(ops) == 2:
                a, b = d, self.freg(ins, ops[1].reg)[0]
            else:
                a, b = self.freg(ins, ops[1].reg)[0], self.freg(ins, ops[2].reg)[0]
            return ["%s = %s;" % (d, three[base] % (a, b))]
        if base in ("vmla", "vmls", "vnmla", "vnmls"):
            d, t, _ = self.freg(ins, ops[0].reg)
            a, b = self.freg(ins, ops[1].reg)[0], self.freg(ins, ops[2].reg)[0]
            # VFP rounds the product before the add (no fused multiply-add)
            ty = "float" if t == "s" else "double"
            return ["{ %s p_ = %s * %s; %s = %s; }" % (ty, a, b, d,
                    {"vmla": "%s + p_", "vmls": "%s - p_", "vnmla": "-%s - p_", "vnmls": "-%s + p_"}[base] % d)]
        raise Unsupported("%s %s" % (name, ins.op_str))

    def vmov(self, ins, mode):
        ops = ins.operands
        names = [ins.reg_name(o.reg) if o.type == arm.ARM_OP_REG else None for o in ops]
        isf = [n is not None and n[0] in "sd" and n not in ("sp", "sb", "sl") and n[1:].isdigit() for n in names]
        if len(ops) == 2 and isf[0] and isf[1]:
            d, t, _ = self.freg(ins, ops[0].reg)
            s = self.freg(ins, ops[1].reg)
            arr = "su" if t == "s" else "du"
            return ["c->f.%s[%d] = c->f.%s[%d];" % (arr, self.freg(ins, ops[0].reg)[2], arr, s[2])]
        if len(ops) == 2 and isf[0] and not isf[1] and ops[1].type == arm.ARM_OP_REG:
            _, t, idx = self.freg(ins, ops[0].reg)
            return ["c->f.su[%d] = %s;" % (idx, self.R(ins, mode, ops[1].reg))]
        if len(ops) == 2 and not isf[0] and isf[1]:
            _, t, idx = self.freg(ins, ops[1].reg)
            return ["c->r[%d] = c->f.su[%d];" % (self.gpr(ins, ops[0].reg), idx)]
        if len(ops) == 3 and isf[0] and not isf[1]:  # vmov dN, rlo, rhi
            _, t, idx = self.freg(ins, ops[0].reg)
            lo, hi = self.R(ins, mode, ops[1].reg), self.R(ins, mode, ops[2].reg)
            return ["c->f.du[%d] = ((uint64_t)%s << 32) | %s;" % (idx, hi, lo)]
        if len(ops) == 3 and not isf[0] and isf[2]:  # vmov rlo, rhi, dN
            _, t, idx = self.freg(ins, ops[2].reg)
            lo, hi = self.gpr(ins, ops[0].reg), self.gpr(ins, ops[1].reg)
            return ["{ uint64_t v_ = c->f.du[%d]; c->r[%d] = (uint32_t)v_; c->r[%d] = (uint32_t)(v_ >> 32); }" % (idx, lo, hi)]
        if len(ops) == 4 and isf[0] and isf[1]:  # vmov s0, s1, rlo, rhi
            a, b = self.freg(ins, ops[0].reg)[2], self.freg(ins, ops[1].reg)[2]
            return ["c->f.su[%d] = %s; c->f.su[%d] = %s;" % (a, self.R(ins, mode, ops[2].reg), b, self.R(ins, mode, ops[3].reg))]
        if len(ops) == 4 and isf[2] and isf[3]:  # vmov rlo, rhi, s0, s1
            a, b = self.freg(ins, ops[2].reg)[2], self.freg(ins, ops[3].reg)[2]
            ra, rb = self.gpr(ins, ops[0].reg), self.gpr(ins, ops[1].reg)
            return ["{ uint32_t x_ = c->f.su[%d], y_ = c->f.su[%d]; c->r[%d] = x_; c->r[%d] = y_; }" % (a, b, ra, rb)]
        raise Unsupported("vmov %s" % ins.op_str)

    def vcvt(self, ins, mode):
        parts = ins.mnemonic.split(".")
        if ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID) and parts[0][-2:] in CC_SUFFIX:
            parts[0] = parts[0][:-2]
        if len(parts) != 3:
            raise Unsupported(ins.mnemonic)
        dt, st = parts[1], parts[2]
        ops = ins.operands
        d, dk, di = self.freg(ins, ops[0].reg)
        s, sk, si = self.freg(ins, ops[1].reg)
        if len(ops) > 2:
            raise Unsupported("fixed-point vcvt")
        if dt in ("f64", "f32") and st in ("f32", "f64"):
            return ["%s = (%s)%s;" % (d, "double" if dt == "f64" else "float", s)]
        if dt in ("f32", "f64") and st in ("s32", "u32"):
            src = "(int32_t)c->f.su[%d]" % si if st == "s32" else "c->f.su[%d]" % si
            return ["%s = (%s)%s;" % (d, "double" if dt == "f64" else "float", src)]
        if dt in ("s32", "u32") and st in ("f32", "f64"):
            src = s
            if ins.mnemonic.startswith("vcvtr"):
                src = "nearbyint(%s)" % s  # FPSCR rounding mode (round to nearest by default)
            fn = "f2s32" if dt == "s32" else "f2u32"
            return ["c->f.su[%d] = (uint32_t)%s(%s);" % (di, fn, src)]
        raise Unsupported(ins.mnemonic)
