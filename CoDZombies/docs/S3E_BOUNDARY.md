# The s3e API boundary

How the game (`s3e_code`, 0x65f00–0x2c45e0) reaches Marmalade's iOS loader
(`__text`, 0x2040–0x5a0c4), and therefore what a recompiled build's runtime
has to provide. Produced by

```
python3 -I tools/s3e_boundary.py > build/s3e_boundary.tsv
```

which writes one row per stub (address, implementation, trampoline, return
width, argument class, kind, subsystem, call sites, strings) and a summary on
stderr.

## Ways in and out

**Into the game.** The loader holds exactly one pointer into `s3e_code`: the
local pointer at `0xa001e4` holds `0x65f00`, the image's start routine. That
routine (ARM) aligns the stack, then calls the constructor at `0x250e39`
(Thumb), the main path at `0x67944` (Thumb) and a final routine at `0x708d8`.
The `XE3U` header at the start of `s3e_data` holds sizes and offsets of the
image, not further entry points. Every other entry into the game is a function
pointer the game hands over at run time (see "Callbacks").

**Out of the game.** All of it goes through 809 stubs at the end of
`s3e_code`, 0x2ba6b0–0x2be5d0, 20 bytes each:

```
stub:     e59fc000  ldr ip, [pc]      ; ip = implementation in __text
          e59ff000  ldr pc, [pc]      ; jump to a trampoline in __text
          .word     impl
          .word     trampoline
          4778 46c0 bx pc ; nop       ; Thumb entry of the following stub
```

Game code never branches to a stub directly. It calls entries of a table of
251 ARM `b` instructions at 0x65f80–0x6636c (`bl` from ARM, `blx` from Thumb),
and each entry branches to one stub. The recursive-descent survey
(`tools/s3e_survey.py`) finds no other branch into the stubs, no `svc` in
reached code, and no stub address stored as data. Apart from the 20 data
exports described below, that table is the game's only way out. The call-site
counts below are calls to table entries.

## Trampolines

The trampoline encodes the calling convention. Each one saves the game's `sp`
in `r4`, switches to the loader's stack and r9 (kept at `0xa0c228`: `sp`,
`r9`, and an alternative `r9` at +8), calls `impl`, switches back and clears
the scratch registers that aren't part of the result.

| trampoline | return | arguments | enter/leave hooks | stubs |
|---|---|---|---|---|
| 0x383ec | void | r0–r3 | `0x201f0(1, impl)` / `0x20098(1, 0)` | 389 |
| 0x38678 | i32 | r0–r3 + up to 8 on the stack | yes | 176 |
| 0x38460 | i32 | r0–r3 | yes | 135 |
| 0x3865c | void | r0–r3 + stack | yes | 57 |
| 0x38100 | i64 (r0:r1) | r0–r3 | no | 25 |
| 0x38690 | i64 | r0–r3 + stack | yes | 4 |
| 0x384d0 | i64 | r0–r3 | yes | 2 |
| 0x380b8 | i32 | r0–r3 | no | 1 |

The stack variants go through `0x38254`, which also checks the current thread
(`0x3934c`): on the main game thread it switches stacks, otherwise it only
sets r9. They copy exactly 8 stacked words (`[r4+0x28]`…`[r4+0x44]`), so no
s3e call takes more than 12 word arguments.

For the recompiled build, the trampoline gives each import its return width
and whether it takes stack arguments, which is enough to generate the
lifted-code side of every import. Exact argument counts need the s3e headers
or a look at each implementation.

## Callbacks

The loader calls into the game through one routine, `0x37da0`, which has 59
call sites in `__text`:

```
call_game(fn, a0, a1, a2, a3, a4, a5, a6)  ->  fn(a0, ..., a6)
```

That's four register arguments and three on the stack, with r0:r1 returned.
If the loader is on its own stack (`0x393a4` != 0), it switches to the game's
saved stack, sets r4–r11 to zero (r9 included) and `bx`s to `fn`, so
interworking picks ARM or Thumb from bit 0. Otherwise it calls `fn` on the
current stack. Callers by subsystem:

| subsystem | call sites | what calls back |
|---|---|---|
| s3eFile | 46 | user file systems: the game registers one for its `.dz` derbh archives, so file operations on those paths run game code |
| s3eCallbacks | 5 | the device callback registry (`Invalid callback ID`, `callback %p is already registered on device %d`) used by keyboard, pointer, device and sound events |
| s3eCompression | 3 | user decompression callbacks |
| s3eMemory | 3 | user allocator hooks |
| s3eTimer | 1 | `s3eTimerSetTimer` callbacks; the timer table is 32 × 16 bytes at `0xa0c4d0` (due time, function, user data), and the dispatcher at 0x3a9a0 calls `fn(0, userdata)` |

For the recompiled build, a callback is a guest address that has to be looked
up in the lifted code's function table (ARM or Thumb by bit 0) and called with
up to 7 arguments.

## r9

The game uses r9 as an ordinary callee-saved register: it's pushed and popped
with r4–r11 and used for arithmetic, with 12.7k instructions touching it and
6k writing it. It isn't a static base. The loader zeroes it on every callback
and restores its own r9 in every trampoline. A lifter can treat r9 like
r4–r8.

