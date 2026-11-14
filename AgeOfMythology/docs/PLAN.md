# Plan: a static recompilation of Age of Mythology (Gold Edition)

Goal: run Age of Mythology and The Titans on Linux (x86_64 first, then aarch64
on the Retroid Pocket 5) by lifting the original 32-bit x86 code to C and
linking it with a runtime that stands in for Windows. Same method as
`../GunBound`, whose `README.md` describes the lifter and runtime this builds
on. Read it first.

Nothing derived from the game is committed. `orig/` (the installed game) and
`build/` are ignored.

## The input

`orig/` is the installed game, extracted from
`Age of Mythology Gold Edition/Age of Mythology.exe`. That file is a Smart
Install Maker 5.02 installer (data in a proprietary compressed overlay at file
offset 0x1ea00; 7-Zip and innoextract cannot open it). It was unpacked by
running it silently under Wine in a throwaway prefix with the null graphics
driver:

```
WINEPREFIX=$tmp wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
WINEPREFIX=$tmp wine 'Age of Mythology.exe' /s /p="C:\aom"
```

The result is 4604 files, 1.3 GB. `aom.exe` and `aomx.exe` are byte-identical
to the copy installed by Lutris in `~/Games/age-of-mythology`.

| file | SHA-1 | version | built | .text |
|---|---|---|---|---|
| `aom.exe` | `7cd7f5390ff11093e2981365e126b1e3dc5af825` | 3.2004.4.2 (AoM 1.10) | 2004-04-23 | 5.3 MiB |
| `aomx.exe` | `17688dfd27fc2017e736b4f97cbee1e29b630763` | 4.2004.1.5 (Titans 1.03) | 2004-01-05 | 5.6 MiB |

**Target `aomx.exe` first.** It is the same engine with more content, and it
is what people play. `aom.exe` should then cost little: the lifter is generic
and the runtime is the same.

## What the executable is (facts from the survey)

- **Not packed or encrypted.** The sections are `.text .rdata .data .data1
  .rsrc` with a normal import table. There is no SafeDisc or SecuROM section,
  and `drvmgt.dll` in the install is unused leftover. Whether a CD check
  remains in code has not been verified yet (step 0).
- **Toolchain:** MSVC 6 (Rich header: VC6 SP5 compiler, linker 6.0) with
  dynamic CRT: `MSVCRT.dll` (120 imports) and `MSVCP60.dll`. Both DLLs are
  shipped in `orig/`.
- **Source file names survive** in assertion strings: 146
  `d:\rts3 patch\*.cpp` paths. MSVC RTTI in `.rdata` gives class names for
  the vtables. These two sources, plus the XS function registration table
  (script function names with signatures and help strings), are how functions
  get names. There is no hand decompilation to lean on, unlike GunBound.
- **About 4.3 times GunBound's code:** 5.6 MiB of `.text` against 1.3 MiB.
- **Graphics: Direct3D 8**, loaded by name (`d3d8.dll`, `Direct3DCreate8`).
  It uses hardware transform and lighting: untransformed vertices, matrices,
  lighting, fog, several texture stages and render-state tables. There are no
  vertex or pixel shader strings. D3DX 8 is linked in statically, with
  processor-specific pipelines (`DisableD3DXPSGP`). Textures are the game's
  own `.ddt` (`BD3DTexture::loadddt`), decompressed by the game into D3D
  surfaces, probably DXT.
- **Sound: Miles Sound System 6.1c** (`mss32.dll`, used through RIB
  interfaces) with the `mp3dec.asi` provider. Miles in turn uses
  `dsound.dll` or winmm `waveOut`.
- **Other DLLs the game loads:**
  - `granny.dll` (RAD Granny: model and animation; 29 imports, CPU skinning)
  - `rockalldll.dll` (Rockall heap: `FAST_HEAP`)
  - `binkw32.dll` 1.5J (videos in `avi/`)
  - `EBUEula.dll` (EULA dialog)
  - `ESOClientDLL.dll` and `zoneaccessapi.dll` (the defunct online service)
  - `dpnhpast.dll` (DirectPlay NAT helper; `CLSID_DirectPlay8Address`)
- **Networking:** winsock with `listen`/`accept`/`WSAAsyncSelect`, so LAN and
  direct-IP play goes over the game's own sockets.
- **Threads:** created with `_beginthreadex`. There are also
  `SuspendThread`, `ResumeThread`, `QueueUserAPC` and `TerminateThread`.
