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

## Status: step 0 (survey)

- The decrypted binary runs under Bridge32 (dynarmic) as a Mac Catalyst app
  up to the title screen; see `docs/SURVEY.md`.
- Not started yet: the ARM lifter and the runtime.

See `docs/SURVEY.md` for the binary's layout and what it means for the plan.
