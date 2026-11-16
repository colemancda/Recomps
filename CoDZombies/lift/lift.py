#!/usr/bin/env python3
"""Static ARMv6/Thumb-1 -> C lifter for the game code in kiwi's s3e_code.

    python3 -I lift/lift.py [--exe orig/Payload/kiwi.app/kiwi] [--funcs build/s3e_funcs.tsv] [--out build/gen]

Emits <out>/chunk_NNN.c (lifted functions), <out>/funcs.h (prototypes),
<out>/tables.c (address -> function) and <out>/report.txt.

Each function is `void f_XXXXXXXX(CPU *c)`:
  - bl/blx imm: set lr, call, then check c->r[15] (where the callee returned to)
    against the expected return address;
  - a call to the s3e branch table or a stub: rt_import(c, stub);
  - bx lr / pop {pc} / ldr pc, [sp], #4: c->r[15] = target; return;
  - b to another function, or falling into one: tail call;
  - ARM jump tables: a switch on the loaded or computed target;
  - `mov lr, pc` followed by a write to pc: an indirect call;
  - any other write to pc: a return if the target is the lr the function was
    called with (pop {r3}; bx r3), else rt_jmp (a tail call through the table).
"""
import argparse
import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402
from translate import COND, Translator, Unsupported  # noqa: E402
from capstone import arm  # noqa: E402


class FunctionContext:
    """Control flow for one function being emitted."""

    def __init__(self, em, entry, mode, bodyset, tables, consts=None):
        self.consts = consts or {}
        self.em = em
        self.entry = entry
        self.mode = mode
        self.bodyset = bodyset
        self.tables = tables
        self.labels = set()
        self.ins = None
        self.prev = None

    def thumb_bit(self):
        return 1 if self.mode == "thumb" else 0

    def branch(self, t):
        em = self.em
        if t in self.bodyset:
            self.labels.add(t)
            return "goto L_%08x;" % t
        stub = em.an.stub_of(t)
        if stub is not None:
            em.stats["tail_imports"] += 1
            return "{ rt_import(c, 0x%08xu); return; }" % stub
        if em.an.starts.get(t) == self.mode:
            em.stats["tailcalls"] += 1
            return "{ f_%08x(c); return; }" % t
        em.stats["bad_targets"] += 1
        return "rt_bad_code(c, 0x%08xu, 0x%08xu);" % (self.ins.address, t)

    def check_ret(self, ret):
        return "if ((c->r[15] & ~1u) != 0x%08xu) rt_ret_mismatch(c, 0x%08xu, 0x%08xu);" % (ret, ret, self.ins.address)

    def call(self, t, ret, from_thumb, tmode):
        em = self.em
        lr = "c->r[14] = 0x%08xu;" % (ret | (1 if from_thumb else 0))
        stub = em.an.stub_of(t)
        if stub is not None:
            em.stats["import_calls"] += 1
            return "%s rt_import(c, 0x%08xu);" % (lr, stub)
        t &= ~1
        em.stats["calls"] += 1
        if t in em.an.starts:
            return "%s f_%08x(c); %s" % (lr, t, self.check_ret(ret))
        em.stats["calls_unknown"] += 1
        return "%s rt_call(c, 0x%08xu); %s" % (lr, t | (1 if tmode == "thumb" else 0), self.check_ret(ret))

    def icall(self, expr, ret, from_thumb):
        t = self.consts.get(self.ins.address)
        if t is not None and (t & ~1) in self.em.an.starts:
            self.em.stats["icalls_resolved"] += 1
            return "c->r[14] = 0x%08xu; f_%08x(c); %s" % (ret | (1 if from_thumb else 0), t & ~1, self.check_ret(ret))
        self.em.stats["icalls"] += 1
        return "c->r[14] = 0x%08xu; rt_call(c, %s); %s" % (ret | (1 if from_thumb else 0), expr, self.check_ret(ret))

    def indirect(self, ins, expr, kind):
        em = self.em
        a = ins.address
        t = self.consts.get(a)
        if t is not None and (t & ~1) in em.an.starts and kind == "reg":
            # bx through a register holding a known constant (a PIC veneer)
            if em.an.is_indirect_call(ins, self.mode):
                em.stats["icalls_resolved"] += 1
                return "f_%08x(c); %s" % (t & ~1, self.check_ret(a + ins.size))
            em.stats["tailcalls"] += 1
            return "{ f_%08x(c); return; }" % (t & ~1)
        # `mov lr, pc` right before: an indirect call returning to the next instruction
        if self.em.an.is_indirect_call(ins, self.mode) and kind != "lr":
            em.stats["icalls"] += 1
            return "rt_call(c, %s); %s" % (expr, self.check_ret(a + ins.size))
        if kind in ("lr", "pop"):
            em.stats["returns"] += 1
            return "c->r[15] = %s; return;" % expr
        if a in self.tables:
            em.stats["jump_tables"] += 1
            cases = []
            for t in sorted(set(self.tables[a])):
                if t in self.bodyset:
                    self.labels.add(t)
                    cases.append("case 0x%08xu: goto L_%08x;" % (t, t))
                elif t in em.an.starts:
                    cases.append("case 0x%08xu: f_%08x(c); return;" % (t, t))
            return "switch (%s) { %s default: rt_jmp(c, %s, 0x%08xu); return; }" % (
                expr, " ".join(cases), expr, a)
        # A jump to the address the function was called with is a return,
        # whichever register carries it (Thumb-1: pop {r3}; bx r3).
        em.stats["ijumps"] += 1
        return "if (%s == lr0) { c->r[15] = lr0; return; } rt_jmp(c, %s, 0x%08xu); return;" % (expr, expr, a)


