# The runtime

`runtime/` stands in for Marmalade's iOS loader: it maps guest memory, loads
and decrypts the image, runs the lifted game code, and answers the game's
imports natively. The host build (`./build.sh host` → `build/codz-host`) runs
headless on this Mac for bring-up; the iOS app comes later.

```
./build.sh host                        # lift if needed, compile, link build/codz-host
cd build && ./codz-host ../orig/Payload/kiwi.app
tools/discover.sh                      # run, re-lift with newly reached functions, repeat
```

Environment: `RT_LOG_IMPORTS=1` logs each import call with any string
arguments (stopping after `RT_LOG_LIMIT` lines, default 200000);
`RT_MISSING=zero` makes unimplemented imports return 0 instead of stopping;
`RT_EXTRA_FUNCS=path` records code addresses the game reaches that weren't
lifted (`lift/lift.py --extra` reads them); `CODZ_SAVE` is the writable
directory (default `save/`).

## Pieces

| file | what |
|---|---|
| `main.c` | maps memory, loads the image, reads the config, calls the start routine `0x65f00` on an 8 MB guest stack |
| `mem.c` | 4 GB guest reservation; the guest heap (size-class free lists in `0x10000000`–`0x30000000`) |
| `loader.c` | copies the executable's segments, decrypts the game's data, builds the function table |
| `config.c` | the embedded `.icf`: groups, conditional blocks, integer expressions |
| `file.c` | guest paths (`rom://`, `ram://`, relative) to the bundle and the save directory |
| `dispatch.c` | `rt_import` (stub → handler), indirect calls to unlifted code, return mismatches, `guest_call` for callbacks, diagnostics |
| `s3e.c` | the s3e functions identified so far, including files and user file systems |
| `compress.c` | s3eCompression's streaming decompressor: LZMA (liblzma) and zlib/gzip/deflate |
| `gl.c` | OpenGL ES by name (none yet) |

## Things the game expects from the loader

**Encrypted data.** The s3e image's initialized data (`0x5c88` bytes at
`0xa149e8`) is encrypted in the executable. Marmalade's loader decrypts it
before starting the game (`0x9198`, called from `0x613c`). The key is the low
16 bits of a hash, `h = 1234; h = h * 31 + byte`, over every byte from `__text`
(`0x2040`) to the end of `s3e_code` (`0x2c45e0`), computed by `0x8fc4`. That
routine also adds any time over 10 ms, an anti-debugging check that corrupts
the key under a debugger. Each byte is then
`out = in ^ (key >> 8); key = (key + in) * 0xce6d + 0x58bf`. The range comes
from the `XE3U` header: start `hdr + hdr[0x2c] + hdr[0x30] + hdr[0x3c]`,
length `hdr[0x18] − hdr[0x44]`. Without this, the game's allocator table at
`0xa19e24` is garbage. With it, that table is `0x65fc4, 0x65fcc, 0x65fc0`,
branch-table slots for s3eMallocBase, s3eReallocBase and s3eFreeBase.

**Config.** The `.icf` text after the header (`hdr[0x30]` bytes at
`hdr + hdr[0x2c]`) holds 154 settings for iPhone. A condition such as
`{OS=…}` or `{ID=…}` holds until the next condition or `{}`, and group headers
inside it belong to it. `{ID=ANY}` and `{CLASS=ANY}` hold, and so does
`{OS=IPHONE}`. `{[GROUP] Key==value}` (or `!=`) compares a setting defined
earlier, for example `{[GX] Rasterisation==SW}`, which must not match:
Rasterisation is `HW`. Group names and keys are case-insensitive, and the last
definition wins. Values may be expressions such as
`[GAME]ExtraLuaMemorySize + 400000`. `RT_LOG_CONFIG=1` logs each lookup with
its result.

**Asset archives.** The game reads its `.dz` (derbh, magic `DTRZ`) archives
itself and mounts them with **s3eFileAddUserFileSys**, a struct of game
callbacks:

| offset | callback |
|---|---|
| 0x00 | open(name, mode) → handle |
| 0x04 | read(buf, size, count, handle) |
| 0x08 | eof(handle) |
| 0x0c | seek(handle, offset, origin) |
| 0x10 | tell(handle) |
| 0x14 | close(handle) |
| 0x18 | list a directory → listing |
| 0x1c | next directory entry |
| 0x20 | close a listing |
| 0x24, 0x28 | optional, unused |

The first nine are required. The game itself shows the layout: open loops over
the mounted archives; eof tests the handle's state; tell returns
`handle[0]`; close unlinks the handle; slot 0x18 builds a 0x88-byte listing
from a directory name. s3eFileOpen and s3eFileCheckExists try the user file
systems first. There's no exists callback, so a file exists if it opens. A
handle from a user file system forwards read, getc, seek, tell, size and close
to the callbacks through `guest_call`.