## Kinds of stub

| kind | stubs | distinct targets | what |
|---|---|---|---|
| import | 446 | 223 | `impl` is `b <__symbol_stub4>`: **OpenGL ES 1.1 and 2.0**, passed straight to the OpenGLES framework. Named from the indirect symbol table. |
| call | 343 | 305 | Marmalade's s3e API, implemented in `__text` |
| data | 20 | 20 | not called; the game reads the word. The image base (0x65f00), the `s3e_data` base (0xa0e000), and 18 loader pointers (`INDIRECT_SYMBOL_LOCAL` slots 0xa00a94–0xa00ad8) to trampoline entry points |

The 18 trampoline entry points come in two families of nine (void/i32/i64 ×
register, register without hooks, stack), at 0x38068–0x3821c and
0x383e8–0x3868c. They differ in the first argument to the enter hook (0 or 1).
The static stubs use the second family. The game's extension code
(`s3eExtFacebook`, `s3eExtIPhone`, `s3eExtZeroConf`, …) resolves extension
functions at run time and calls them through these.

261 of the 809 stubs repeat an implementation already stubbed earlier in the
table, mostly the GL block, which appears twice. The game calls one of them,
once. In practice the surface is the first 548.

## What the game actually calls

Call sites are direct `bl`/`b`/`blx` in ARM and Thumb code plus the branch
table. 249 of the 548 distinct targets have at least one call site, with 1916
call sites in all.

| subsystem | distinct | called | call sites |
|---|---|---|---|
| OpenGL ES | 223 | 98 | 538 |
| s3eFile | 28 | 16 | 290 |
| s3eDebug | 21 | 13 | 178 |
| s3eSocket | 22 | 22 | 172 |
| s3eTimer | 14 | 5 | 157 |
| s3eConfig | 4 | 2 | 138 |
| s3eDevice / s3eError | 47 | 6 | 72 |
| s3eSound | 41 | 10 | 62 |
| s3eMemory | 20 | 13 | 51 |
| s3eSecureStorage? | 5 | 5 | 50 |
| s3eSurface | 6 | 6 | 37 |
| s3eGL / EGL (iOS layer) | 31 | 13 | 32 |
| s3eKeyboard | 7 | 6 | 30 |
| unknown (0x45b04–0x47e40) | 6 | 6 | 28 |
| s3eGL? | 5 | 3 | 18 |
| s3eAccelerometer | 8 | 8 | 18 |
| s3eAudio | 7 | 5 | 18 |
| s3eCallbacks | 15 | 5 | 11 |
| s3eCompression | 4 | 4 | 8 |
| s3ePointer | 1 | 1 | 4 |
| s3eLicense | 3 | 1 | 2 |
| s3eVideo | 3 | 1 | 2 |
| s3eCrypto | 6 | 0 | 0 |
| s3eExt | 1 | 0 | 0 |

Subsystem names come from address ranges in `__text`. Marmalade's sources are
linked alphabetically. The `Disable<Subsystem>` config keys list every one:
Accelerometer, Audio, Callbacks, Camera, Compression, Config, Crypto, Debug,
Device, Error, Ext, Fibre, File, GL, Keyboard, License, Memory, Pointer,
SecureStorage, Socket, Sound, Surface, Timer, Vibra, Video. Each range is
anchored by the strings its functions use, such as `Invalid s3eFile handle %p`, `Invalid s3eSocket handle %p`,
`Invalid channel: %d` (sound) and `Maximum number of active timers reached`.
The boundaries between anchors, and names marked `?`, are guesses. The
0x12000–0x16f00 block between Audio and Callbacks is a bignum library
(`bint_*`, axTLS style) with no API entries. Debug starts at 0x1dea8,
right after Crypto's SHA-1 constants. Its two most-called entries, 0x1dea8
(19 calls) and 0x1deb0 (69 calls), are `return 0`, so some debug feature is
compiled out. The binary
has no symbols, codwawzpc (a touchHLE fork) emulates the iOS layer and never
names s3e functions, and no Marmalade SDK was found on disk. Per-function
names need the SDK's `s3e*.h`.

## What this means for the recompile

- Lifting only `s3e_code` is viable. The game's whole outside world is about
  150 s3e functions plus 98 GLES functions. GLES maps 1:1 to the OpenGLES
  framework, which arm64 iOS still ships. Each s3e function is a small native
  arm64 implementation that converts 32-bit guest pointers. The loader's
  `__text` and its 32-bit Objective-C wouldn't be lifted at all.
- Each stub becomes one generated import thunk: the stub address (and its
  Thumb entry at −4, and its branch-table slot) maps to a host function. The
  argument class comes from the trampoline.
- GL calls need pointer arguments translated from guest to host addresses,
  so `glVertexPointer` and friends work against the fixed-base guest mapping.
  Client-side vertex arrays read at draw time can be handled the same way
  Bridge32 does.

## Open

- Per-function names and signatures for the ~150 called s3e functions.
- Which callback signatures each registering API uses: how many of the seven
  arguments are real, and what's returned.