- **SIMD:** a linear sweep finds SSE (about 2300 instructions, mostly packed:
  `mulps`, `shufps`, `movaps`), 3DNow! (about 2700: `pfmul`, `pfadd`,
  `femms`), MMX (about 3700) and about 55,000 x87. The SSE and 3DNow! code
  sits in CPU-dispatched paths (D3DX PSGP, the vector math). A CPU that
  reports neither should get the x87 paths, the same trick GunBound uses for
  SSE2. That must be confirmed (step 0). MMX may not be dispatched.
- **Game logic in scripts:** AI and random maps are XS scripts (`ai/*.xs`,
  random-map scripts) compiled and interpreted by the game itself. That is
  ordinary lifted code; nothing special is needed.
- **Data:** `.bar` archives (`data.bar`, `textures.bar`, `models.bar`,
  `sounds.bar` and their Titans `*2.bar` counterparts) plus loose files.
  File access is the game's own.

## Differences from GunBound that drive the design

1. **Several images, not one.** GunBound's lifter and loader handle one EXE.
   Here the loader must handle DLLs: relocation to a chosen base, export
   tables, `DllMain`, `GetProcAddress` into lifted DLLs, and per-module TLS.
   Each DLL either has a lifted body or is replaced by host code
   ("high-level emulation", HLE):

   | module | treatment | why |
   |---|---|---|
   | `msvcrt.dll`, `msvcp60.dll` | lift (shipped in `orig/`), HLE for hot routines (`memcpy`, `memset`, `strlen`, `_ftol`) | exact semantics of `sprintf` with x87 values, `_except_handler3`, `__CxxFrameHandler`, `_CxxThrowException`, locale, streams; GunBound's SEH and non-local-return work applies unchanged |
   | `granny.dll` | lift | no public source for this version; pure computation over 62 kernel32 imports |
   | `mss32.dll` + `mp3dec.asi` | lift, with the runtime providing `waveOut` or DirectSound under it | the RIB interface is wide (351 exports) and loosely documented; lifting it is cheaper than reimplementing it. GunBound's DirectSound mixer is reused |
   | `rockalldll.dll` | lift first; HLE onto `malloc` later if profiling says so | 82 KB, C++ objects with vtables that the game calls into |
   | `binkw32.dll` | stub (videos skipped) at first, lift later | intro videos are optional |
   | `EBUEula.dll`, `ESOClientDLL.dll`, `zoneaccessapi.dll`, `EsInet.dll`, `dpnhpast.dll` | HLE stubs: accept the EULA, fail online login, no NAT helper | the services are gone |
   | `d3d8.dll`, `dsound.dll` | runtime COM objects | as in GunBound |

2. **Direct3D 8 needs the GPU.** GunBound's software rasteriser handles
   pre-transformed triangles. AoM draws a 3D world with lighting, fog,
   multitexture and many thousands of triangles per frame; a software path
   would be far too slow on the handheld. The D3D8 layer is therefore built on
   OpenGL ES 3 from the start, with fixed-function emulation by generated
   shaders keyed on the render and texture-stage state. That is the design in
   `../GunBound/docs/PLAN_GLES_RENDERER.md`, which is simpler here:
   - AoM is a pure D3D8 game, with no CPU writes interleaved into the back
     buffer.
   - Vertex and index buffers map to GL buffers.
   - DXT textures need `EXT_texture_compression_s3tc`, with a CPU
     decompressor as the fallback.

   Recommendation: implement this renderer in a shared place so that
   GunBound's GLES plan gets it as well (see Repository layout).
3. **Determinism matters.** AoM multiplayer is lockstep and the game detects
   out-of-sync play by checksumming its state. Recorded games (`.rec`) replay
   the commands and rely on the same determinism. Two consequences:
   - **Recorded games are the best test oracle available.** A game recorded
     on Windows or Wine and replayed by the recompilation must finish without
     going out of sync. That checks the simulation bit for bit over thousands
     of turns.
   - **The x87 model must become exact where the simulation needs it.**
     GunBound treats x87 values as doubles, applies 24-bit precision only to
     arithmetic results, and uses host libm for transcendentals.
     `IDirect3D8::CreateDevice` without `D3DCREATE_FPU_PRESERVE` sets the FPU
     to 24-bit precision, which the game then runs in. The replay test shows
     whether the current model is enough; if not, implement precision control
     and rounding faithfully, and bit-exact `fsin`/`fcos`/`fpatan`/`fsqrt`
     from x87 reference tables.
4. **SIMD.** Add MMX and SSE1 to `translate.py`. Both are a direct mapping:
   MMX onto `uint64_t` lanes, SSE onto four `float`s with
   round-to-nearest-even. Do it even if `cpuid` steers most code away,
   because undispatched MMX in the game would otherwise trap. 3DNow! stays
   trapped, and `cpuid` must not report it.