**Compression.** Files in the archives are LZMA streams in the "LZMA alone"
format (properties `0x5d`, 64 KB dictionary, 64-bit unpacked size), unpacked
through **s3eCompressionDecompInit(alg 0, readFn, userData)** /
**DecompRead(handle, buf, &len)** / **DecompFinal**. The loader pulls
input with the game's `readFn({buf, len}, userData)`. The host build uses
Homebrew's liblzma; iOS will need it built for the device or a vendored
decoder.

**Stack.** The loader runs the entry on a fibre whose stack size comes from
config (default `0x8000`). We use one 8 MB stack.

## Lifter changes the runtime needed

Bringing the game up exposed control flow the static passes had missed:

- **Thumb-1 returns through a register** (`pop {r3}; bx r3`). Every lifted
  function keeps the lr it was called with in `lr0`, and an indirect jump to
  `lr0` is a return.
- **`mov lr, pc; bx rX`** is a call that falls through. The body walk now
  continues past it.
- **PIC and interworking veneers** (`ldr r3, [pc, #4]; add r3, pc, r3; bx r3`,
  `add ip, pc, #1; bx ip`, and 854 `bx ip` long-branch veneers) are resolved by
  a constant tracker in `analyze.py`. Their targets become function starts and
  direct tail calls.
- **The C++ static constructor list** at `0x2ba410`–`0x2ba6ac` holds
  self-relative offsets (entry + word = function). The survey accepts runs of
  self-relative code pointers (eight or more, or three or more mostly starting
  with `push`) after the main walk, skipping anything the walk already decoded,
  so a wrongly moded entry can't shadow real code. One such entry,
  `0xac35c`, had been taken for Thumb when it's ARM.
- **Lone ARM pointers to `push {…, lr}`** count as function starts, like
  Thumb ones already did.
- **Modes at function boundaries.** A start at the same address in the other
  mode (one of them a misread pointer) no longer ends a body walk or turns a
  branch into a tail call. The function table now records each function's
  mode, and `rt_call`/`rt_jmp` treat a mode mismatch as a miss: that's how
  `0x1e020c` (ARM, taken for Thumb from a table) showed up. A mode recorded at
  run time overrides the survey's.
- **The Thumb switch helper** `0x25a71c` (ARM, reached with `blx` from 26
  Thumb sites) takes its index in r3, reads a byte count just before the
  return address and a byte table after it, and jumps to
  `lr + 2 * table[min(r3, count)]`. The analyzer reads the table and the
  lifter emits the helper's register effects (r3, ip, lr and the flags of its
  `cmp`) followed by a `switch`. Before this, the return landed in the table
  and was recorded as a function start.
- **Computed jumps into straight-line code.** `add pc, pc, rX, lsl #2` with no
  branch table after it enters an unrolled loop part way (the division
  routines at `0x665cc`). Every instruction of the run after it is a possible
  target. Two bogus starts inside that loop came out of `extra_funcs.tsv`.
- Code still found only at run time is recorded in `extra_funcs.tsv` and lifted
  next time (`tools/discover.sh`). Nine starts so far.

## Imports identified

| impl | function | notes |
|---|---|---|
| `0x2e718` | s3eMemorySetUserMemMgr | the game installs its own malloc/realloc/free (`0x25511d`, `0x255449`, `0x254cf9`) |
| `0x2ed9c` | s3eMallocBase | guest heap |
| `0x2f16c` | s3eReallocBase | |
| `0x2e098` | s3eFreeBase | |
| `0x19b10` | s3eConfigGetString | 128-byte result |
| `0x19cb0` | s3eConfigGetInt | |
| `0x277a0` | s3eFileOpen | `0x271f8(name, mode, 0)`; user file systems first |
| `0x277a8` | s3eFileCheckExists | |
| `0x24ef8` | s3eFileOpenFromMemory | |
| `0x25f30` | s3eFileRead | |
| `0x259d0` | s3eFileWrite | host files only |
| `0x25de4` | s3eFileGetChar | −1 at the end |
| `0x27cdc` | s3eFileSeek | |
| `0x24db4` | s3eFileTell | |
| `0x26488` | s3eFileGetSize | the game reads a file only if its size is positive |
| `0x254e8` | s3eFileClose | |
| `0x285b8` | s3eFileCheckDirectoryExists | via `0x2815c` |
| `0x27fb0` | s3eFileMakeDirectory | the game makes `data-iphone` one component at a time |
| `0x27120` | s3eFileDelete | |
| `0x29b54` | s3eFileAddUserFileSys | see "Asset archives" |
| `0x189b0`, `0x18638`, `0x18424` | s3eCompressionDecompInit / Read / Final | see "Compression" |
| `0x1f068` | s3eDeviceYield | clamps the ms argument, then runs the event loop; returns at once for now |
| `0x398bc` | s3eSoundGetInt | volume 256, rates 22050, 16 channels, not available; a 0 rate made the game divide by zero |
| `0x3a964` | timer function | 1 for 0, else a Timer error and −1 (`0x4fda0`) |
| `0x1dea8`, `0x1deb0` | compiled-out debug checks | 0 |
| `0x3ae20` | s3eTimerGetMs | 64-bit |
| `0x3a974` | s3eTimerGetUTC | 64-bit; the loader uses `gettimeofday` |
| `0x1ed28` | s3eDeviceGetInt | iPhone values from the loader's table at `0x457b4`: OS = 14, 8 → 5, 10 → 100, 16 → 1, many −1 |
| `0x21c98` | s3eExtGet | every extension unavailable |
| `0x170fc`, `0x171ec`, `0x2fe2c`, `0x45b54` | callback registration | the generic registry `0x16e3c`/`0x17054(device, kind, id, fn, userdata)` for devices 13, 2, 6 (the pointer range) and 24 |
| `0x1e068` | s3eDebugErrorShow | logged; continues |
| `0x39c3c` | s3eSurfacePtr | 320×480 RGB565 |
| `0x39c6c` | s3eSurfaceSetup | pixel type `0x422` |
| `0x2aafc` | s3eGLGetInt | 0: GL version `0x101`, 1: EGL version `0x103`; IwGx takes the hardware path when the GL version is above `0xff` |
| `0x2ab4c` | s3eGLGetNativeWindow | `0xa` |
| `0x2aaf4` | s3eGL (nothing on iPhone) | 0 |
| `0x47e40` | glGetString | the loader's wrapper |
| `0x460c8`–`0x46900` | EGL 1.2 | see "EGL and GL" |

