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
- **Runtime side:** dispatch, the loader and `main` handle all images (see
  the next section).

## Booting the recompilation (step 2, in progress)

`./build.sh` lifts the seven images plus `msxml4.dll`, compiles and links
`build/host/aom`; `build/run/go.sh` runs it headless with the game files read
from `orig/` and every write going to `build/run/`.

What runs so far, in order:

- **Loading.** All images map at their bases; Granny's 10,226 relocations are
  applied; imports between lifted images are linked directly. DllMain runs for
  msvcrt, msvcp60, Rockall, Granny and Miles (each after the DLLs it imports),
  then `aomx.exe`'s CRT start-up and its static constructors.
- **MSLU.** `_GetProcAddressInternal` (0x4cd815) walks a DLL's export
  directory by hand; it is replaced by a host hook (`lift.py --hook`,
  `rt_hooks[]` in `runtime/aom.c`) that answers from the import handlers.
  MSLU decides between NT and Windows 9x by `GetFileAttributesW("???.???")`
  failing with `ERROR_INVALID_NAME`; it does now, so it calls the W functions
  directly.
- **Start-up checks the game makes**, and what satisfies them:
  - `shfolder.dll` must load (`SHGetFolderPathW` finds My Documents, mapped
    to `C:\Documents and Settings\Player\My Documents` under
    `build/run/drive_c`).
  - DirectX 8.1 is detected as in the DirectX SDK's `GetDXVersion`:
    `dpnhpast.dll` (the DirectPlay NAT helper) must load.
  - The language strings come from `xpackLanguage.dll` and `language.dll`,
    resource-only DLLs mapped as data (`LoadStringW` on their handles).
  - The licence stub (`EBUEula`) is called once and accepts.
  - The main window is created and `Direct3DCreate8` is called.
- **MSXML 4.0.** The game reads its XML data through MSXML 4's COM DOM
  (`CoCreateInstance`). The installer carries MSXML 4.0 in
  `orig/msxmlenu.msi`; `build.sh` extracts `msxml4.dll` from it into
  `build/extracted/` and lifts it like the game's DLLs, and the runtime's
  `CoCreateInstance` asks the lifted DLL's `DllGetClassObject`. Its imports:
  ole32 (9), shlwapi (29, a dozen by ordinal, mapped with Wine's
  `shlwapi.spec`) and kernel32.
  - msxml4 has no `.rdata`: its vtables are in `.text`, so the lifter now
    also takes relocated words inside code sections as pointers. With
    Ghidra's function list (5962 functions) as seeds.

Runtime pieces added for this: per-module function lookup (a two-level page
table), the multi-image loader, a null Direct3D 8 device (`d3d8.c`), GDI with
FreeType fonts (`gdi.c`, Liberation fonts standing in for Arial and Times New
Roman), W forms of the user32, kernel32 and registry functions, semaphores,
directory enumeration, per-module resources, display modes, and the AoM
registry defaults (the installer creates only an empty key).

### Where it stands

The game now runs its frame loop headless: `build/host/aom --headless --exe
orig/aomx.exe --savedir build/run --frames 600 -- +noIntroCinematics` presents
600 frames and exits cleanly (about 35 frames a second on the null device).

- **Display.** Without a player profile the game picks 1024x768 at 16 bits.
  A steady frame is about 1,170 draws (780 `DrawIndexedPrimitive`, 386
  `DrawPrimitive`) with transforms, materials and lights: a 3D scene, not
  the 35-draw 2D menu seen under Wine at 640x480. What it is will be visible
  once step 3 draws it.
- **Sound** is reported as unavailable (`DirectSoundEnumerateW` and
  DirectSound 8 are not provided yet); the game says so in a dialog and
  carries on.
- **Dialogs** cannot be shown: `DialogBoxIndirectParamW` logs the template's
  title and texts and answers IDOK. The game's error dialogs were how the
  font search bug and the sound state were found.
- **A guest path to check:** the game opens
  `\C:\Program Files\...\users\Default.prf` (a leading backslash before the
  drive), which fails. Probably a path built from a value it expects in a
  different form.

Runtime bugs found on the way:

- **Heap size classes.** A request of 2041-2048 bytes made a 2064-byte "small"
  block whose bin (128) was also the first large bin, so a freed one was later
  handed out as a 2560-byte block and overrun. A 32x32 16-bit texture is
  exactly 2048 bytes. Fixed in `runtime/mem.c`; the same code is in
  `GunBound/runtime/mem.c`. `RECOMP_HEAP_DEBUG=1` fills freed blocks and
  checks them on reuse, and reports who freed a block that was written
  after free.
