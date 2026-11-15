#!/usr/bin/env python3
"""Static x86-32 -> C lifter.

  lift.py --pe GunBound.gme --csv PROGRESS.csv --out build/gen
  lift.py --raw blob.bin --base 0x401000 --entries 401000,401020 --out build/test/gen
  lift.py --pe granny.dll --module granny --rebase 0x10100000 --out build/gen/granny

tables.c defines `RtImageDesc rt_image_<module>`. With --module, functions are
named f_<module>_<addr> so several images link into one program, and the build
lists the images in rt_images[]; without it the module is "main" and tables.c
defines rt_images[] itself. --rebase applies the PE's relocations
to lift it at another base.

Emits  <out>/chunk_NNN.c  (lifted functions), <out>/funcs.h (prototypes),
<out>/tables.c (function table, names, return sites) and <out>/report.txt.
"""
import argparse
import csv
import io
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from image import Image  # noqa: E402
from analyze import Analysis, JCC, LOOPS, TERMINATORS  # noqa: E402
from translate import ALL_FLAGS, COND, Translator, Unsupported, insn_name  # noqa: E402


# C name of a lifted function; main() sets the module prefix (f_<module>_<addr>).
FN = "f_%08x"


def fn(a):
    return FN % a


class Emitter:
    def __init__(self, an, tr, flag_liveness=True):
        self.an = an
        self.tr = tr
        self.img = an.img
        self.flag_liveness = flag_liveness
        self.stats = {"insns": 0, "ijmp_table": 0, "ijmp_import": 0, "ijmp_dynamic": 0, "icall": 0,
                      "int3": 0, "bad_targets": 0, "tailcalls": 0}
        self.ijmp_dynamic_sites = []
        self.retsites = set()
        self.iat = {va: (dll, name) for va, dll, name in self.img.imports}
        self.hooks = set()

    # ------------------------------------------------------------ helpers
    def goto(self, t, frm, bodyset, labels):
        if t in bodyset:
            labels.add(t)
            return "goto L_%08x;" % t
        if t in self.an.starts:
            self.stats["tailcalls"] += 1
            return "{ %s(c); return; }" % fn(t)
        if self.img.in_text(t):
            self.stats["bad_targets"] += 1
            return "rt_bad_code(c, 0x%08xu, 0x%08xu);" % (frm, t)
        return "{ rt_jmp(c, 0x%08xu, 0x%08xu); return; }" % (t, frm)

    # ----------------------------------------------------------- function
    def function(self, entry):
        an = self.an
        if entry in self.hooks:
            # Replaced by a host handler (rt_hook in the runtime); the guest code is not lifted.
            return "void %s(CPU *c) { rt_hook(c, 0x%08xu); }\n" % (fn(entry), entry), 1
        insns = an.insns
        body = an.function_body(entry)
        if not body:
            self.stats["bad_targets"] += 1
            return "void %s(CPU *c) { rt_bad_code(c, 0, 0x%08xu); }\n" % (fn(entry), entry), 0
        bodyset = set(body)
        n = len(body)
        idx = {a: i for i, a in enumerate(body)}
        trs = [None] * n
        uses = [0] * n
        kills = [0] * n
        succs = [()] * n
        exits = [False] * n

        def succ_of(t):
            return idx[t] if t in bodyset else None

        for i, a in enumerate(body):
            ins = insns[a]
            mn = ins.mn
            nxt = succ_of(ins.next)
            if mn in TERMINATORS:
                uses[i] = ALL_FLAGS
                exits[i] = True
            elif mn == "jmp":
                t = an.direct_target(ins)
                if t is not None:
                    s = succ_of(t)
                    if s is None:
                        exits[i] = True
                    else:
                        succs[i] = (s,)
                else:
                    tab = an.tables.get(a)
                    exits[i] = True
                    if tab:
                        succs[i] = tuple(idx[t] for t in set(tab[1]) if t in bodyset)
            elif mn in JCC or mn in LOOPS:
                t = an.direct_target(ins)
                if mn in JCC:
                    uses[i] = COND[mn[1:]][1]
                elif mn in ("loope", "loopne"):
                    uses[i] = 8
                ss = []
                for s in (succ_of(t), nxt):
                    if s is None:
                        exits[i] = True
                    else:
                        ss.append(s)
                succs[i] = tuple(ss)
            elif mn == "call":
                uses[i] = ALL_FLAGS
                if nxt is None:
                    exits[i] = True
                else:
                    succs[i] = (nxt,)
            else:
                tr = self.tr.translate(ins)
                trs[i] = tr
                uses[i] = tr.uses
                kills[i] = tr.kills
                if nxt is None:
                    exits[i] = True
                else:
                    succs[i] = (nxt,)

        live_out = [ALL_FLAGS] * n
        if self.flag_liveness:
            live_in = [0] * n
            live_out = [0] * n
            changed = True
            while changed:
                changed = False
                for i in range(n - 1, -1, -1):
                    out = ALL_FLAGS if exits[i] else 0
                    for s in succs[i]:
                        out |= live_in[s]
                    lin = uses[i] | (out & ~kills[i])
                    if lin != live_in[i] or out != live_out[i]:
                        live_in[i] = lin
                        live_out[i] = out
                        changed = True

        labels = set()
        lines = []
        for i, a in enumerate(body):
            ins = insns[a]
            mn = ins.mn
            falls = True
            if trs[i] is not None:
                code = trs[i].render(live_out[i])
            else:
                code, falls = self.control(ins, entry, bodyset, labels)
            if falls:
                nxt = ins.next
                if i + 1 >= n or body[i + 1] != nxt:
                    code += " " + self.goto(nxt, a, bodyset, labels)
            lines.append((a, ins.text, code))
        self.stats["insns"] += n

        out = ["void %s(CPU *c) {" % fn(entry)]
        if body[0] != entry:
            labels.add(entry)
            out.append("    goto L_%08x;" % entry)
        for a, text, code in lines:
            lab = "L_%08x: " % a if a in labels else ""
            out.append("    %s/* %08x %s */ { %s }" % (lab, a, text.replace("*/", "* /"), code))
        out.append("}\n")
        return "\n".join(out), n

    # ------------------------------------------------- control transfers
    def control(self, ins, entry, bodyset, labels):
        """Returns (code, falls_through)."""
        an = self.an
        mn = ins.mn
        a = ins.addr
        tr = self.tr
        if mn == "ret":
            nbytes = ins.ops[0].imm if ins.ops else 0
            return "RET(%d);" % nbytes, False
        if mn == "int3":
            self.stats["int3"] += 1
            return 'rt_unimplemented_insn(c, 0x%08xu, "int3");' % a, False
        if mn in TERMINATORS:
            tr.note_trap(ins)
            return 'rt_unimplemented_insn(c, 0x%08xu, "%s");' % (a, insn_name(ins)), False
        if mn == "jmp":
            t = an.direct_target(ins)
            if t is not None:
                return self.goto(t, a, bodyset, labels), False
            try:
                target = tr.rd(ins.ops[0])
            except Unsupported:
                tr.note_trap(ins)
                return 'rt_unimplemented_insn(c, 0x%08xu, "%s");' % (a, insn_name(ins)), False
            tab = an.tables.get(a)
            if tab:
                self.stats["ijmp_table"] += 1
                cases = []
                for t in sorted(set(tab[1])):
                    if t in bodyset:
                        labels.add(t)
                        cases.append("case 0x%08xu: goto L_%08x;" % (t, t))
                return "uint32_t t = %s; switch (t) { %s default: rt_jmp(c, t, 0x%08xu); return; }" % (
                    target, " ".join(cases), a), False
            op = ins.ops[0]
            if op.t == "m" and op.base is None and op.index is None and op.disp in self.iat:
                self.stats["ijmp_import"] += 1
            else:
                self.stats["ijmp_dynamic"] += 1
                self.ijmp_dynamic_sites.append((a, ins.text))
            return "uint32_t t = %s; rt_jmp(c, t, 0x%08xu); return;" % (target, a), False
        if mn in JCC:
            t = an.direct_target(ins)
            return "if (%s) %s" % (COND[mn[1:]][0], self.goto(t, a, bodyset, labels)), True
        if mn in LOOPS:
            t = an.direct_target(ins)
            cond = {"jecxz": "ECX == 0", "jcxz": "CX == 0", "loop": "--ECX != 0",
                    "loope": "--ECX != 0 && c->zf", "loopne": "--ECX != 0 && !c->zf"}[mn]
            return "if (%s) %s" % (cond, self.goto(t, a, bodyset, labels)), True
        if mn == "call":
            ra = ins.next
            t = an.direct_target(ins)
            if t is not None:
                if t == ra:
                    return "PUSH32(0x%08xu);" % ra, True
                self.retsites.add(ra)
                if t in an.starts:
                    return "CALLF(%s, 0x%08xu);" % (fn(t), ra), True
                return "CALLI(0x%08xu, 0x%08xu);" % (t, ra), True
            try:
                target = tr.rd(ins.ops[0])
            except Unsupported:
                tr.note_trap(ins)
                return 'rt_unimplemented_insn(c, 0x%08xu, "%s");' % (a, insn_name(ins)), False
            self.retsites.add(ra)
            self.stats["icall"] += 1
            return "CALLI(%s, 0x%08xu);" % (target, ra), True
        raise AssertionError("unhandled control instruction " + mn)