class Emitter:
    def __init__(self, an):
        self.an = an
        self.tr = Translator()
        self.stats = collections.Counter()
        self.unsupported = collections.Counter()

    def function(self, entry, body):
        an = self.an
        mode = an.starts[entry]
        addrs, tables, _, consts = body
        if not addrs:
            self.stats["empty"] += 1
            return "void f_%08x(CPU *c) { rt_bad_code(c, 0, 0x%08xu); }\n" % (entry, entry)
        bodyset = set(addrs)
        self.current_mode = mode
        ctx = FunctionContext(self, entry, mode, bodyset, tables, consts)
        out = []
        prev_end, prev_falls = None, False
        for a in addrs:
            ins = an.decode(a, mode)
            ctx.ins = ins
            if prev_end is not None and prev_end != a and prev_falls:
                # a gap after an instruction that falls through (the next code isn't contiguous)
                out.append("    " + ctx.branch(prev_end))
            try:
                stmts = self.tr.insn(ins, mode, ctx)
            except Unsupported as ex:
                self.unsupported[str(ex).split(" ")[0]] += 1
                stmts = ['rt_unimplemented(c, 0x%08xu, "%s %s");' % (a, ins.mnemonic, ins.op_str)]
            self.stats["insns"] += 1
            out.append((a, "/* %s %s */" % (ins.mnemonic, ins.op_str)))
            out += ["    " + s for s in stmts]
            prev_end, prev_falls = a + ins.size, self.falls_through(ins)
            ctx.prev = ins
        if prev_falls:
            ctx.ins = an.decode(addrs[-1], mode)
            out.append("    " + ctx.branch(prev_end))
        head = ["void f_%08x(CPU *c) {" % entry, "    uint32_t lr0 = c->r[14]; (void)lr0;"]
        if addrs[0] != entry:
            ctx.labels.add(entry)
            head.append("    goto L_%08x;" % entry)
        lines = []
        for item in out:
            if isinstance(item, tuple):
                a, comment = item
                lines.append(("L_%08x: %s" % (a, comment)) if a in ctx.labels else "    " + comment)
            else:
                lines.append(item)
        return "\n".join(head + lines) + "\n}\n"

    def mode_of(self, ins):
        return self.current_mode

    def falls_through(self, ins):
        i = ins.id
        cond = ins.cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID)
        if i == arm.ARM_INS_B:
            return cond
        if i in (arm.ARM_INS_BL, arm.ARM_INS_BLX):
            return True
        if self.an.writes_pc(ins):
            return cond or self.an.is_indirect_call(ins, self.mode_of(ins))
        return True


RET = {"void": 0, "i32": 1, "i64": 2, "-": 3}


