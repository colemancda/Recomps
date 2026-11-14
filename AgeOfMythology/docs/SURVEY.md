# Step 0 survey: `aomx.exe` (Titans 1.03)

Findings for step 0 of `PLAN.md`. Addresses are virtual addresses in
`aomx.exe` (image base 0x400000). Tools: capstone and pefile scripts, Ghidra
12.1.2 headless (`tools/ghidra/ExportFunctionsCsv.java`), and the lifter
copied from `../GunBound/lift`.

## Baseline lift (no function list)

`python3 lift/lift.py --pe orig/aomx.exe --out build/gen-nocsv`, using
GunBound's lifter unchanged, with function starts taken only from the entry
point, call targets and code pointers found in data and immediates:

| | |
|---|---|
| functions | 38974 (call=13519, data-code=17565, data-label=1065, imm-code=6786, imm-label=38) |
| code section bytes covered | 5631149 of 5855095 (96.2%) |
| unique instructions | 1800225 (GunBound: 361391) |
| decode failures | 0 (73 overlapping decodes) |
| jump tables | 404, all sized (bound=175, bound2=141, scan=88) |
| indirect jumps | table=446, import-thunk=3819, runtime-dispatch=235 |
| indirect calls | 20347 |
| trapped instructions | 8207 sites, 64 mnemonics: MMX, SSE1, 3DNow! (none SSE2) |
| output | 587 chunks, 156 MB of C |
| time | 104 s analysis + 15 s emission (GunBound: 14 s in all) |

The 235 run-time-dispatched jumps are of two kinds, both handled by the
existing dispatcher:

- Virtual tail calls (`jmp [reg+off]`).
- The Microsoft Layer for Unicode thunks: `jmp [slot]` through `.data` slots
  around 0xa99fc8-0xa9a0cc, starting at 0x4cd21f. See below.

The emitted C is about four times GunBound's: 1.9 M instructions in 156 MB.
Compile time at GunBound's rate (about a minute for 0.37 M instructions)
extrapolates to roughly five minutes on 4 cores. Lift time needs work:
`analyze.py` is single-threaded Python, so caching the analysis per input
hash is the obvious fix.

## CPU feature detection

Nine `cpuid` instructions in two routines, both in the statically linked
library code near the end of `.text`:

- **0x94eb00 region.** Extended leaf 0x80000001 edx bit 31 (3DNow!), then
  leaf 1 edx bit 25 (SSE) and bit 26 (SSE2), each confirmed under an SEH
  frame (executing an instruction to check OS support). It also calls
  `IsProcessorFeaturePresent` (the only call site, 0x94ec8d).
- **0x94efc0 region.** Leaf 1 edx bit 23 (MMX), extended leaf presence,
  0x80000001 edx bit 31 (3DNow!) and bit 30 (extended 3DNow!), and a vendor
  string compare (`repe cmpsb` with "AuthenticAMD").

So a CPU that reports
- leaf 0 max = 1,
- leaf 1 edx without MMX/SSE/SSE2,
- max extended leaf = 0x80000000,

and `IsProcessorFeaturePresent` returning FALSE should select the x87 paths
everywhere these routines decide. Not yet known: whether any MMX/SSE code
runs without going through them (the 1938 `movq` sites are the candidates).
That is settled at run time by the trap report, not statically.

## Hidden imports: unicows (MSLU)

The game links `unicows.lib`. At 0x4cd940 it does
`LoadLibraryA("unicows.dll")` (the name is at 0x99b908) and resolves each
wide API lazily into a `.data` slot. On NT, MSLU's loader uses the system
DLL's own export instead. About 90 W functions are reached this way and do
**not** appear in the import table, among them:

- **windows and messages:** `CreateWindowExW`, `DefWindowProcW`,
  `PeekMessageW`, `GetMessageW`, `DispatchMessageW`, `SendMessageW`,
  `PostMessageW`, `RegisterClassW`, `SetWindowLongW`