def repair_scope_tables(an):
    """Recover SEH scope-table handler fields that the import rebuilder overwrote.

    The tool that rebuilt this dump's import table took the handler field of some
    `__try/__finally` scope tables for IAT slots (each funclet leads to
    LeaveCriticalSection) and listed them as imports of ntdll, so the loader would
    replace them with import stubs. A scope-table entry is {enclosing level,
    filter, handler}; a single-entry `__finally` table reads {-1, 0, handler}. The
    owning function names the table (`push offset table; call __SEH_prolog`) and,
    on the normal path out of the guarded block, resets its try level and calls
    the funclet directly (`or dword ptr [ebp-4], -1; call funclet`): that call
    target is the lost handler address.

    Returns (patches, problems): patches is a list of (slot va, handler va)."""
    img = an.img
    insns = an.insns
    order = sorted(insns)
    pos = {a: i for i, a in enumerate(order)}
    pushes = {}
    for a in order:
        ins = insns[a]
        if ins.mn == "push" and ins.ops and ins.ops[0].t == "i":
            pushes.setdefault(ins.ops[0].imm, []).append(a)
    patches, problems = [], []
    for va, dll, name in img.imports:
        if img.in_text(va):
            continue
        table = va - 8
        if not (img.has(table, 12) and img.u32(table) == 0xFFFFFFFF and img.u32(table + 4) == 0):
            continue
        sites = pushes.get(table, [])
        if not sites:
            continue  # an ordinary IAT slot that happens to follow {-1, 0}
        targets = set()
        for site in sites:
            i = pos[site]
            prolog = an.direct_target(insns[order[i + 1]]) if insns[order[i + 1]].mn == "call" else None
            for k in range(i + 2, min(i + 400, len(order) - 1)):
                ins = insns[order[k]]
                if order[k] - site > 0x600:
                    break
                if prolog is not None and ins.mn == "call" and an.direct_target(ins) == prolog:
                    break  # the next function's SEH prolog
                if ins.mn == "or" and ins.text.replace(" ", "") == "ordwordptr[ebp-4],0xffffffff":
                    nxt = insns.get(ins.next)
                    if nxt is not None and nxt.mn == "call":
                        t = an.direct_target(nxt)
                        if t is not None and site < t < site + 0x600:
                            targets.add(t)
        if len(targets) == 1:
            patches.append((va, targets.pop()))
        else:
            problems.append("scope table %08x (import slot %08x %s!%s): %d candidate funclets" % (
                table, va, dll, name, len(targets)))
    return patches, problems


