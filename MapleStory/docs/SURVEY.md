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

Defeating this obfuscated layer — identifying every anti-hack entry point and
either neutralising the checks or emulating the GameGuard responses the client
expects — is the next focused effort. Until then the recompiled client boots
through its entire CRT and engine initialisation and stops at the scanner, which
is a clean, reproducible checkpoint.
