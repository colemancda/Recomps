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
start of `s3e_code`. The `blx` count is high, so whether that's indirect calls
through the s3e API table or ARM↔Thumb interworking has to be settled before
the lifter is written. The VFP count matters for armv6 (VFPv2).

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

- **Two lifting targets.** `s3e_code` is the game, and its only way out is
  Marmalade's s3e API. `__text` is Marmalade's iOS layer, which talks to
  UIKit/EAGL/AudioQueue through 32-bit Objective-C. One option is to lift only
  `s3e_code` and reimplement the s3e API natively on arm64 iOS, which avoids
  bridging 32-bit Objective-C at all. The other is to lift both and keep a
  Bridge32-style Objective-C bridge. The first needs the s3e API boundary
  mapped (how `s3e_code` reaches `__text`: the branch table at 0x65f74 and
  `blx` through function tables), and that's the next survey item.
- **Lifter.** GunBound's lifter is x86-only, so this needs an ARM (A32,
  armv6 + VFPv2) frontend. Flags are NZCV plus conditional execution on nearly
  every instruction; there's no Thumb in `s3e_code` at first glance, but that's
  unconfirmed.
- **Memory.** All guest addresses sit below 0xa1b000 plus heap; the same
  "guest memory at a fixed host base" scheme as GunBound's iOS plan applies.