def write_if_changed(path, text):
    """Write `text` to `path` unless the file already holds it (keeps its mtime for make/ninja)."""
    try:
        with open(path) as fh:
            if fh.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as fh:
        fh.write(text)


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pe")
    ap.add_argument("--raw")
    ap.add_argument("--base", default="0x401000")
    ap.add_argument("--entries", default="", help="comma-separated hex entry addresses (raw mode)")
    ap.add_argument("--csv", help="PROGRESS.csv-style function list (addr,size,name,...)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--module", help="C identifier for this image (multi-image builds)")
    ap.add_argument("--rebase", help="hex base to relocate the PE to before lifting")
    ap.add_argument("--hook", default="", help="comma-separated hex addresses of functions replaced by host "
                    "handlers: their body becomes a call to rt_hook()")
    ap.add_argument("--no-pointer-scan", action="store_true")
    ap.add_argument("--no-flag-liveness", action="store_true",
                    help="compute every flag at every instruction (slow code, useful to validate the liveness pass)")
    args = ap.parse_args()

    global FN
    if args.module:
        if not args.module.isidentifier():
            ap.error("--module must be a C identifier")
        FN = "f_%s_%%08x" % args.module
    t0 = time.time()
    names = {}
    seeds = []
    if args.pe:
        img = Image.from_pe(args.pe, rebase=int(args.rebase, 16) if args.rebase else None)
        seeds.append(img.entry)
        names[img.entry] = "entry"
        for va, nm in img.exports:
            if img.in_text(va):  # exported data (e.g. _iob) is not code
                seeds.append(va)
                names.setdefault(va, nm)
    else:
        img = Image.from_raw(args.raw, int(args.base, 16))
    for e in args.entries.split(","):
        if e:
            if "=" in e:
                e, nm = e.split("=", 1)
                names[int(e, 16)] = nm
            seeds.append(int(e, 16))
    if args.csv:
        for r in csv.DictReader(open(args.csv)):
            a = int(r["addr"], 16)
            seeds.append(a)
            names[a] = r["name"]

    an = Analysis(img, seeds, names)
    an.run(scan_pointers=bool(args.pe) and not args.no_pointer_scan)
    t1 = time.time()

    patches, patch_problems = repair_scope_tables(an)
    for _, handler in patches:
        an.add_start(handler, "seh-funclet")
    an.explore()

    tr = Translator()
    em = Emitter(an, tr, flag_liveness=not args.no_flag_liveness)
    for h in args.hook.split(","):
        if h:
            em.hooks.add(int(h, 16))
            if int(h, 16) not in an.starts:
                sys.exit("--hook %s is not a function start" % h)
    os.makedirs(args.out, exist_ok=True)

    # Chunks are keyed by address (one per 16 KiB of code), so that a change to the
    # lift rewrites only the chunks it touches and the build recompiles only those.
    # Each chunk declares just the functions it uses: funcs.h changes whenever a
    # function is added, and every chunk including it would be rebuilt.
    funcs = sorted(an.starts)
    buckets = {}
    for f in funcs:
        code, n = em.function(f)
        buckets.setdefault(f >> 14, []).append(code)
    proto = re.compile(r"\b%s\b" % FN.replace("%08x", "[0-9a-f]{8}"))
    wanted = set()
    for key, codes in buckets.items():
        name = "chunk_%05x.c" % key
        wanted.add(name)
        body = "\n".join(codes)
        used = sorted(set(proto.findall(body)))
        text = ('/* Generated by recomp/lift/lift.py. Do not edit, do not commit. */\n'
                '#include "cpu.h"\n\n' + "".join("void %s(CPU *c);\n" % u for u in used) + "\n" + body)
        write_if_changed(os.path.join(args.out, name), text)
    for f in os.listdir(args.out):
        if f.startswith("chunk_") and f.endswith(".c") and f not in wanted:
            os.remove(os.path.join(args.out, f))
    chunks = buckets

    guard = "RECOMP_FUNCS_%s_H" % (args.module or "").upper()
    write_if_changed(os.path.join(args.out, "funcs.h"),
                     "/* Generated by recomp/lift/lift.py. */\n#ifndef %s\n#define %s\n" % (guard, guard)
                     + "".join("void %s(CPU *c);\n" % fn(f) for f in funcs) + "#endif\n")

    import zlib
    crc = zlib.crc32(img.bytes_at(img.text_start, img.text_end - img.text_start)) & 0xFFFFFFFF
    m = args.module or "main"
    fh = io.StringIO()
    if True:
        fh.write('/* Generated by recomp/lift/lift.py. */\n#include "rt.h"\n#include "funcs.h"\n\n')
        fh.write("static const RtFuncDesc funcs[] = {\n")
        for f in funcs:
            nm = names.get(f) or "sub_%08x" % f
            fh.write("    {0x%08xu, %s, %s},\n" % (f, fn(f), c_string(nm)))
        fh.write("};\n")
        rs = sorted(em.retsites)
        fh.write("static const uint32_t retsites[] = {\n")
        for i in range(0, len(rs), 8):
            fh.write("    " + " ".join("0x%08xu," % r for r in rs[i:i + 8]) + "\n")
        fh.write("    0\n};\n")
        fh.write("\n/* Data words the loader rewrites after mapping the image: {address, value}. */\n")
        fh.write("static const uint32_t patches[][2] = {\n")
        for va, val in patches:
            fh.write("    {0x%08xu, 0x%08xu},\n" % (va, val))
        fh.write("    {0, 0}\n};\n\n")
        fh.write("const RtImageDesc rt_image_%s = {\n" % m)
        fh.write("    .name = %s,\n" % c_string(os.path.basename(args.pe or args.raw).lower()))
        fh.write("    .base = 0x%08xu, .pref_base = 0x%08xu, .size = 0x%xu,\n" % (
            img.base, img.pref_base, len(img.data)))
        fh.write("    .text_base = 0x%08xu, .text_size = 0x%xu, .text_crc = 0x%08xu,\n" % (
            img.text_start, img.text_end - img.text_start, crc))
        fh.write("    .entry = 0x%08xu,\n" % img.entry)
        fh.write("    .funcs = funcs, .nfuncs = %d,\n" % len(funcs))
        fh.write("    .retsites = retsites, .nretsites = %d,\n" % len(rs))
        fh.write("    .patches = patches, .npatches = %d,\n};\n" % len(patches))
        if not args.module:
            # A single-image program: this table is the whole image list.
            fh.write("\nconst RtImageDesc *const rt_images[] = {&rt_image_main};\n")
            fh.write("const unsigned rt_nimages = 1;\n")
    write_if_changed(os.path.join(args.out, "tables.c"), fh.getvalue())
    t2 = time.time()

    kinds = {}
    for k in an.seed_kinds.values():
        kinds[k] = kinds.get(k, 0) + 1
    how = {}
    for _, _, h in an.tables.values():
        how[h] = how.get(h, 0) + 1
    rep = []
    rep.append("functions            %d" % len(funcs))
    rep.append("  by origin          %s" % ", ".join("%s=%d" % kv for kv in sorted(kinds.items())))
    rep.append("unique instructions  %d (%d bytes of %d in the code section)" % (
        len(an.insns), sum(1 for b in an.cover if b), len(an.cover)))
    rep.append("emitted instructions %d (shared tails are duplicated per function)" % em.stats["insns"])
    rep.append("chunks               %d" % len(chunks))
    rep.append("jump tables          %d (%s)" % (len(an.tables), ", ".join("%s=%d" % kv for kv in sorted(how.items()))))
    rep.append("indirect jumps       table=%d import-thunk=%d runtime-dispatch=%d" % (
        em.stats["ijmp_table"], em.stats["ijmp_import"], em.stats["ijmp_dynamic"]))
    for a, text in sorted(set(em.ijmp_dynamic_sites)):
        rep.append("    %08x %s" % (a, text))
    rep.append("indirect calls       %d" % em.stats["icall"])
    rep.append("tail calls           %d" % em.stats["tailcalls"])
    rep.append("int3 reached         %d" % em.stats["int3"])
    rep.append("undecodable targets  %d (decode failures: %d, overlapping decodes: %d)" % (
        em.stats["bad_targets"], len(an.badflow), an.overlaps))
    rep.append("rejected pointers    %d" % an.rejected_ptrs)
    rep.append("scope tables repaired %d" % len(patches))
    for va, val in patches:
        rep.append("    %08x -> %08x" % (va, val))
    for msg in patch_problems:
        rep.append("    UNRESOLVED " + msg)
    trapped = {k: len(v) for k, v in tr.trapped.items()}
    total_trap = sum(trapped.values())
    rep.append("trapped mnemonics    %d sites, %d distinct" % (total_trap, len(tr.trapped)))
    for k, v in sorted(trapped.items(), key=lambda kv: (-kv[1], kv[0])):
        rep.append("    %-16s %d" % (k, v))
    rep.append("time                 analysis %.1fs, emission %.1fs" % (t1 - t0, t2 - t1))
    text = "\n".join(rep) + "\n"
    with open(os.path.join(args.out, "report.txt"), "w") as fh:
        fh.write(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