- **Directory search in the overlay.** When a folder exists in the write
  overlay (the game creates empty `Fonts` there), the game-tree side of the
  search was joined verbatim (`orig/Fonts`) instead of matched without regard
  to case (`orig/fonts`), so `fonts\xfonts.xml` was "not found" and the game
  reported an XML error.
- **Lifter coverage.** A vtable slot (`0x720070`) shaped like UTF-16 text was
  rejected; data words beside other code pointers now count as vtable entries.

## Direct3D 8 on OpenGL ES 3 (step 3, first version)

`runtime/d3d8gl.c` draws for the device in `runtime/d3d8.c` (shared state in
`runtime/d3d8.h`):

- **Context.** EGL, Mesa's surfaceless platform first (no window system
  needed, so headless runs draw too), the default display otherwise. The game
  draws into a framebuffer object; Present reads it back as RGB565 for
  `video.c`, so the window, `--dump-frame` and `--frames` work unchanged.
  With a window, SDL must not hold a GL context on the same thread
  (libglvnd refuses an EGL and a GLX context current together): the window
  uses SDL's software renderer with `SDL_HINT_FRAMEBUFFER_ACCELERATION` off.
- **Fixed function.** One generated GLSL ES program per state combination
  (vertex format, lighting and lights, material sources, texture stage
  operations and arguments, texture coordinate generation and transforms,
  fog, alpha test); uniforms carry matrices, lights, material, fog, texture
  factor. Vertex input comes from FVF codes or from the declarations of
  `CreateVertexShader`; vertices are copied from guest memory per draw.
- **Textures** are converted to RGBA8 (DXT1-5 decoded in software) when first
  used after a change.
- **Culling.** D3D's and GL's windings agree here: D3DCULL_CCW culls GL's
  front (counter-clockwise) faces. Getting this backwards hid the terrain and
  the whole interface.
- **Debugging:** `RECOMP_D3D_LOG=N` describes every draw of frame N;
  `RECOMP_D3D_NODEPTH`, `RECOMP_D3D_NOCULL` switch those tests off;
  `RECOMP_NO_GL` keeps the null device.

Result: the main menu (with its 3D background), the Single Player menus and
a random-map game render, and mouse and keyboard input work in a window.

Fixed on the way:

- **Window and display mode.** A full-screen device now sets user32's display
  mode and resizes its window with WM_MOVE/WM_SIZE, as Windows does;
  `video.c` maps the mouse to the size of the presented frames.
