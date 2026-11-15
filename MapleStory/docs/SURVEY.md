# MapleStory v62 recompilation — survey

Findings while bringing up the static recompilation. The method and most of the
runtime come from the Age of Mythology recompilation in this repository.

## The binary

Three forms of the v62 client ship in `orig/` (see `docs/PLAN.md` for the full
table). The retail `MapleStory.exe` is Themida-packed and keeps functions as VM
"stolen code"; it cannot be lifted. The lift target is **`GMSv62.exe`**, the same
client unpacked with a rebuilt import table.

Verification: `tools/dump_unpacked.py` runs `MapleStory.exe` under Wine and reads
its image from `/proc/PID/mem` after Themida unpacks it. The dumped image's entry
point and 99.62% of its code section are byte-identical to `GMSv62.exe`; the
difference is exactly the Themida VM stubs and import thunks (where `GMSv62.exe`
holds the real code). So lifting `GMSv62.exe` lifts the original client's code.

## Imports

`GMSv62.exe` imports 265 symbols. The CRT is statically linked (no msvcrt
import), which is simpler than AoM. Beyond the standard Win32 set it imports:

- `dinput8.dll` (DirectInput 8), `ws2_32.dll` (Winsock), `wininet.dll` (the
  launcher's web page), `netapi32`, `iphlpapi`.
- `npkcrypt.dll` (GameGuard / nProtect KeyCrypt): seven `NPK*` functions,
  stubbed in `runtime/maple.c` to report success.
- `ijl15.dll` (Intel JPEG Library): `ijlInit/ijlFree/ijlWrite`, stubbed until
  lifted or reimplemented.
- `wzmss.dll`: seven `WzSoap_*` bulletin-board functions, stubbed.

## The function list

Ghidra's headless export (`tools/ghidra/ExportFunctionsCsv.java`) recovers 68,661
functions. A baseline lift with no list recovers 25,828 and covers ~43% of the
code section.

MSVC C++ keeps function pointers in `.data`/`.rdata` tables — the CRT
static-initialiser array and the engine vtables — whose entries are only called
indirectly. Ghidra leaves 833 of these unrecovered, and the client faults in
`_initterm` at the first static constructor (an init thunk at `0x427121`, a
pointer in `.data` at `0x963088`). `tools/data_code_pointers.py` scans the data
sections for aligned words that point into `.text` with a plausible prologue and
appends the missing ones to the function list. `build.sh` regenerates this merge
automatically. With it, 74,684 functions are lifted.

## Boot state (step 2, in progress)

`build/host/maple --headless` runs the entry point and the CRT startup, then the
C++ static initialisers, as real lifted code. Reached so far, each a Win32 call
that succeeds: `WSAStartup`, file enumeration of the game folder
(`FindFirstFile`/`FindNextFile`), resource loading, a mutex, `inet_addr`,
`ntohl`/`ntohs`, `CoInitialize`, `OpenProcessToken`. Filling imports one blocker
at a time (privileges, CryptoAPI, COM error info, dialogs, `FindWindow`) carried
the boot through CRT start-up and into the engine load; see the sections below
for the engine DLLs and the GameGuard frontier where it now stops.

Imports added in `runtime/`: `ntohl`/`ntohs` (Winsock); token privileges, the
CryptoAPI set and COM error info (stubs.c); `DialogBoxParam`/`FindWindow`
(user32.c); and the GameGuard/ijl15/WzMss stubs and MapleStory module list.

## Engine DLLs and msvcrt (step 1 continued)

The client loads the Wizet engine DLLs at start-up. All are now lifted and linked
(bases are each DLL's own ImageBase, non-conflicting): PCOM, NameSpace, ResMan,
Shape2D, Canvas, Gr2D_DX8, Sound_DX8, and ijl15. Ghidra exports range from 276
(ResMan) to 1431 (Sound_DX8) functions each. Unlike the EXE, these DLLs link the
dynamic `msvcrt.dll` (41 distinct imports: malloc/free, memcpy, sprintf, the math
functions, C++ exception handling and `_initterm`). `msvcrt.dll` (the VC6 system
runtime, the same build Age of Mythology uses) is lifted too, at 0x78000000; it
is a Microsoft system DLL, kept in the ignored `orig/`. `build.sh` now applies
the data-section code-pointer merge to every module, not just the EXE.

With these, the client loads and initialises all engine DLLs and runs their CRT
start-up, then reaches GameGuard.

## GameGuard (the current frontier)

Start-up runs into nProtect GameGuard's anti-hack layer, woven into the client's
C++ static initialisers (not just the stubbed `npkcrypt` imports):

- **0x672c93** — a scanner run from a static constructor. It enumerates files
  with `FindFirstFileA("*")`, decrypts a blocklist of cheat-tool names (encrypted
  bytes at 0x96a0f0, 0x96a104, 0x96a118, … decoded by `FUN_00865f61`), and
  compares each file with `lstrcmpiA`; on a match it shows a Korean "close the
  hacking program and restart" MessageBox (caption "Error Message") and calls
  `ExitProcess`.
- Past the scanner, GameGuard resolves Win32 APIs indirectly into a pointer table
  around **0x97b384** (no direct `mov [0x97b384], reg` writes exist — the address
  is computed into a register and stored through it) and calls through those
  slots. Hooking the scanner alone is not enough: the table must be populated or
  each call site handled.

**Resolved.** The scanner is not obfuscated anti-cheat but a **DLL-planting
check**: it enumerates the game folder (`FindFirstFileA "*"`) and, for each file,
compares the name against a whitelist of ~16 Windows system DLLs (kernel32,
user32, ws2_32, msvcrt, d3d8, …). A match means a system DLL has been *planted*
in the game folder (a DLL-search-order hijack), so it errors. A real install has
none of these in its folder and passes. The recompilation only failed because the
build copies **`msvcrt.dll`** into the game folder to lift it, and `msvcrt.dll` is
on the whitelist. The fix: keep lifted system DLLs in a `syslib/` subfolder that
the top-level scan does not enumerate, which the loader also searches
(`runtime/loader.c`); `build.sh` reads `orig/syslib/msvcrt.dll`.

Past the scanner, the client resolves many ordinary Win32 APIs with
`GetProcAddress` and calls through the stored pointers (the "table around
0x97b384" is this, not GameGuard indirection). A slot is null, and the call
faults, when the runtime does not yet implement that API. These are filled one at
a time, the usual step-2 work; the GameGuard anti-hack layer itself is passed.

## Boot to the login network stage (step 2 complete)

Past GameGuard, filling the Win32 functions the client resolves with
`GetProcAddress` (a null slot faults when called) carried the boot to completion.
Added in `runtime/extra.c`: RECT maths (CopyRect/InflateRect/UnionRect), file
mapping (CreateFileMapping/MapViewOfFile), OpenProcess, the I/O completion port
stubs, window hooks, DrawText, OLE init, SHGetSpecialFolderPathA and the IMM
context functions.

Two lifter fixes were needed:
- **C++ exception handlers.** MSVC embeds the exception FuncInfo/handler tables in
  `.text`, and their catch/cleanup funclets can start with a bare `ret`. The
  client jumps to one (0x405a1c) through the EH dispatch, and it was not a lifted
  entry. `tools/data_code_pointers.py` now scans `.text` too (accepting prologues
  or `ret`/`ret n`), so these funclets are lifted.
- **Far-pointer load.** A spurious entry decoded as a far `jmp [mem]`, emitting
  `ld48`; it is defined in `cpu.h` to return the 32-bit offset (the selector is
  ignored in the flat model).

With these, the recompiled client **boots completely**: it runs all CRT and
engine initialisation, passes GameGuard, **creates its main window**
(`RegisterClassExA`/`CreateWindowExA`/`ShowWindow` + a `PeekMessageA` loop), and
**connects to the login server at 127.0.0.1:8484** (`socket`/`connect`, with
`WSAAsyncSelect` notifications). It then waits for the server's handshake, as a
MapleStory client does — the server sends the v62 `HelloPacket` (version + send
and receive IVs) first. With nothing listening it retries and exits.

This is the handoff to a v62 server. `colemancda/maplestory`'s
`MapleStoryServer62` is exactly that: it speaks the v62 protocol and the matching
cipher. Pairing the recompiled client with it (client → `MapleStoryServer62` on
127.0.0.1:8484) is the next milestone. The client also resolves `WSASend`/
`WSARecv` (overlapped I/O) which are not yet provided; those are the remaining
Winsock pieces for the receive path.

## The login handshake works (step 2 → step 6)

With `WSASend`/`WSARecv` added (`runtime/winsock.c`), the recompiled client was
driven against a minimal mock server that sends the v62 handshake — the exact
bytes from the Swift repo's test vector (`Tests/MapleStory62Tests/LoginTests`):

    0D 00 3E 00 00 00  46 72 7A 18  52 30 78 14  08
    opcode/len 0x0D | version 62 | value0 0 | recvIV | sendIV | region 8 (global)

On receiving it, the client **stops retrying, sets up its receive cipher from the
IVs, and initialises Direct3D 8** (the GL backend reports "drawing with … OpenGL
ES 3.2, 800x600") and DirectInput — i.e. it accepts the handshake and begins the
login scene. This validates the recompiled client's network receive path and the
v62 protocol end to end.

The client then exits before presenting the login screen. It uses
`WSAAsyncSelect` (message-based async sockets): it expects a window message when
the socket connects / has data, and drives its login state machine from that.
Headless (no real window with a running message pump) the notification does not
advance the state machine, so after reading the handshake it stalls and exits.
The next steps: run under a real window (Xvfb) so the message loop delivers the
socket notifications, confirm the login UI loads from `UI.wz` (ijl15, now lifted,
decodes the JPEGs — its boot-time stubs were removed), and then pair with
`colemancda/maplestory`'s `MapleStoryServer62` for the full login. (The Swift
server has its own Linux-portability issues — `XMLParser`/`FoundationXML` and a
socket `featureUnsupported` error — separate from the recompilation.)

## Login screen: the real blocker is the lifted Wz resource engine, not the network

Running under a real window on Xvfb (`DISPLAY=:99`, so SDL opens an X11 window
with a running message pump) ruled out the message-pump theory above: the client
still exits right after Direct3D init, without presenting the login screen. A
backtrace at the exit (see the `RECOMP_TRACE_EXIT` instrumentation below) shows a
**clean** teardown — `PostQuitMessage(0)` -> `WM_QUIT` -> `WinMain` returns ->
CRT `exit(0)` — not a crash. The network path is fine: the WSAAsyncSelect
FD_CONNECT/FD_READ window messages *are* delivered and pumped (the client calls
`recv` and reads all 15 handshake bytes, which in that model only happens after
those messages arrive).

Reference check against the genuine client: `GMSv62.exe` run under Wine 10
against the same mock handshake server **stays alive at an idle login screen**
(killed only by a 40 s timeout, `WINE_EXIT=124`) and opens ~20 resource files
(List.wz, MapleStoryUS.ini, LV0.INI, String.wz, Character.wz, UI.wz, ...). So the
handshake alone is enough to render the login screen — the divergence is ours.

Root cause: the exit is triggered by a caught C++ exception during login/input
setup (`FUN_0052625f`, which does CreateEventA x3 + DirectInput8Create + reads
resource properties). Decoding the throws (MSVC `__CxxThrowException`, code
0xE06D7363):

  - `ZException`               — benign: `C:\WINDOWS\system32\msexcr.ini` absent.
  - `_com_error (hr=80070057)` — E_INVALIDARG, caught and recovered.
  - `_com_error (hr=80030002)` — **STG_E_FILENOTFOUND**, the fatal one. A
    `#import` COM smart-pointer wrapper turned a failed HRESULT from a Wz
    resource/property call into a throw; the top-level handler responds by
    shutting the client down cleanly.

Our client opens **only Base.wz** (the ~6.5 KB root index) and then a child
lookup in the lifted resource engine returns STG_E_FILENOTFOUND *before any
sub-`.wz` is opened* — i.e. the lifted ResMan/PCOM/NameSpace (PCOM.dll @
0x50c00000, ResMan.dll @ 0x51000000, NameSpace.dll @ 0x50800000) fail to mount /
navigate the Wz namespace, where the real DLLs mount the whole `.wz` set. This is
a correctness bug inside the lifted engine DLLs (or a runtime primitive they rely
on), not a missing import stub. Next step is to debug the Wz-mount path in the
lifted ResMan against the real DLL's behaviour under Wine (compare the file-open
sequence and the namespace it builds).

Debug instrumentation added for this (all cost-free unless enabled):
  - `RECOMP_TRACE_EXIT=1` -> guest backtrace at `ExitProcess` and `PostQuitMessage`.
  - `--trace-imports` now decodes each MSVC C++ throw's catchable type names, and
    prints the HRESULT for a thrown `_com_error` (`runtime/seh.c`).