Error and callback device numbers seen so far: Debug 7, Memory 9, Timer 14.

## EGL and GL

The loader has no real EGL. Its "Ideaworks3D (dummy iPhone impl)" hands out
fixed handles over EAGL: native window `0xa`, display `0xb`, surface `0xc`,
config `0xd`. The game reaches it through 34 stubs at `0x2ba6b0` (and a copy at
`0x2bc168`) in alphabetical EGL 1.2 order, from eglBindAPI to eglWaitNative.
The order checks out against the dummies: index 4 makes the EAGL context,
16 takes the native window and returns the display, 18 compares names,
28 checks display and surface before presenting. `runtime/gl.c` answers with
the same handles (and context `0xe`).

The GL stubs are named from the executable's indirect symbols. It imports the
GLES 2 framebuffer names (`glBindFramebuffer`); the game asks
`eglGetProcAddress` for the OES ones, which get the stub of the name without
`OES`.

The host build has a null GL backend. Queries get the original iPhone's
answers (PowerVR MBX, OpenGL ES-CM 1.1, its extension list, PVRTC formats,
1024 textures, 5/6/5 colour, 24-bit depth), names count up, shader and program
status is always success, and everything else does nothing. `RT_LOG_GL=1` logs
the first call of each function that does nothing. `dispatch.c` looks each GL
stub up once and keeps the answer.

## Where it stands

From the start routine, the game runs:

1. its static constructors and memory setup;
2. its memory buckets, sized from config;
3. callback registration (device events, pointer and others), surface setup
   and extension checks;
4. mounting `_main.dz` and loading `console.bin`, the localisation tables
   (`localisation/english_uk.dat`, `LocaliseHashes.dat`) and
   `bootstrap/bootstrap.group.bin`, all LZMA-compressed;
5. writing its save files (`data-iphone/savedata1.i3d`, `firstRunSink.dat`,
   `shipSink.dat`) into the save directory;
6. GL setup through EGL, then `splash/splash.group.bin` with its textures;
7. `fixed/fixed.group.bin`, the five achievement groups, the map archives
   (`_nacht.dz` … `_derriese_full.dz`), `frontend/frontend.group.bin` and
   `frontend/mapInfo/mapInfo.group.bin`;
8. its main loop, using about 40 distinct GL functions (matrices, client arrays,
   `glDrawElements`) and `eglSwapBuffers`. A two-minute run swaps 4800 frames
   without stopping.

Still missing and returning 0: s3eAccelerometer `0x1161c`, s3eDebug `0x1ece0`,
`0x1fd7c`, s3eKeyboard `0x2ba58`, `0x2bae8`, `0x2bd38`, s3eMemory `0x2f7a8`,
`0x2f838`, `0x2fdb8`, s3eSocket `0x30d88`, s3eSound `0x34a20`, s3eTimer
`0x3ac5c` and `0x3b064`, s3eVideo `0x3ba74`.

Next:

- **Rendering.** A GL backend that draws: GLES 1.1 through EAGL on iOS, and
  for the host an offscreen desktop GL context (fixed-point `x` calls
  converted to float) that can save frames, so the frontend can be checked.
  Client-array pointers are guest addresses and become host pointers through
  `G()`; with a buffer bound they stay offsets.
- **Events and time.** Deliver device, pointer and timer callbacks from
  s3eDeviceYield, and pace frames.
- **The missing imports above.**
