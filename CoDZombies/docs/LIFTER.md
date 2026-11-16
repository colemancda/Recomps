# The lifter

`lift/` turns the game code in `s3e_code` (ARMv6 A32, Thumb-1 and VFPv2) into
C that compiles for arm64 iOS. Only the game is lifted. Marmalade's loader
(`__text`) is replaced by a native runtime behind the 809 import stubs; see
`S3E_BOUNDARY.md`.

```
tools/extract.sh                 # orig/ from the decrypted IPA
./build.sh ios                   # survey + lift if needed, then compile -> build/obj-ios/
./build.sh host --relift         # the same for this Mac, re-lifting first
```

## Pieces

| file | what |
|---|---|
| `tools/s3e_survey.py` | function starts by recursive descent (`build/s3e_funcs.tsv`) |
| `lift/analyze.py` | function bodies: everything reachable from the entry without a call; ARM jump tables; call targets become new starts |
| `lift/translate.py` | one instruction → C statements, written against `runtime/cpu.h` |
| `lift/lift.py` | emits `build/gen/chunk_NNN.c`, `funcs.h`, `tables.c` (address → function) and `report.txt` |
| `runtime/cpu.h` | guest CPU state, memory access, flag and shifter helpers, the runtime interface |
| `test/insn_test.py` | single instructions against Unicorn |
| `test/func_test.py` | whole leaf functions against Unicorn |

## Model

- **State.** `CPU` holds r0–r15, N Z C V Q, GE, the VFP register file (`s[32]`
  overlapping `d[16]`), the FPSCR flags and the exclusive monitor. Flags are
  computed eagerly; there's no flag liveness yet.
- **Memory.** Guest address `a` is host `RT_GUEST_BASE + a` (4 GB at a fixed
  base, or `g_mem` with `RT_DYNAMIC_BASE`). Loads and stores are `memcpy`, so
  unaligned word accesses work as they do on iOS. The mapping needs one spare
  page past 4 GB for accesses that wrap.
- **Functions.** `void f_XXXXXXXX(CPU *c)`. A `bl` sets lr, calls, then checks
  that the callee returned to the expected address (`c->r[15]`, set by every
  return), otherwise `rt_ret_mismatch`. Returns are `bx lr`, `mov pc, lr`,
  `pop {…, pc}`, `ldm sp!, {…, pc}` and `ldr pc, [sp], #4`. A branch or
  fall-through into another function is a tail call. Functions shared between
  bodies are duplicated, not split.
- **Imports.** A call to a branch-table entry, a stub or a stub's Thumb entry
  becomes `rt_import(c, stub)`. The runtime runs the s3e or GL function for
  that stub and sets `c->r[15]` to lr, as the loader's trampolines do.
- **Indirect control flow.** `blx reg`, and `mov lr, pc` followed by a write to
  pc, are calls through `rt_call`, which looks the address up in the function
  table (bit 0 is the Thumb bit). ARM jump tables
  (`ldr pc, [pc, rX, lsl #2]`, `add pc, pc, rX, lsl #2`) become a `switch` on
  the target, with `rt_jmp` as the default. Any other write to pc is `rt_jmp`,
  a tail call through the table.
- **VFP.** Single and double precision as C `float`/`double`. Compile with
  `-ffp-contract=off`, because `vmla`/`vmls` round the product before the add.
  Float-to-int conversions saturate and map NaN to 0. Vector mode (FPSCR LEN)
  and flush-to-zero aren't modelled.
- **Exclusives.** `ldrex` records the address; `strex` succeeds if it matches
  (`rt_strex`). That's enough for one thread, but not atomic across threads yet.

## Status

`python3 -I lift/lift.py`, 2026-11-15:

| | |
|---|---|
| functions | 15478 (13057 ARM, 2421 Thumb) |
| instructions lifted | 575982 (counting code shared by several functions once per function) |
| untranslated | 0 |
| calls | 35607 direct, 4501 indirect, 2202 imports, 44 tail calls to imports |
| other | 15222 returns, 3783 tail calls, 201 jump tables, 1694 indirect jumps |
| branch targets outside known code | 37 (`rt_bad_code`) |
| output | 48 chunks, 75 MB of C, 19 s to lift |
| arm64 iOS build | `./build.sh ios`: 49 objects, 16 MB, 20 s on this Mac, no errors |

Both tests use Unicorn with an ARM1176 CPU (armv6 + VFPv2), the core the
original iPhones had.

- `test/insn_test.py --per-shape 4 --states 25` samples 2904 game instructions
  in 953 shapes (mnemonic, operand kinds, shift, condition, S, writeback, mode),
  306 of them VFP. 72451 runs pass and 0 fail. 149 are skipped where the
  emulator faults, mostly unaligned `ldrd`/`ldm`.
- `test/func_test.py --count 400 --states 6` picks 400 of the 4586 leaf
  functions (7387 instructions, 53 Thumb). 2212 runs pass and 0 fail. 188 are
  skipped for emulator faults or for running past 200000 instructions.

What the tests found and fixed along the way:

- capstone reports `update_flags` for `adc`/`sbc`/`rsc` without S, so ARM
  takes S from bit 20 of the encoding.
- ARM shift aliases (`lsl r0, r3, #29`) carry the shift on the source operand.
- Thumb `movs rd, rm` has its own instruction ID.
- Conditional VFP mnemonics carry the condition (`vaddeq.f32`).
- `fldmiax`/`fstmiax` transfer one extra word.
- Thumb-2, `cbz`, `bxns`, armv6 media instructions and `ldm`/`stm … ^` never
  occur in the game's code. Where a walk reaches one, it has run into data.

### Running the tests

Unicorn is built from Bridge32's vendored copy (ARM only):

```
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) cmake -S ~/Developer/Bridge32/external/unicorn -B /tmp/unicorn-arm -G Ninja \
    -DUNICORN_ARCH=arm -DUNICORN_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_SYSROOT=$SDKROOT -DCMAKE_C_COMPILER=$(xcrun -f clang)
ninja -C /tmp/unicorn-arm
export LIBUNICORN_PATH=/tmp/unicorn-arm PYTHONPATH=~/Developer/Bridge32/external/unicorn/bindings/python
python3 test/insn_test.py
python3 test/func_test.py
```

## Next

- **Runtime.** Reserve and map guest memory, copy in `s3e_code`, `s3e_data`
  and the loader data the game reads, and build the function table from
  `g_lifted`. Then implement `rt_import` for the ~150 s3e functions and 98 GL
  functions the game calls (GL with guest→host pointer translation), plus
  callbacks into the game through `g_lifted`, threads, and `rt_strex`.
- **Return mismatches.** Find out whether anything returns somewhere other than
  its caller (setjmp/longjmp, Thumb `__gnu_thumb1_case_*` switch helpers),
  and handle it.
- **The 37 bad branch targets** and the ~100 KB of ARM-looking code the survey
  doesn't reach. Indirect jumps there need a fallback.
- **Speed.** Flag liveness, constant literal-pool loads (`s3e_code` is
  read-only), and local copies of registers inside a function.