def write_imports(tsv, path):
    """imports.c: one row per import stub, from tools/s3e_boundary.py's table."""
    rows = []
    with open(tsv) as f:
        next(f)
        for line in f:
            stub, impl, tramp, ret, args, hooks, kind, sub, calls, _ = line.rstrip("\n").split("\t")
            rows.append((int(stub, 16), int(impl, 16), RET[ret], 1 if args.startswith("stack") else 0, kind, sub))
    with open(path, "w") as f:
        f.write('#include "imports.h"\n\n')
        f.write("const ImportStub g_import_stubs[] = {\n")
        for stub, impl, ret, stack, kind, sub in rows:
            k = {"call": "IMPORT_S3E", "import": "IMPORT_GL", "data": "IMPORT_DATA"}[kind]
            f.write('    {0x%08xu, 0x%08xu, %d, %d, %s, "%s"},\n' % (stub, impl, ret, stack, k, sub))
        f.write("};\nconst uint32_t g_import_stub_count = %d;\n" % len(rows))


def main():
    ap = argparse.ArgumentParser()
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    ap.add_argument("--exe", default=os.path.join(root, "orig/Payload/kiwi.app/kiwi"))
    ap.add_argument("--funcs", default=os.path.join(root, "build/s3e_funcs.tsv"))
    ap.add_argument("--out", default=os.path.join(root, "build/gen"))
    ap.add_argument("--extra", default=os.path.join(root, "extra_funcs.tsv"),
                    help="function starts found at run time (address, mode), from RT_EXTRA_FUNCS")
    ap.add_argument("--chunk", type=int, default=12000, help="instructions per chunk file")
    ap.add_argument("--imports", default=os.path.join(root, "build/s3e_boundary.tsv"),
                    help="tools/s3e_boundary.py output, for <out>/imports.c")
    args = ap.parse_args()

    t0 = time.time()
    an = analyze.Analysis(args.exe, args.funcs, args.extra)
    bodies = an.all_functions()
    t1 = time.time()
    em = Emitter(an)
    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.startswith("chunk_") and f.endswith(".c"):
            os.remove(os.path.join(args.out, f))

    entries = sorted(bodies)
    chunk, n_in_chunk, idx = [], 0, 0

    def flush():
        nonlocal chunk, n_in_chunk, idx
        if not chunk:
            return
        with open(os.path.join(args.out, "chunk_%03d.c" % idx), "w") as f:
            f.write('#include "cpu.h"\n#include "funcs.h"\n\n')
            f.write("\n".join(chunk))
        idx += 1
        chunk, n_in_chunk = [], 0

    for e in entries:
        chunk.append(em.function(e, bodies[e]))
        n_in_chunk += len(bodies[e][0])
        if n_in_chunk >= args.chunk:
            flush()
    flush()

    with open(os.path.join(args.out, "funcs.h"), "w") as f:
        f.write("#ifndef CODZ_FUNCS_H\n#define CODZ_FUNCS_H\n#include \"cpu.h\"\n")
        for e in entries:
            f.write("void f_%08x(CPU *c);\n" % e)
        f.write("#endif\n")
    with open(os.path.join(args.out, "tables.c"), "w") as f:
        f.write('#include "funcs.h"\n\n')
        f.write("typedef struct { uint32_t addr; uint8_t thumb; GuestFn fn; } LiftedFn;\n")
        f.write("const LiftedFn g_lifted[] = {\n")
        for e in entries:
            f.write("    {0x%08xu, %d, f_%08x},\n" % (e, 1 if an.starts[e] == "thumb" else 0, e))
        f.write("};\nconst uint32_t g_lifted_count = %d;\n" % len(entries))
    if args.imports:
        write_imports(args.imports, os.path.join(args.out, "imports.c"))
    t2 = time.time()
    modes = collections.Counter(an.starts[e] for e in entries)
    report = [
        "functions %d (%s)" % (len(entries), ", ".join("%s %d" % kv for kv in sorted(modes.items()))),
        "chunks %d" % idx,
        "stats " + ", ".join("%s=%d" % kv for kv in sorted(em.stats.items())),
        "untranslated %d: %s" % (sum(em.unsupported.values()), dict(em.unsupported.most_common(20))),
        "time %.1fs analysis + %.1fs emission" % (t1 - t0, t2 - t1),
    ]
    with open(os.path.join(args.out, "report.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))


if __name__ == "__main__":
    main()