- **dialogs:** `DialogBoxParamW`, `CreateDialogParamW`,
  `DialogBoxIndirectParamW`, `IsDialogMessageW`, `SetDlgItemTextW`,
  `GetDlgItemTextW`, `SendDlgItemMessageW`, `MessageBoxW`
- **GDI text:** `CreateFontW`, `CreateFontIndirectW`, `GetGlyphOutlineW`,
  `GetTextMetricsW`, `GetTextExtentPoint32W`, `ExtTextOutW`, `TextOutW`.
  The game renders its fonts through GDI, so the runtime needs a real glyph
  rasteriser (FreeType plus a bundled TrueType font).
- **files:** `CreateFileMappingW`, `CreateFileW`, `FindFirstFileW`,
  `FindNextFileW`, `GetFileAttributesW`, `DeleteFileW`, `MoveFileW`,
  `CopyFileW`, `CreateDirectoryW`, `RemoveDirectoryW`,
  `GetCurrentDirectoryW`, `SetCurrentDirectoryW`, `GetTempPathW`,
  `GetTempFileNameW`, `GetLogicalDriveStringsW`
- **registry:** `RegOpenKeyExW`, `RegQueryValueExW`, `RegSetValueExW`,
  `RegCreateKeyExW`
- **modules:** `LoadLibraryW` (this is how `d3d8.dll` and `dsound.dll` are
  loaded; their names are UTF-16 strings), `GetModuleFileNameW`,
  `GetModuleHandleW`, `FindResourceW`, `LoadStringW`, `LoadCursorW`,
  `LoadIconW`
- **other:** `GetFileVersionInfoW`, `VerQueryValueW`, `SHGetFolderPathW`,
  `DirectSoundEnumerateW`, `ImmGetCompositionStringW`, `CreateProcessW`,
  `GetComputerNameW`, `GetVersionExW`, `GetLocaleInfoW`, `GetDateFormatW`,
  `GetTimeFormatW`, `CompareStringW`, `CharLowerW`, `CharUpperW`

Consequence for the runtime: `GetProcAddress` on `kernel32`, `user32`,
`gdi32`, `advapi32`, `shell32`, `version` and `imm32` must serve these
names. The runtime's string handling must be UTF-16 throughout, where
GunBound's was Latin-1.

Other `LoadLibraryA` sites:
- `advapi32.dll` (0x4cdab2)
- `imm32.dll` (0x676b70)
- `ESOClientX.dll` (0x7bdcc2)
- `WIProxy` (0x95a29e, in the library code)
- one name built at run time (0x6782b5)

## Registry, configuration, copy protection

- **Registry keys:**
  `Software\Microsoft\Microsoft Games\Age of Mythology Expansion Pack\1.0`
  (Titans) and `...\Age of Mythology\1.0` (base game). The installer writes
  nothing there; the Lutris install shows the game itself writes only
  `SystemInitialization="1"`. **No registry provisioning is needed to
  start.**
- **Configuration files:**
  - `startup\gamex.cfg`, `startup\production.cfg`, `profile.cfg`
  - `startup\*.con` (console scripts)
  - `gfxconfig2\reference.gfx` and the other graphics presets
- **Copy protection:** no CD or disc messages in `aomx.exe`,
  `language.dll` or `xpacklanguage.dll`, and no `GetDriveTypeA` or
  `GetVolumeInformationA` import. `GetLogicalDriveStringsW` is reachable
  through MSLU; what it is used for will show at boot.
  `drvmgt.dll` (a SafeDisc helper) is shipped but nothing loads it.

## The DLLs to lift

| file | version | preferred base | relocs | imports | notes |
|---|---|---|---|---|---|
| `msvcrt.dll` | 6.10.8924.0 | 0x78000000 | yes | 140 | above 1 GiB |
| `msvcp60.dll` | 6.00.8168.0 | 0x780c0000 | yes | 88 | above 1 GiB |
| `granny.dll` | (none) | 0x10000000 | yes | 64 | base collides with Rockall |
| `rockalldll.dll` | 3.02.8.0 | 0x10000000 | yes | 34 | `TlsAlloc`, `VirtualProtect` (guard pages) |
| `mss32.dll` | 6.1c | 0x21100000 | yes | 132 | code also in a second section, `MSSMIXER` |
| `mp3dec.asi` | 6.1c | 0x26f00000 | yes | 45 | Miles provider |
| `binkw32.dll` | 1.5J | 0x30000000 | yes | 107 | 30 code sections (one per pixel format) |

