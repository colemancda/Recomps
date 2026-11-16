# Static recompilation of Call of Duty: World at War: Zombies (iOS, 1.5.0)

Goal: turn the original 32-bit iPhone executable (`kiwi`, armv6) into a native
arm64 iOS app by lifting its ARM code to C and linking it with a runtime, the
way `../GunBound` and `../AgeOfMythology` lift x86 Windows executables. No JIT,
so the result is an ordinary signed iOS app.

Nothing derived from the game is committed. `orig/` (the unpacked IPA) and
`build/` are ignored.

## Input

```
tools/extract.sh            # unpacks the decrypted IPA into orig/Payload/kiwi.app
```

| | |
|---|---|
| bundle | `com.activision.callofduty`, `kiwi.app`, version 1.5.0, MinimumOSVersion 3.0 |
| executable | `kiwi`, Mach-O **armv6 only** (1.5.0 has no armv7 slice) |
| SHA-256 | `4c940898943a1ac63318083b17bc04219251ad4b3cd2b846eaf41c1702f98f0d` |
| source | `/Volumes/4TB/Downloads/IPA/Decrypted/com.activision.callofduty-1.5.0-decrypted.ipa` |

The App Store copy (`IPA/COD Zombies 1.5.0.ipa`) is FairPlay-encrypted
(`cryptid 1`) and can't be used; `extract.sh` refuses it. The two decrypted
IPAs in `IPA/Decrypted/` contain the same executable.

## Status: step 2 (runtime bring-up)

- The decrypted binary runs under Bridge32 (dynarmic) as a Mac Catalyst app
  up to the title screen; see `docs/SURVEY.md`.
- The boundary between the game and Marmalade's loader is mapped:
  `tools/s3e_boundary.py` lists all 809 import stubs with their calling
  convention. The game needs about 150 s3e functions and 98 OpenGL ES
  functions; see `docs/S3E_BOUNDARY.md`.
- The loader calls back into the game through one routine (file systems for
  the `.dz` archives, timers, device events), and r9 is an ordinary register.
- `tools/s3e_survey.py` finds 15712 functions by recursive descent (85% ARM,
  15% Thumb-1), covering 80% of the game code.
- The lifter (`lift/`) turns all 15478 functions into C with nothing left
  untranslated. `./build.sh ios` compiles it for arm64 iOS. Single
  instructions (72k runs) and 400 whole leaf functions match Unicorn; see
  `docs/LIFTER.md`.
- The runtime (`runtime/`, `docs/RUNTIME.md`) runs the lifted game headless on
  this Mac (`./build.sh host`). It decrypts the game's data as Marmalade's
  loader does, reads the embedded config and answers 39 identified s3e
  functions, among them files, the game's own archive file system and LZMA
  decompression. The game loads its bootstrap data, localisation and
  achievement groups and writes its save files. It then falls back to software
  texture pages because GL isn't there yet, which is the next piece.

See `docs/SURVEY.md` for the binary's layout and what it means for the plan.