5. **Scale.** The lifter should split output into more chunks, and the build
   should compile in parallel with ninja (already the case). Expect about four
   times GunBound's lift and compile time: on the order of a minute to lift
   and five to compile on the 4-core machine.

## Repository layout

```
Recomps/
  GunBound/           existing, working (milestone 4)
  AgeOfMythology/
    orig/             installed game (ignored)
    lift/ runtime/ test/ tools/ docs/ build.sh run.sh
```

Recommendation: start AgeOfMythology with a copy of GunBound's `lift/` and
`runtime/`, keeping file and function names. Once AoM boots (milestone 2),
extract the parts that are the same in both into `Recomps/common/`: the
lifter, `cpu.h`, `mem`, `dispatch`, `thread`, `seh`, `file`, `registry`,
`winsock`, `user32`, `video`, `pad`, `dsound`, and the GL renderer. Doing it
before then would mean refactoring GunBound's working build around guesses.

## Steps and milestones

### 0. Survey (no runtime work yet)

- **Function list.** Run Ghidra headless auto-analysis on `aomx.exe` and the
  DLLs to be lifted, and export `addr,size,name`. This is the `PROGRESS.csv`
  equivalent and seeds the lifter's function starts. Add names from RTTI,
  assertion source paths and the XS registration table.
- **CPU detection.** Find the `cpuid` sites and what each feature bit
  selects. List every SSE, 3DNow! and MMX block reachable when `cpuid`
  reports a plain Pentium Pro (CMOV, no MMX/SSE/3DNow!). Expected result:
  only MMX, if anything.
- **CD check.** Find any `GetDriveTypeA`, volume-label or `.bar`-on-disc
  check. None is in the import table, but `LoadLibrary` and `GetProcAddress`
  could hide one.
- **Registry and command line.** List the registry keys and values read at
  start-up (`HKLM\Software\Microsoft\Microsoft Games\Age of Mythology...`)
  and the command-line switches.
- **Reference trace.** Record a reference trace under Wine: run the game
  with `WINEDEBUG=+relay,+d3d` (or an API-logging shim) from launch to the
  main menu and through a short skirmish. This gives the import and COM call
  sequence the runtime must reproduce, and the D3D8 state actually used.
- **Reference recordings.** Record two or three skirmish games against the
  AI under Wine (`.rec` files) to serve as the determinism tests later.

### 1. Lifter generalisation

- **Multi-image support.** The lifter takes the EXE plus a list of DLLs and
  emits one `f_<module>_<addr>` namespace per image. DLLs are lifted at their
  preferred base, and the loader places them there. Guest address-space
  layout: EXE at 0x400000; DLLs at their own bases (msvcrt 0x77c10000-style
  bases sit above GunBound's 1 GiB guest limit, so either rebase them all
  below 1 GiB at lift time using `.reloc`, or reserve the full 4 GiB of guest
  space on 64-bit hosts. Recommend the 4 GiB reservation: no rebasing, and
  pointer values in memory match Windows.)
- **New instructions.** MMX and SSE1 in `translate.py`, each with test
  snippets in `test/snippets.S` checked against a real CPU as today.
- **Larger output.** Chunking and the report scaled up; `report.txt` gives
  coverage per image.

Done when: everything lifts with zero decode failures, the instruction tests
pass, and the trapped-instruction list matches step 0's expectation.

### 2. Boot to the main menu loop, headless (null renderer)

- **Loader.** Maps the lifted DLLs, runs `DllMain` (and the CRT init in
  msvcrt), resolves imports between lifted images directly, and sends the
  rest to host handlers.
- **Win32 handlers.** Whatever the trace from step 0 shows is missing. The
  additions over GunBound will be mostly:
  - advapi32 security calls (stubs)
  - `GetGlyphOutlineA`, the font rasterisation the game uses for text (real
    implementation needed: FreeType with a bundled font, or the game's own
    font files if it has them)
  - `CreateDIBSection`
  - the clipboard
  - `QueueUserAPC`, `SuspendThread`, `ResumeThread`
- **D3D8 null device.** Every method present, logging its arguments and
  returning success. Enough for the game to believe it has a device.
- **Stub DLLs.** The HLE stubs from the table above, accepting the EULA.

Done when the game, run headless on the virtual clock, reaches its main menu
loop: the trace shows the menu's per-frame D3D call pattern, matching the
Wine trace.

### 3. Direct3D 8 on OpenGL ES 3: the menu, then a map