None has a writable code section, so there is no sign of run-time code
generation. Two consequences for the loader:
- **Base collision:** Granny and Rockall both want 0x10000000, so at least
  one image must be relocated. The lifter must apply `.reloc` when it lifts
  an image at a non-preferred base.
- **Guest address space:** `msvcrt` and `msvcp60` sit at about 1.9 GiB,
  above GunBound's 1 GiB guest limit. This settles the guest-space decision
  in `PLAN.md`: reserve 4 GiB.

## Ghidra function list

Ghidra 12.1.2 headless with default auto-analysis took about 25 minutes on
`aomx.exe`. The command is:

```
analyzeHeadless build/ghidra aom -import orig/aomx.exe \
    -scriptPath tools/ghidra -postScript ExportFunctionsCsv.java build/survey/aomx.functions.csv
```

It found **28905 functions**. Function ID matched only 41 names: the CRT
helpers (`_ftol`, `__alldiv`, `__allmul`, `_CIpow`), MSLU internals
(`_GodotFail*`, `_GetProcAddressInternal`) and `_AIL_shutdown`. Everything
else is `FUN_`/`thunk_FUN_`, so names have to come from RTTI, assertion
paths and the XS table, as planned. The project is kept in `build/ghidra`
for later scripts.

Compared with the lifter's own discovery: 719 starts that only Ghidra found
(small functions reached through vtables the pointer scan missed), and 10788
that only the lifter found (catch funclets, labels taken as code pointers,
and starts inside functions Ghidra merged). With Ghidra's list as seeds
(`--csv`):

| | |
|---|---|
| functions | 39846 (seed=28905, call=67, data-code=3146, data-label=1242, imm-code=6473, imm-label=13) |
| code section bytes covered | 5634929 of 5855095 (96.2%) |
| jump tables | 407 (bound=178, bound2=141, scan=88) |
| indirect jumps | table=451, import-thunk=3820, runtime-dispatch=237 |
| decode failures | 0 (5 overlapping decodes) |
| trapped instructions | 8290 sites, all MMX, SSE1 or 3DNow! |
| time | 62 s analysis + 11 s emission |

`build.sh` should use this list as its `--csv`, the way GunBound uses
`PROGRESS.csv`.

## DLLs through the lifter

