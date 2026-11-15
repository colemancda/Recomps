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
`ntohl`/`ntohs`, `CoInitialize`, `OpenProcessToken`. The boot currently stops at
the next unimplemented import (`advapi32!LookupPrivilegeValueA`); from here it is
the usual step-2 work of filling import handlers one blocker at a time.

Added so far: `ntohl`/`ntohs` (Winsock), the GameGuard/ijl15/WzMss stubs, and the
MapleStory module list. The engine DLLs (Canvas, Gr2D_DX8, Sound_DX8, …) are not
lifted yet; they are added as the boot reaches their COM activation.
