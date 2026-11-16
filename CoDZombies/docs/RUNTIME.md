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
| `s3e.c` | the s3e functions identified so far |
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
`hdr + hdr[0x2c]`) holds 249 settings for iPhone. A `{OS=…}`/`{ID=…}` block
ends at `{}` or at the next `[GROUP]` header. Group names and keys are
case-insensitive, and the last definition wins. Values may be expressions
such as `[GAME]ExtraLuaMemorySize + 400000`.

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
- Code still found only at run time is recorded in `extra_funcs.tsv` and lifted
  next time (`tools/discover.sh`). Four starts so far, all Thumb code entered
  from ARM.

## Imports identified

| impl | function | notes |
|---|---|---|
| `0x2e718` | s3eMemorySetUserMemMgr | the game installs its own malloc/realloc/free (`0x25511d`, `0x255449`, `0x254cf9`) |
| `0x2ed9c` | s3eMallocBase | guest heap |
| `0x2f16c` | s3eReallocBase | |
| `0x2e098` | s3eFreeBase | |
| `0x19b10` | s3eConfigGetString | 128-byte result |
| `0x19cb0` | s3eConfigGetInt | |
| `0x277a0` | s3eFileOpen | `0x271f8(name, mode, 0)` |
| `0x277a8` | s3eFileCheckExists | |
| `0x24ef8` | s3eFileOpenFromMemory | |
| `0x25f30` | s3eFileRead | |
| `0x25de4` | s3eFileGetChar | −1 at the end |
| `0x27cdc` | s3eFileSeek | |
| `0x24db4` | s3eFileTell | |
| `0x254e8` | s3eFileClose | |
| `0x3ae20` | s3eTimerGetMs | 64-bit |
| `0x3a974` | s3eTimerGetUTC | 64-bit; the loader uses `gettimeofday` |
| `0x1ed28` | s3eDeviceGetInt | iPhone values from the loader's table at `0x457b4`: OS = 14, 8 → 5, 10 → 100, 16 → 1, many −1 |
| `0x21c98` | s3eExtGet | every extension unavailable |
| `0x170fc`, `0x171ec` | callback registration | the generic registry `0x16e3c(device, kind, id, fn, userdata)` for devices 13 and 2 |
| `0x1e068` | s3eDebugErrorShow | logged; continues |
| `0x39c3c` | s3eSurfacePtr | 320×480 RGB565 |
| `0x39c6c` | s3eSurfaceSetup | pixel type `0x422` |

Error and callback device numbers seen so far: Debug 7, Memory 9, Timer 14.

## Where it stands

From the start routine, the game runs:

1. its static constructors and memory setup;
2. its memory buckets (365 allocations), sized from config;
3. callback registration, surface setup and extension checks;
4. opening `_main.dz` and reading its directory.

It then fails to find `console.bin` ("Did you include this file in your mkb's
assets block?") and goes on deserialising from a NULL file, so the read loop
runs on a length taken from uninitialised stack. Next:

- Follow the derbh archive layer, which is the game's own code over
  s3eFileOpen/Read/GetChar: why the lookup fails after reading the directory,
  and whether the archive data needs decompression imports.
- Identify the imports reached so far but not yet named: s3eSound `0x398bc`
  (the "Divide by zero" errors are probably its 0 returns), `0x45b54`,
  `0x29b54`, `0x2aafc`, and `0x1deb0` (a compiled-out debug check returning 0).
- Then the main loop: s3eDeviceYield, callbacks into the game through
  `guest_call`, GL and the window.