- **Paths like `\\C:\dir`** (left by MSXML's URL-to-path conversion) are
  resolved as `C:\dir`; the random map descriptions and `Default.prf` load.
- **Lifter:** a relocated word is treated as a pointer even where the same
  value is also an instruction immediate (an msxml4 callback was missed).
- **`xlatb`** is translated (msvcrt's `_CIatan2`), with a test snippet.

Open:

- Render targets other than the back buffer are skipped (logged once).
- Text uses the serif Liberation face where Wine showed Arial-like sans: the
  typeface the game asks for needs checking.

## Cursor and sound

- **1024x768.** The earlier report of dead controls at 1024x768 no longer
  reproduces: headless (scripted input) and through the X window (XTEST
  clicks on the scaled 640x480 window), Single Player, Random Map, Hard and
  START all work and the game starts. It was fixed by the guest screen
  following the presented frame size.
- **Cursor.** The game never calls `SetCursorProperties`: it uses Windows
  cursors, `LoadCursorW(hInstance, id)` from aomx.exe's resources (35 shapes)
  and `SetCursor` every frame. `LoadCursor` now decodes the `RT_GROUP_CURSOR`
  / `RT_CURSOR` resources (1, 4, 8, 24 and 32-bit DIBs with their AND mask) to
  ARGB, and the video layer makes SDL colour cursors of them; stock `IDC_*`
  cursors map to SDL system cursors and `SetCursor(NULL)` hides the pointer.
  The window shows the pointer in full screen too (GunBound drew its own).
  Checked with XFixes: the X server shows the game's bronze arrow.
- **Sound.** Miles (mss32) enumerates devices with `DirectSoundEnumerateW`
  before creating one; with that added it uses the runtime's DirectSound
  mixer. Buffers answer `QueryInterface` for `IDirectSoundBuffer8`. aomx.exe
  reads WAV files through winmm's mmio functions, now provided (files and
  `FOURCC_MEM` buffers, RIFF descend/ascend). The menu music (MP3, decoded
  by the lifted mp3dec) and UI clicks play; in a skirmish the start fanfare
  and unit sounds mix in (`--dump-audio`).


## Replaying the reference recording (step 5, started)

- **Fonts.** The menus ask for `Georgia` and `GiovanniITCTT` (serif) as well as
  `Arial` / `Arial Unicode MS`. The reference Wine prefix has no fonts at all,
  so Wine drew everything with its sans fallback; the serif text here is what
  the game asks for, not a mapping error.
- **Render targets.** Menus and a skirmish never call `SetRenderTarget`.
- **Loading `rec1`.** Single Player > Recorded Games lists the `.rcx` files in
  `Savegame` (the reference recording copied there as `rec1.rcx`).
- **`wsprintfW`** (through MSLU) builds `<savegame>\rec1-*.rbm`. It was
  missing, and adding it exposed a calling-convention bug: functions handed out
  through `rt_host_fn` (MSLU's lookups, `GetProcAddress`) were always
  stdcall, so the cdecl `wsprintfW` popped its two arguments and the caller
  popped them again. The game then read its buffer 8 bytes late (the find
  pattern lost `C:\D`) and later returned into the heap. `rt_host_fn` now takes
  the convention from the import table, and `GetProcAddress` no longer refuses
  cdecl functions.
- **Running.** The replay loads the same desert map and starting town as
  under Wine and runs without a fault; at normal speed the progress bar is
  about a third along after 10 minutes.
- **First full replay.** With Play Fast it runs to the end (headless, about
  25,000 presented frames): the Hard AI's army raids the town and kills every
  villager, the player resigns, and the game shows "You have been defeated"
  and "Record Game Ended" with the Town Center standing, as under Wine.
- **Postgame statistics match Wine exactly.** After the replay ends, F10 >
  View Postgame shows the statistics the simulation accumulated. The same
  replay under Wine (640x480, Play Fast) and under the recompilation
  (headless, 1024x768) give identical tables on every tab:
  - Score: Default 356 (economy 250, military 91, improvements 0,
    mythology 15); Megara 1565 (918, 277, 309, 61).
  - Economy: Default food 250, wood 255, gold 100, favor 15, civilian high 3;
    Megara 5621, 5966, 2875, favor 56, civilian high 56.
  - Military: Default 1 military, 9 killed, 0 razed, 4 lost, 1 building
    lost; Megara 34, 4, 1, 9, 1.
  - Improvements: Megara reached the Classical Age at 00:04:59, 12
    researches. Mythology: one settlement each, favourite soldiers
    Kataskopos (1) and Medium Toxotes (22), Megara's myth unit Centaur (1).
  - Timeline: the same population curves over the 14 minutes.
  The Hard AI's whole game (gathering, research, army, attack) is
  reproduced: the AI's decisions, pathing, combat and random numbers come out
  the same as on Windows-under-Wine for this recording.
- **Sync log.** `+alwaysLogSync` makes the game open `RTS3synclog.txt` when the
  sync manager is torn down (leaving the game, not `--frames`), but a retail
  build writes only the header unless it detects an out-of-sync. The
  postgame tables are the comparison used instead.
- **Navigation for these runs.** Recomp (1024x768, `--input`): Single Player
  505,422; Recorded Games 505,542; first file 290,265; Open 444,534; Play
  Fast 297,691 (after loading); F10 is `key 79 44`; View Postgame 512,313;
  tabs at y=38, x = 126, 280, 434, 589, 741, 896. Headless with Play Fast the
  replay ends before frame 28000. Wine (640x480 at the desktop's top-left):
  the same positions scaled by 0.625, F10 through `tools/xinput.py key F10`.

## Cross-architecture comparison (step 5)

- **Draw-stream hash.** `RECOMP_D3D_HASH=1` logs, at every Present, an FNV-1a
  hash of what the frame asked Direct3D to draw: primitive types and counts and
  the vertex and index bytes used (`d3d8.c`). It does not depend on the
  backend: x86 with OpenGL ES and x86 with the null device (`RECOMP_NO_GL=1`)
  give the same hashes. So the aarch64 build can be checked under
  `qemu-aarch64`, where there is no GPU and the sysroot has no software GL.
- **aarch64 build.** `./build.sh aarch64` (batocera h616 toolchain) builds
  cleanly. Under `qemu-aarch64 -L <sysroot>` with the null device it boots to
  the main menu in under two minutes and plays the menu music (the mixed peak
  matches the x86 run's).
- **Virtual clock.** `--virtual-clock` (from GunBound) makes guest time advance
  with the main thread's work. Boot, main menu, Single Player and the Recorded
  Games dialog (560 frames) give the same hashes on x86 (twice, with and
  without GL) and on aarch64.
- **Sound makes runs unrepeatable.** With sound on, two x86 virtual-clock runs
  of the replay diverge at frame 618 (just after loading starts), keep the same
  draw counts, re-converge now and then and end on the same static postgame
  screen: the content is the same, the timing is not. Miles' service thread
  (`CreateThread` + `Sleep`) and the runtime's headless mixer thread run on
  real time, and what the main thread finds when it calls into Miles changes
  how many clock reads a frame takes. With `--no-sound` two x86 runs agree on
  all 4000 frames compared. Comparisons use `--no-sound`; the simulation does
  not depend on sound.
- **The whole replay matches across architectures.** The reference recording,
  replayed with `--virtual-clock --no-sound` to frame 30000 (the game ends and
  "Record Game Ended" shows before frame 30000), gives the same draw-stream
  hash on every one of the 30000 frames on x86_64 with OpenGL ES, x86_64 with
  the null device and aarch64 under qemu with the null device. The aarch64 run
  took 45 minutes. With the postgame tables matching Wine, the 14-minute game
  is reproduced on Windows (Wine), x86_64 Linux and aarch64 Linux.
  `test/replay_hash.sh RECORDING` runs this check; `tools/replay_check.sh`
  captures the postgame tables (real time, with sound) for a by-eye check
  against Wine.

## Second reference recording

- **`build/recordings/rec2-moderate-1v1.rcx`** (SHA-1
  `998b202c9e462a8eaf09101cf220efff44ae00b8`, 27329 bytes, 359719
  uncompressed). Recorded under Wine: 1v1 random map (a green map), Zeus against
  a Moderate AI that rolled the Norse (Halogaland). The player queued villagers
  every three minutes and sent them to wood; the AI used the Great Hunt god
  power at once, reached the Classical Age at 4:43 and the Heroic Age at 14:50,
  traded (142 gold from caravans), trained 16 Medium Throwing Axemen and a
  Frost Giant, and burned the player's Town Center at about 16 minutes. The
  file is written at the defeat.
- **The live game's postgame tables** were captured under Wine before quitting
  (`build/recordings/rec2-postgame-wine/`): Score 507 / 1901; Economy food
  250 / 5918, wood 1024 / 6150, gold 100 / 3993, gold from trade 0 / 142,
  favor 15 / 45, civilian high 8 / 30; Military 1 / 39 units, 23 / 9 killed,
  0 / 3 buildings razed, 9 / 23 lost, 2 / 0 buildings lost; research 0 / 15.
- **Replayed by the recompilation** (`tools/replay_check.sh`, real time, with
  sound, FRAMES=36000), every table equals the live game's. That run's tabs
  are the reference set `build/recordings/rec2-postgame/` for later
  `tools/replay_check.sh ... build/recordings/rec2-postgame` runs.
- **Across architectures.** `test/replay_hash.sh rec2-moderate-1v1.rcx 36000`
  passes: all 36000 frames have identical draw streams on x86_64 and aarch64
  (qemu). An x86 run with OpenGL ES gives the same hashes and its frame 36000
  is the end of the game: "You have been defeated", "Record Game Ended", the
  Town Center burned down to a Settlement, as in the live game under Wine.

## Handhelds and the window renderer

- **Packaging.** `package.sh` builds `build/dist/aom-rocknix.tar.gz` (ROCKNIX port:
  launcher, program, the lifted `msxml4` DLLs, Liberation fonts; no game data). It runs
  on the Powkiddy X55 (RK3566, Mali-G52), the Retroid Pocket Flip 2 (Adreno 650) and,
  with its own `aom.sh`, a Debian tablet with GNOME (Lenovo Duet 3, Adreno 618).
  The program needs libSDL2, libEGL, libGLESv2, libfreetype and glibc 2.38.
- **Cursor.** The game's pointer is an SDL colour cursor, which the compositor shows only
  with a real mouse; stick or touch input moves the game's pointer alone. `--soft-cursor`
  draws the current cursor shape into the window at the guest pointer instead and hides
  the system pointer. The stock cursors, which have no pixels, get a plain arrow.
- **Window renderer.** The game's Direct3D frame is read back and presented through SDL.
  Under X11 that has to use SDL's software renderer (SDL's GLX context cannot share the
  thread with Direct3D's EGL one). Where SDL itself uses EGL (Wayland, KMS/DRM) the window
  now uses SDL's GPU renderer. SDL keeps its own record of which context is current and
  does not notice `d3d8gl.c`'s `eglMakeCurrent`, so before drawing the runtime releases and
  reselects SDL's context through SDL (`sdl_gl_reclaim`). With SDL 2 on SDL 3 (sdl2-compat)
  the software renderer cannot be created once framebuffer acceleration is off, which left
  the game without a window. `RECOMP_SDL_RENDERER=hardware|software` overrides the choice.
- **Open: text at 1024x768.** Glyphs with descenders (p, y, g) are drawn wrong at 1024x768
  (the descender is missing and shows up as a mark elsewhere, "Options" reads "Qptions");
  640x480 is clean. The raw frame has it, the x86_64 build too, so it is in the text
  textures, not in presenting. The rendered descenders do fit the reported ascent/descent.