- **Objects.** Device, swap chain, surfaces, textures (including DXT),
  vertex and index buffers, and the state blocks the game uses. Present goes
  through SDL as in GunBound, with the same scaling and full-screen options.
- **Fixed-function emulation.** Generated GLSL from a key over:
  - the FVF
  - lighting with up to 8 lights
  - material
  - fog (vertex and table)
  - texture stages 0-3: colour/alpha op, arguments, texcoord index and
    transform
  - alpha test, blend, depth, stencil and cull

  Programs are cached by key.
- **Testing.** Frame dumps are compared against Wine's rendering of the same
  scripted input (Wine's D3D8 is itself on GL, so differences point at this
  layer) by perceptual difference, not byte equality.
- **Software reference.** Mesa llvmpipe in CI-like runs, so tests need no GPU.

Done when the main menu is right and a skirmish map renders: terrain, water,
units animated by Granny, the UI and the minimap.

### 4. Sound and video

- **Sound.** Miles (lifted) on top of DirectSound or `waveOut` from the
  runtime. GunBound's mixer is the starting point. Check whether Miles asks
  for 3D buffers or EAX; refuse both and let it fall back to its software
  mixer.
- **Video.** Bink: lift `binkw32.dll`, or keep skipping videos.

Done when music and effects are measured continuous in a `--dump-audio` run,
and the intro video plays or is cleanly skipped.

### 5. Play a skirmish; determinism

- **Input.** Mouse, keyboard and hotkeys through window messages and
  `GetAsyncKeyState`. There is no DirectInput import here, which makes this
  simpler than GunBound.
- **Replays.** Play the reference `.rec` files from step 0 to the end with no
  out-of-sync. If they desync, fix the x87 model (see Differences, point 3)
  until they don't.
- **AI.** A scripted skirmish against the AI runs to a victory or defeat
  screen. This exercises XS, pathing and combat.

Done when every reference recording replays in sync on x86_64 and on aarch64
(under qemu), and the two builds produce identical frames on the virtual
clock (as `test/frames.sh` does for GunBound).

### 6. LAN multiplayer

- **Interoperability.** Two recompiled instances, then one recompiled
  instance against one under Wine, over LAN or direct IP, using GunBound's
  `--local-address` for several instances on one machine.
- **Sync.** A full game with no desync. Against Wine this is the strictest
  test of the x87 work.

### 7. The handheld

- **Package.** aarch64 build and ROCKNIX package, as for GunBound.
- **Controls.** Touch as the primary pointer (tap = select, drag = box
  select, two fingers = pan/zoom). The sticks pan the camera and move the
  pointer, and the buttons map to the common hotkeys. This needs a design
  pass: an RTS on a gamepad is a UI problem, not a runtime one.
- **Speed.** Profile and optimise. AoM's simulation and Granny skinning are
  CPU-heavy. Expect to need what GunBound postponed: keeping guest registers
  in host locals within a function, flag liveness across calls, and HLE for
  the hottest CRT and math routines. The renderer's cost is already on the
  GPU.

## Risks, in order

1. **x87 exactness for lockstep.** If the replays desync, bit-exact x87
   (80-bit internal precision, precision control, transcendentals) is a
   substantial piece of work. It is required for multiplayer with Windows or
   Wine clients and for replays. Single-player works without it.
2. **Performance on the handheld.** The current lifter keeps every register
   in memory. GunBound needs 45% of a core on the device; AoM's simulation is
   far heavier. Register promotion is probably required, not optional.
3. **D3D8 fixed-function fidelity.** Lighting, fog and texture-stage
   combinations must match closely enough to look right. Wine's
   implementation is the practical reference to compare against.
4. **Miles and Granny lifted rather than reimplemented** depend on these
   DLLs having no self-modifying or run-time-generated code. Miles 6 does
   build mixer code at run time on some versions; step 0 should check
   `VirtualProtect` and executable allocations. If it does, select its plain
   C mixer path or reimplement the RIB subset used.
5. **Indirect control flow at scale.** About 4 times GunBound's code means
   more jump tables and computed calls. The interpreter fallback GunBound
   lists under "Next" (code reached only through computed addresses) may
   become necessary.

## First session checklist

1. Ghidra headless function export for `aomx.exe`, `msvcrt.dll`,
   `msvcp60.dll`, `granny.dll`, `mss32.dll`, `mp3dec.asi`, `rockalldll.dll`.
2. `cpuid` site survey and the CD-check search.
3. Wine reference trace to the main menu, plus two recorded skirmishes.
4. Copy GunBound's `lift/` and `runtime/` here, then add DLL loading and the
   4 GiB guest space.
