# Plan: the recompiled client on iOS (arm64)

For a separate working session. The Linux arm64 build already runs on a Retroid
Pocket 5 (see `recomp/README.md`); this plan ports the same lifted code and
runtime to iOS. Read the README first. The renderer is a separate plan
(`PLAN_GLES_RENDERER.md`): iOS needs the GPU path for acceptable speed, but the
first iOS milestones use the software rasteriser.

## What carries over unchanged

- The lifter and the lifted C. The code is plain C over a CPU struct and one
  guest memory mapping. No JIT, no code generation at run time, no
  `mprotect(PROT_EXEC)`, no Unicorn, so iOS code-signing rules are not a
  problem. This is the main reason static recompilation suits iOS.
- The runtime: loader, threads, exceptions, kernel32, user32 on SDL2, files,
  registry file, winsock on BSD sockets, DirectDraw/D3D7/DirectInput/DirectSound
  fakes, gamepad and touch input.
- SDL2 supports iOS (UIKit, OpenGL ES, CoreAudio, GameController, touch,
  on-screen keyboard through `SDL_StartTextInput`).

## What is different

1. **Toolchain.** iOS binaries need Xcode's clang and SDK, so the build runs
   on a Mac (Mac minis on the LAN: `alseys-mac-mini.local` in `~/.ssh/config`;
   first check that Xcode and `xcodebuild` are installed and which iOS SDK).
   The lifter is Python and can run anywhere; run it on the Mac too so the
   lifted C never leaves the user's machine.
2. **Guest memory base.** `mem.c` reserves 1 GiB at the fixed host address
   4 GiB so guest address `a` is host `0x100000000 + a` and the lifted code
   uses a compile-time constant. iOS arm64 executables have a 4 GiB
   `__PAGEZERO` and the address space above it is limited; a fixed `mmap` there
   may fail. First try: build with `-DRT_DYNAMIC_BASE` (variable base), measure
   the slowdown with the lifted blitters, and only then look at linker options
   (`-pagezero_size`, `-image_base`) which iOS may reject. Also check the
   `MAP_NORESERVE` 1 GiB reservation against the per-app memory limit; resident
   memory is much smaller, but test on a device. Request the
   `com.apple.developer.kernel.increased-memory-limit` entitlement if needed.
3. **Memory ordering.** The Linux build pins every guest thread to one CPU
   (`--guest-cpu`) to restore the x86 ordering that three lock-free queues rely
   on (input event ring, per-connection receive ring, game event ring; listed
   with addresses in the README). iOS has no CPU affinity. Options, in order of
   preference:
   a. Make those three rings safe by hand: in the lifter, emit
      release stores for the publishing writes and acquire loads for the
      consuming reads in the six named functions (`EnqueueInputEvent`,
      `ProcessInputEventQueue`, `ReceiveFramedPackets`, `ProcessIncomingPackets`,
      `QueueGameEvent`, `PumpBattleActions`), plus barriers already added around
      locked instructions, critical sections and events. Prove with a stress
      test on the Linux ARM device with `--guest-cpu any` and a soak run.
   b. Run all guest threads as coroutines on one host thread (as bo2-mac does):
      deterministic and TSO-equivalent, but every blocking call (`Sleep`, waits,
      socket I/O, audio) must yield. More work, more robust.
   Decide with data from (a)'s soak test. Do not ship iOS without one of them.
4. **Display and input.** SDL2's UIKit backend gives an OpenGL ES window (the
   software path presents through `SDL_Renderer` here, which also works). Touch
   already maps to an absolute pointer plus left button through the scaling
   rectangle; add: two-finger tap = right click, an on-screen keyboard for
   login, room titles and chat (`SDL_StartTextInput` when an EDIT control has
   focus), MFi/DualSense controllers through the existing gamepad code, and an
   optional on-screen overlay (stick plus A for Space). Safe-area insets, 4:3
   letterboxing on tall screens, orientation locked to landscape.
5. **Files and settings.** Game data is imported by the user into the app's
   Documents folder (`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`)
   through the Files app or Finder; `--gamedir`, `--savedir`, the registry file
   and the token live in the sandbox. A simple first-run screen (or a text file
   `gunbound.conf`) takes the broker address, user and password.
