# Step 0 survey: `kiwi` (CoD: WaW Zombies 1.5.0, armv6)

Addresses are virtual addresses in `kiwi` (no ASLR; `__TEXT` at 0x1000).
Tools: `otool`, capstone 5.0.7, Bridge32 `b32-inspect` and the Bridge32
Catalyst shell.

## It's a Marmalade (Airplay SDK 4.3.0) app

The log prints `Airplay v4.3.0beta [231940u]`; the app delegate is
`s3eAppDelegate`, and there are `s3eKeyInput`, `GLView` and `MainController`
classes, plus `s3eGL_start`. The game's data are Marmalade derbh archives
(`_main.dz`, `_nacht*.dz`, `_verruckt*.dz`, `_shinonuma*.dz`, `_derriese*.dz`).
The executable has two parts:

| segment / section | address | size | what |
|---|---|---|---|
| `__TEXT,__text` | 0x00002040 | 360580 | Marmalade's iOS loader and s3e platform layer (Objective-C, UIKit, EAGL, AudioQueue) |
| `__TEXT,__symbol_stub4` | 0x0005a0c4 | 5844 | import stubs (866 binds) |
| `__TEXT,s3e_code` | 0x00065f00 | 2483936 | **the game**: code and read-only data of the s3e image, ARM mode |
| `__DATA` | 0x00a00000 | ~56 KB | loader data, Objective-C metadata |
| `__S3E_DATA,s3e_data` | 0x00a0e000 | 50800 | game data; begins with an `XE3U` header, then the embedded `.icf` config text |

Entry is `LC_UNIXTHREAD` at 0x20a8, with one module initializer.

The game addresses used by Bridge32's controller profile
(`Bridge32/core/games/CodZombies.mm`, ported from
[codwawzpc](https://github.com/aurelioochoa/codwawzpc)) all fall in
`s3e_code`: functions such as 0x158664 (post `Event_AnaloguePadPosition`) and
vtables at 0x2a…–0x2b… (`CGame`, `CEnt`, `CIwGuiFrame`, …). Its globals
(0xa15080 `CGame *`, 0xa1a54c `CLevelManager *`) are in `s3e_data`/`__DATA`.
codwawzpc checks the same SHA-256, so its names and addresses apply directly.

### Code statistics (linear sweep, ARM mode)

| | `s3e_code` | `__text` |
|---|---|---|
| words decoded | 602336 | 87326 |
| undecodable words | 18648 | 2819 |
| distinct BL targets | 6374 | 2783 |
| `blx` | 13627 | 95 |
| VFP instructions | 2846 | 476 |

The sweep runs over interleaved literal pools and rodata (lots of
`andeq`/`eoreq`), so these are upper bounds; a real function list needs
recursive descent from the entry points, vtables and the branch table at the
start of `s3e_code`; see the next section.

### Recursive descent (`tools/s3e_survey.py`)

```
python3 -I tools/s3e_survey.py --funcs build/s3e_funcs.tsv
```

Roots are the start routine, the 251-entry branch table, runs of two or more
code pointers in data (vtables, callback tables), lone Thumb pointers to a
`push {…, lr}`, and code pointers loaded from literal pools. From there it
follows branches, calls and ARM jump tables in both instruction sets. Each
function is walked into scratch state and dropped if the walk hits something
the game's armv6 code can't contain: an undecodable word, `svc`/`bkpt`, a
32-bit Thumb instruction other than BL/BLX, an armv6 media instruction, or a
non-VFP coprocessor instruction. 137 roots were dropped this way, mostly
table words that pointed into strings.

| | |
|---|---|
| game code | 0x65f00–0x2ba6ac (2443180 bytes; the stubs follow) |
| functions | 15712: 13292 ARM, 2420 Thumb |
| found by | 9684 tables, 5054 calls, 688 literal pools, 251 branch table, 34 lone pointers |
| instructions | 434430 ARM, 101448 Thumb |
| coverage | 80.0% of the bytes |
| jump tables | 101 |
| indirect calls | 4227 (virtual calls, callbacks) |
| other indirect jumps | 1152 (writes to pc that aren't returns or jump tables; not yet classified) |
| VFP | 2272 instructions, all ARM |
| `svc` | none |
| r9 | 12713 instructions touch it, 6002 write it: an ordinary register |

So the game is mostly **ARM**, with Thumb-1 for about 15% of its functions
(the start routine's constructor and main path among them). The `blx` count in
the linear sweep is ARM↔Thumb interworking. Of the 478 KB not covered, by a rough
byte classification about 83 KB is text, 116 KB pointer tables and 179 KB
other data. About 100 KB still looks like ARM code: unreferenced functions, or
ones reached only through pointers this pass doesn't follow (computed
addresses, `adr`). A lifter needs a
fallback for an indirect branch to an address that wasn't lifted.

## Under Bridge32

```
cd ~/Developer/Bridge32
cmake -S . -B build/catalyst -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/catalyst.cmake
cmake --build build/catalyst
tools/make-app.sh /Volumes/4TB/Downloads/IPA/Decrypted/com.activision.callofduty-1.5.0-decrypted.ipa build/apps/CODZombies.app
open --env B32_DUMP_WINDOW=1 build/apps/CODZombies.app
```

Result (2026-11-15): loads, `UIApplicationMain` with `s3eAppDelegate`, main
nib, a loader thread, `s3eGL_start`, skips `intro.mp4`, and draws the title
screen ("Touch screen to start") in a 320x480 guest screen. Bridge32 started
at 4x render scale and stepped it down to 1x, so the JIT can't keep up at higher
scales. Play past the title screen was not tried in this session.

Bridge32 is the reference for the recompiled build: same guest, same thunks,
so frame dumps (`B32_DUMP_FRAMES`) and import traces (`B32_TRACE_THUNKS`) from
it can be compared with the recompiled build.

## What this means for the plan

- **Lift only `s3e_code`.** `s3e_code` is the game, and its only way out is
  809 stubs into Marmalade's iOS layer: about 150 s3e functions plus 98
  OpenGL ES functions passed straight through. Reimplementing those natively
  on arm64 iOS avoids lifting `__text` and bridging 32-bit Objective-C. See
  `S3E_BOUNDARY.md`.
- **Lifter.** GunBound's lifter is x86-only, so this needs an ARM frontend:
  A32 (armv6, with VFPv2 in ARM code only) for 85% of the functions and
  Thumb-1 for the rest, with interworking on `bx`/`blx`. Flags are NZCV, with
  conditional execution on nearly every ARM instruction. r9 is an ordinary
  register.
- **Memory.** All guest addresses sit below 0xa1b000 plus heap; the same
  "guest memory at a fixed host base" scheme as GunBound's iOS plan applies.