Two changes to the lifter (in `lift/`, which is now ahead of GunBound's):

- **All code sections.** The code range spans every code section, so Bink's
  30 code sections and Miles's `MSSMIXER` are included. The lifter refuses an
  image with a non-code section between code sections.
- **Exports as seeds.** Exports become function starts named after the
  export (forwarders and exported data are skipped).

Each DLL is still lifted on its own at its preferred base; one `f_` namespace
per image and relocation to another base are still to do (step 1).

| file | functions | code bytes covered | decode failures | trapped |
|---|---|---|---|---|
| `msvcrt.dll` | 1552 | 198933 / 201330 | 2 | 28: `int`, `in`, `out`, `aam`, `mov ds`, `xlatb` |
| `msvcp60.dll` | 3391 | 170185 / 170915 | 0 | 0 |
| `granny.dll` | 3396 | 343358 / 380853 | 0 | 1616, MMX |
| `rockalldll.dll` | 349 | 43856 / 46202 | 0 | 3: `prefetcht0`, `prefetchnta` |
| `mss32.dll` | 1075 | 214769 / 247570 | 0 | 0 |
| `mp3dec.asi` | 135 | 53530 / 65858 | 0 | 2054, MMX and 3DNow! |
| `binkw32.dll` | 606 | 204633 / 282951 | 0 | 4154, MMX |

The trapped instructions in `msvcrt.dll` and its two decode failures are
data decoded as code (`in al, 0x13` after `aam 0x13`), on paths that are
never taken.

The SIMD in Granny, Bink and Miles is behind CPU detection: each has one
`cpuid` site:
- `granny.dll`: 0x1004fb9d
- `binkw32.dll`: 0x3002817e
- `mss32.dll`: 0x2113d007

`mp3dec.asi` has no `cpuid`. Its MMX and 3DNow! decoder is presumably
selected by what Miles detected, which run time will confirm.

## Reference runs under Wine

Wine 10.0 on Xvfb. A run gets to the main menu and through a 1v1 random-map
game against the AI with every system working: Direct3D 8 (wined3d on Mesa),
Granny animation, Miles music, the GDI-drawn interface text.

Setup, all under the ignored `build/`:

- **Wine prefix:** `build/wine-prefix`, only for these runs.
  - The Wine virtual desktop is on:
    `HKCU\Software\Wine\Explorer` `Desktop=Default`, and
    `HKCU\Software\Wine\Explorer\Desktops` `Default=1024x768`.
  - Its user folders are real directories, not Wine's default links into
    `~/Documents` etc. Otherwise the game writes its profile, logs and
    recordings into the real home directory.
- **Game directory:** `build/wine-game`, a writable copy of `orig/`. Files
  over 1 MiB are symlinks; the rest (4528 files) are copies.
- **Display:** `Xvfb :99 -screen 0 1024x768x16 -nolisten tcp`. It must be
  16-bit: see the mode below.
- **Command:**
  `wine aomx.exe +noIntroCinematics`. Without that switch the Bink intro
  video keeps the screen black.
- **Input:** `tools/xinput.py` (XTEST through ctypes).
- **Screenshots:** `import -window root` from ImageMagick. `xwd | convert`
  gets the colours of a 16-bit visual wrong.

What the runs showed:

- **Display mode.** The game always switches to full screen with
  `ChangeDisplaySettings(640x480, 16 bpp)`. That size and depth come from
  the profile (`<resolution>640`, `<displaybitdepth>16`, `<windowmode>0`).
  On a 24-bit X screen Wine has no 16-bit mode, so the call fails with
  `DISP_CHANGE_BADMODE` and the game shows "Initialization Failed". Even in
  a virtual desktop the depth has to be available. The recompilation's
  `ChangeDisplaySettings` must accept 640x480x16, or the profile must be
  changed.
- **Licence dialog.** `EBUEulax.dll` shows its licence dialog
  (a rich-edit control) on the first start, before Direct3D is created, and
  stores the acceptance in the registry. In the recompilation the stub has
  to report acceptance without showing anything.
- **The player's folder.** Profiles (`Users\*.prf`, UTF-16 XML), logs
  (`Acommlog.txt`, `RTS3banglog.txt`), `Savegame`, `Scenario`, `Screenshots`,
  `AI2`, `RM2`, `trigger2` and `startup` all live under
  `My Documents\My Games\Age of Mythology`. The game finds the folder with
  `SHGetFolderPathW(CSIDL_PERSONAL)` through MSLU. The installer's
  `Users\Default.prf` in the game directory is the template.
- **Recording.** Single-player random-map games are recorded when the
  profile has `<sprmrecordgame>1`; the installer's profile has 0. The file is
  `Savegame\Recorded Game.rcx`. It is opened when the game starts and
  written as play goes on (a 10-byte header at first).
- **Recording format.** `l33t`, then the uncompressed size as a 32-bit
  little-endian number, then a zlib stream. The file stays a 10-byte stub
  while the game runs and is written in full when the game ends, as
  `Recorded Game N.rcx`.
- **First reference recording.** `build/recordings/rec1-hard-1v1.rcx`
  (SHA-1 `99252645a52971e9a1db706a1b9d27f24d2c672c`, 26278 bytes,
  388124 uncompressed).
  - The game: 1v1 random map (it rolled a desert map), Zeus against a Hard
    AI. One villager was sent to the berry bushes, the AI's army raided the
    town and killed every villager, then the player resigned.
  - Replayed under Wine (Single Player > Recorded Games, Play Fast), it
    shows the same raid and ends in the same state: population 0/15 with
    the Town Center standing.
- **Redrawing.** Under Xvfb with software GL the window repaints only when
  the pointer moves, so scripts nudge the pointer before each screenshot.
  The simulation runs regardless, at about 2.6 cores of llvmpipe.
- **Music.** The game opens its MP3 music files (`sound\xpack\xmusic\*.mp3`)
  directly for Miles' streamed playback.

## Reference trace to the main menu

`WINEDEBUG=+relay,+d3d8,+loaddll,+seh`, from launch to the main menu and two
minutes idling there (6217 frames). The raw log is 3.9 GB, kept compressed
as `build/survey/trace-menu.log.zst`. `tools/trace_summary.py` keeps only
the calls made from the game's own images and writes
`build/survey/trace-menu/`: `modules.txt`, `imports.txt` (API, calling
module, count), `sequence.txt` (first call of each API, in order),
`d3d8.txt` and `d3d8-seq.txt`.

- **API surface.** 383 distinct (API, caller) pairs: 220 from `aomx.exe`,
  the rest from the DLLs (`EBUEulax` 35, `mss32` 34, `granny` 27,
  `msvcrt` 24 (Wine's own), `mp3dec` 23, `rockalldll` 10, `msvcp60` 10).
  Only the game's threads count: Wine's explorer and services processes
  also run an EXE at 0x400000, and the summary tells them apart by the
  threads that attach a game-only DLL.
- **Hot calls.** Per two minutes of menu: `WideCharToMultiByte` 1.7 M,
  `wcslen` 576 k, `_wcsicmp` 366 k, `_ftol` 314 k, `ReadFile` 302 k,
  `strtok` 295 k, `timeGetTime` 275 k, `SetFilePointer` 228 k,
  `CompareStringW` 199 k. Lifting msvcrt keeps most of these off the import
  path; `WideCharToMultiByte` and `CompareStringW` need fast host versions.
- **Unicode layer.** On NT the MSLU loader never loads `unicows.dll`: the
  helper at 0x4cdce7 calls `LoadLibraryA` for `user32`, `advapi32`,
  `version` and `gdi32`, no `GetProcAddress` call for a W function shows in
  the trace, and the W functions are then called straight from `aomx.exe`.
  The recompilation should give the MSLU lookups the same handlers as the
  system DLLs' imports.
- **Run-time loads from game code.** `LoadLibraryW` for `d3d8.dll`
  (`Direct3DCreate8`), `dsound.dll` (`DirectSoundCreate8`,
  `DirectSoundEnumerateW`), `shfolder.dll` (`SHGetFolderPathW`),
  `EBUEulax.dll` (`EBUEula`, unloaded again afterwards), `dpnhpast.dll`
  (loaded and freed at once, twice), `xpackLanguage.dll` (string
  resources). `LoadLibraryA` for `imm32.dll` (five IMM functions),
  `user32`/`advapi32`/`version`/`gdi32` (from the CRT-style helper at
  0x4cdce7), `mp3dec.asi` (from `mss32`) and `RICHED32.DLL` (from the
  licence dialog). `GetProcAddress(gdi32, "GetFontUnicodeRanges")`.
  Granny, Miles, mp3dec and the licence DLL each look up
  `IsProcessorFeaturePresent`.
- **Load addresses.** Granny and Rockall both prefer 0x10000000; Windows
  (and Wine) gives Rockall that base and relocates Granny (to 0x01000000
  under Wine), and the licence DLL too. Wine used its built-in `msvcrt`, so
  the trace has no calls from the game's own `msvcrt.dll`.
- **Threads.** `aomx.exe` creates four threads before the menu; `msvcrt`
  (`_beginthread`) one more.
- **Timing.** `QueryPerformanceCounter` 4.8 k calls and `timeGetTime` 275 k:
  the virtual clock must serve both.
- **Direct3D 8 at the menu.**
  - `CreateDevice` with `D3DCREATE_SOFTWARE_VERTEXPROCESSING` (0x40).
  - No vertex or pixel shader programs: the 16 `CreateVertexShader` calls
    are declarations only (`byte_code` NULL), and drawing uses FVF codes.
  - Textures are 16-bit: `A4R4G4B4`, `R5G6B5` and `A1R5G5B5`, in the
    managed pool, filled with `LockRect` and `CopyRects`.
  - A frame is `TestCooperativeLevel`, `BeginScene`, `Clear`, about 35
    times (`SetTexture`, `SetMaterial`, `Lock`/`Unlock` of one dynamic
    vertex buffer, `SetStreamSource`, `SetVertexShader`, `DrawPrimitive`),
    `EndScene`, `Present`. Render and texture-stage states change on
    about one draw in nine.
  - One `SetGammaRamp`, two lights.

## Translator additions

Found by lifting the DLLs; each has a test in `test/snippets.S`. The suite
passes: 117 tests, 94602 vectors, with and without flag liveness.

- **`lock inc` and `lock dec`** on 8-, 16- and 32-bit memory: reference
  counts in Granny (`lock inc byte ptr [eax]`) and Miles. They use the new
  `rt_atomic_add8/16/32` helpers in `runtime/cpu.h`.
- **`cmpxchg8b`:** Rockall's lock-free heap. It is always atomic, through
  `rt_cmpxchg8b`, and changes only ZF.
- **`pop ds`, `pop es` and `pop ss`:** Miles saves and restores `ds`/`es`
  around its CPU detection. Under the flat model the pop only discards the
  slot; `pop fs` and `pop gs` stay trapped.

## Multi-image lift (step 1)

`lift.py --module NAME` names every function `f_<NAME>_<addr>` and makes
`tables.c` define a single `RtImageDesc rt_image_<NAME>` (bases, code range
and CRC, entry, function table, return sites, patches; `runtime/rt.h`).
Without `--module` the output is the single-image form the instruction tests
use. `--rebase ADDR` applies the PE's base relocations first.

All seven images lift into `build/gen-mod/<module>/` and their objects link
together with no symbol clashes:

| module   | base       | functions | decode failures | trapped sites |
|----------|------------|----------:|----------------:|--------------:|
| aomx     | 0x00400000 | 39846 | 0 | 8290 (64 kinds: MMX, SSE, 3DNow!) |
| msvcrt   | 0x78000000 |  1552 | 2 | 28 (`int`, `in`/`out`, `xlatb`, `aam`) |
| msvcp60  | 0x780c0000 |  3391 | 0 | 0 |
| granny   | 0x10100000 (rebased) | 3397 | 0 | 1612 (MMX) |
| mss32    | 0x21100000 |  1075 | 0 | 0 |
| mp3dec   | 0x26f00000 |   135 | 0 | 2054 (MMX, 3DNow!) |
| rockall  | 0x10000000 |   349 | 0 | 3 (`prefetch`) |

- **Granny's base.** Granny and Rockall both prefer 0x10000000. Rockall
  keeps it (as under Wine) and Granny is lifted at 0x10100000, just past
  Rockall's end at 0x10014000.
- **Guest layout.** `runtime/rt.h` now reserves the full 4 GiB: heap
  0x01000000-0x10000000, VirtualAlloc arena 0x30000000-0x70000000, host
  entry points at 0x7ff00000. The DLL ranges stay free.
- **SIMD.** The trapped sites are the MMX, SSE and 3DNow! paths that the
  `cpuid` routines select. The runtime will report a CPU without them, so
  they should never run; the trap report at run time confirms it. Game
  state cannot depend on them anyway: Athlon (3DNow!) and Pentium 4 (SSE)
  players stay in sync in one multiplayer game, and the reference recording
  was made under Wine with SSE reported, so replaying it on the x87 paths is
  itself the check. MMX and SSE1 translation (plan step 1) is therefore
  deferred until a trap shows one is reached.
- **Not done yet:** the runtime's dispatch, loader and `main` still handle
  one image (step 2).