6. **Network.** Plain BSD sockets work. Add `NSLocalNetworkUsageDescription`
   (LAN servers, peer UDP on 8363) and handle the first-use permission prompt;
   `NSAppTransportSecurity` does not apply to raw sockets. Backgrounding closes
   sockets: on return, treat it as a connection loss the game already handles,
   and pause audio.
7. **Lifecycle.** Pause the game loop and audio on `applicationWillResignActive`,
   resume on return; with the GPU renderer, recreate GL resources from the CPU
   surface copies (the renderer plan makes this possible).
8. **Distribution.** The app contains the lifted game code, so it is built by
   each user from their own executable and sideloaded (Xcode with a free
   account lasts 7 days; a paid account or AltStore longer). It cannot go on
   the App Store and a built `.ipa` must not be published. Keep the repository
   free of generated code and game files, as now.

## Work breakdown

**iOS-0: feasibility on the Mac (half a day).** Copy the repository to a Mac,
run `recomp/lift` there, compile one lifted chunk and `runtime/*.c` with
`clang -arch arm64 -isysroot $(xcrun --sdk iphoneos --show-sdk-path)` and fix
what breaks: GNU-isms (`MAP_FIXED_NOREPLACE`, `sched_setaffinity`, `pthread`
extras, `<sys/prctl.h>`, `epoll`/`poll` differences, `accept4`, `O_CLOEXEC`
users), Mach-O differences (section attributes, symbol visibility, huge
tables), and clang versus gcc warnings turned errors. Also build the same on
macOS to run the game natively on the Mac first; it is the cheapest debugging
target, and `-DRT_DYNAMIC_BASE` can be tested there.

**iOS-1: macOS build.** `recomp/build.sh macos` (arm64, SDL2 from Homebrew or a
vendored xcframework). Acceptance: self-test (`test/run.sh`) passes natively,
and the game reaches the login screen and a lobby against the LAN server with
`run.sh`. Report compile time per chunk (clang `-O1` over about 110 chunks).

**iOS-2: iOS app target.** A CMake or XcodeGen project (`recomp/ios/`) with the
lifted chunks, the runtime, SDL2 as an xcframework, `Info.plist` (landscape,
file sharing, local network usage, `UIRequiresFullScreen`), an app entry that
calls the existing `main` after setting the paths. `recomp/ios/build_ios.sh
--exe GunBound.gme` lifts, builds and produces an `.app`; install with
`xcrun devicectl` or Xcode. Acceptance: boots to the World List on a physical
iPhone or iPad and on the simulator, with the data folder imported.

**iOS-3: input and keyboard.** Touch, on-screen keyboard for text fields,
controllers, overlay. Acceptance: log in, create a room with a typed title and
chat, all by touch.

**iOS-4: ordering and play.** Implement 3(a) or 3(b) above; stress test; a full
two-player battle against the reference server (with the packet fixes from
colemancda/GunBound#18) with the second client on the Retroid or the OptiPlex.

**iOS-5: speed.** Profile on the device (Instruments, or the runtime's
`--profile`). If the software rasteriser dominates, move to the GLES renderer
(`PLAN_GLES_RENDERER.md`, phase 5); note that Apple deprecates OpenGL ES but it
still works, and ANGLE over Metal is the fallback if a future iOS drops it.

## Test matrix

- macOS arm64 (native), iOS simulator (arm64), a physical iPhone and iPad.
- `test/run.sh`, `test/frames.sh` (host versus device frames, byte-identical
  with the soft renderer), `test/pad.sh`, `test/online.sh`.
- A 30-minute soak battle with ordering option (a) or (b) and `--guest-cpu any`
  equivalent behaviour; any hang or stale-data symptom fails the milestone.

## Risks

- Fixed 4 GiB base unavailable; dynamic base costs speed in the blitters.
- Memory limits kill the app (jetsam); check peak resident memory early.
- No CPU pinning: the ordering work in item 3 is mandatory, not optional.
- Compile time of the lifted code with clang on a phone-sized project; use
  chunking and `-O1`, optionally a static library built once per executable.
- Local network permission and background socket loss behave differently from
  Linux; test on a device early.

## Do not

Publish a built app or any lifted code, commit game files or tokens, change the
Linux behaviour (keep `--guest-cpu auto` there), or push without asking.
