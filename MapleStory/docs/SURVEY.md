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

## The client renders: Nexon + Wizet splash screens (login pipeline working)

Four fixes took the client from the Base.wz STG_E_FILENOTFOUND failure all the
way to rendering real game graphics:

1. **File-backed memory mapping** (`runtime/file.c`, `runtime/mem.c`). The Wz
   resource engine (NameSpace.dll) memory-maps Base.wz and every other `.wz` and
   parses them in place. The old mapping stub returned a zeroed heap buffer, so
   the engine read zeroes and every resource lookup failed. Now a real file
   mapping: `CreateFileMappingA`/`MapViewOfFile` map the file's bytes, demand-paged
   via `rt_map_file` (mmap MAP_FIXED into a 2 GiB arena at guest 0x80000000) so
   the 1+ GiB of `.wz` data costs no heap. With this the client mounts all 17
   `.wz` files, exactly like the retail client under Wine.

2. **Lifted ZLZ.dll** (the Wz canvas zlib decompressor). Canvas.dll calls
   `ZLZCreateInflator` to inflate each image out of the Wz data; it was an
   unimplemented dynamic import. Lifted like the other engine DLLs (own base
   0x10000000) and registered so LoadLibraryA finds it.

3. **oleaut32 `CreateErrorInfo` (ord 202) and `VariantCopyInd` (ord 11)**. The
   engine's COM error paths need CreateErrorInfo; property handling needs
   VariantCopyInd (VT_BYREF dereference).

4. **wininet updater stubs**. The auto-updater's FTP calls report the patch
   server unreachable, so it skips the update and proceeds (as the retail client
   does when the server is down).

Result: `exit 0`, no crashes, **784 frames presented** through the full
D3D8-on-GLES 2D pipeline (BeginScene/Clear/DrawPrimitiveUP/EndScene/Present).
The **Nexon** and **Wizet** splash logos render pixel-correct (decompressed from
the `.wz` via the lifted ijl15 JPEG and ZLZ zlib decoders).

### Next blocker: GameGuard KeyCrypt init (after the splash, before the title)

After the two splash logos, while transitioning to the title/login screen, the
client throws a C++ `CSecurityKeyCryptInitFailed` exception, caught by the
top-level handler which shuts down cleanly. It is thrown from `FUN_00867060`
immediately after `npkcrypt.dll!NPKGetAppCompatFlag` / `NPKSetAppCompatFlag`
(both stubbed to return 0 in `runtime/maple.c`): GameGuard's KeyCrypt (nProtect
packet-crypto) initialisation check fails. v62 GMS encrypts packets with its own
Maple AES/Shanda cipher, not GameGuard KeyCrypt, so the fix is to make the
npkcrypt init path report success the way `FUN_00867060` expects (needs reading
that function's check), rather than to implement real KeyCrypt. This is the next
step toward the interactive login screen.

Debugging aids used: `--trace-imports` now prints each MSVC C++ throw's type name
and (for `_com_error`) HRESULT; `RECOMP_TRACE_EXIT=1` dumps a guest backtrace at
`ExitProcess`/`PostQuitMessage`. Run the client windowed with
`DISPLAY=:99 ./build/host/maple --exe orig/GMSv62.exe --gamedir orig
--savedir <dir> --frames N --dump-every K --dump-path f_%d.png` against a mock
server that sends the v62 handshake (Xvfb for the window + message pump).

## MILESTONE: the interactive login screen renders

Past the splash screens, three more fixes reached the actual MapleStory login
screen (logo, forest background, Login ID / Password fields, Log in button):

5. **GameGuard KeyCrypt init** (`runtime/maple.c`). `FUN_00867060` stores
   `NPKOpenDriver`'s return in the KeyCrypt slot and throws
   `CSecurityKeyCryptInitFailed` if it is <= 0, and treats
   `NPKGetAppCompatFlag == -1` as "compatible". So NPKOpenDriver reports a
   positive handle and NPKGetAppCompatFlag returns -1.

6. **npkcrypt stdcall arg counts** (`runtime/maple.c`). NPKOpenDriver takes 0
   args, NPKGetAppCompatFlag 1, NPKSetAppCompatFlag 2, NPKRegisterCryptWindowMsg
   3 (per the call sites). They are `__stdcall`; the previously wrong counts
   corrupted the guest stack *once the KeyCrypt init success path ran them*
   (before, the throw aborted first), yielding a garbage object pointer and a
   SIGSEGV during title-screen setup.

7. **VariantChangeType from VT_EMPTY / VT_NULL -> 0** (`runtime/stubs.c`). The
   title screen reads properties that are sometimes empty and converts them to an
   integer; the missing conversion returned DISP_E_TYPEMISMATCH and the caller
   used the bad result as a pointer (return to a junk address). Empty/null now
   convert to 0.

Result: **exit 0**, 2500+ frames, no crashes, and the full interactive login
screen renders (title screen after the Nexon/Wizet splash). The recompiled v62
client now boots -> connects -> v62 handshake -> mounts all .wz -> passes
GameGuard KeyCrypt -> renders the login screen.

### Next steps
- Drive input (type credentials, click Log in) and complete the login against a
  real `MapleStoryServer62`, so the handshake is followed by a login packet
  exchange. Remaining `VariantChangeType 13 -> 19` (VT_UNKNOWN -> VT_UI4) warnings
  are non-fatal but worth filling in if a later screen needs them.
- Then character selection / world entry, sound, and the aarch64 / handheld build.

## Login buttons: loaded but not drawn (lifted CLogin UI logic)

The login screen renders the signboard, the Login ID / Password fields, the Log in
button and (after the GDI TextOut fix) the "Ver. 0.62" label, but the lower
signboard buttons are missing: Register (BtNew), Homepage, Quit, Guest Login,
Save login ID, Find login ID (BtLoginIDLost), Find P/W (BtPasswdLost).

Traced end to end:
- A WZ reader (tools, GMS IV 4D 23 C7 2B) confirms every button exists in
  UI.wz/Login.img/Title with real bitmap data (datalen ~1-2 KB), normal
  dimensions and format 1 (A4R4G4B4) — the same kind as BtLogin, which renders.
  No `_inlink`/`_outlink`, so it is not a shared-bitmap resolution problem.
- RECOMP_TRACE_TEX shows the client creates all the button textures (the 128x64 /
  128x32 A4R4G4B4 textures).
- Instrumenting IDirect3DDevice8::DrawPrimitiveUP (the only draw path; no indexed
  draws) and transforming each vertex through World*View*Proj + viewport shows
  only TWO buttons are ever submitted as draws — BtLogin at screen (634,232) and
  one more at (664,222), both correctly placed in the top-right of the signboard.
  The other buttons' textures are loaded but never drawn.

So this is not resources, position, transform or clipping: the recompiled login
code only issues draws for a couple of the login buttons. The cause is in the
lifted CLogin / UIWindow child-draw (visibility) logic and needs deeper
recompiled-code debugging. The retail client under Wine (same handshake-only mock,
GameGuard bypassed with a stub npkcrypt.dll) draws them all, confirming the data
and the server interaction are sufficient — the gap is ours.

## Login input: the message pump now runs, but clicks don't focus a field

Reaching the login screen, the remaining blocker is driving input (type the ID /
password, click Log in). Traced the client's input architecture end to end:

- MapleStory does **not** read the keyboard through DirectInput (no
  `SetEventNotification` / `GetDeviceState` / `GetDeviceData` is ever called) and
  does **not** read the mouse through DirectInput either.
- Its main loop at `0x8264a7` drives everything off `MsgWaitForMultipleObjects`
  (the IAT slot `[0x97b5e4]`, resolved via `GetProcAddress`): it waits on 3 event
  handles with `dwWakeMask = QS_ALLINPUT`, and only when the call returns
  `WAIT_OBJECT_0 + nCount` (input available) does it run the Win32 pump —
  `PeekMessageA` (`[0x97b5e0]`), `TranslateMessage` (`[0x97b528]`),
  `DispatchMessageA` (`[0x97b524]`).

The runtime's `MsgWaitForMultipleObjects` stub always returned `WAIT_TIMEOUT`, so
the pump never ran and keystrokes never reached the window procedure. Fixed: it
now polls the wait handles and, when an input event is queued
(`rt_input_pending()`) and the caller asked for input, returns
`WAIT_OBJECT_0 + nCount`. With that, `PeekMessage`/`Translate`/`Dispatch` go from
~2 calls total to pumping every queued event, and a typed `WM_CHAR` reaches the
window procedure at `0x82c96c` with the correct character.

The window procedure routes a printable `WM_CHAR` (via the message pre-filter
`FUN_0081a94a`, gated on `[0x978364]`, which is set) to
`FUN_0081b1a4`, which delivers the character to the **focused input field** at
`[uiManager + 0x80]` (uiManager = `[0x9784c0]`). That field pointer is **null** —
no field is focused — so the character is dropped.

A field is focused by clicking it, and clicks do **not** focus it. The input
reaches the game correctly:

- the mouse button is seen — `GetAsyncKeyState(VK_LBUTTON)` returns pressed at the
  click frame (via the `IsKeyPressed` helper `FUN_005265f8`, confirmed in-trace);
- the cursor position is right — `GetCursorPos` with identity `ScreenToClient`,
  and the cursor renders over the field;
- the hit-test runs and **finds the field** — the per-control test `FUN_004b3b5d`
  (`PtInRect` against the control's `{0,0,w,h}` in local coords, called from the
  recursive find-control-at-point `FUN_0049e625`) returns a hit for a control of
  size `132x15` at screen `~449,240`, i.e. the Login ID field, when clicking it.

So the click is seen, positioned correctly, and hit-tests onto the ID field — yet
`[uiManager + 0x80]` (the focused field) stays null and no `OnMouseDown`/focus
action fires. The gap is between "control under the cursor is found" and "that
control is told it was pressed," inside the lifted UI event-dispatch (the
recursive coordinate-transform traversal through vtable slots 0x18 / 0x2c / 0x30).
This is the same root as the missing login buttons: the recompiled CLogin /
UIWindow event + child-draw logic only partly works. Both need deeper
recompiled-code debugging of that traversal, not more runtime input work.

Runtime trace knobs added for this: `RECOMP_TRACE_HIT` (every `PtInRect` with its
rect, point and result) and `RECOMP_TRACE_FOCUS` (the focused-field pointer on
change).

## Correction: login text input actually works — the blockers are render + a crash

The section above concluded clicks don't focus a field and chars are dropped.
That was a misread: the focus pointer is read from `[0x978364] + 0x80` (the
message-filter object that `FUN_0081a94a` passes to `FUN_0081b1a4`), **not**
`[0x9784c0] + 0x80` (a different object, which does stay null). Tracing the real
one shows the client **auto-focuses the Login ID field** on entry and delivers
typed chars correctly:

- `FUN_0081b1a4` reads `[this+0x80]` (= the focused `CInputString`, e.g. the ID
  field inner object at control+4) and calls its `OnChar` vtbl[0x3c] =
  `FUN_004a0bc9` with the right character.
- `FUN_004a0bc9` inserts the char into the field's string (via `FUN_0045f0f3`).
  Dumping the field object after typing `ABCDE` shows the string buffer at
  `[field+0x34]` grow `A → AB → ABC → ABCD → ABCDE`. **The text is stored.**

So two real blockers remain, both in lifted code, both the same class as the
missing login buttons:

1. **Render:** the field stores its text but never draws it (and the aux buttons
   load but never draw). Cosmetic, but it's why the screen looks empty.
2. **Focus switch between fields:** the *default* focus works, but a click on a
   different field doesn't move focus (the UI click→focus dispatch, traced
   earlier, finds the control but never fires its press). A local demo hook
   (`RECOMP_LOGIN_INJECT`, in gitignored `build/gen`) that forces
   `[0x978364]+0x80` to the hovered field proves both ID and Password fill
   correctly (`USER` into the ID field, `PASS` into the Password field, by object
   address).
3. **Submit crash:** with both fields filled, pressing Enter triggers the login
   path (GetAsyncKeyState/Dispatch activity jumps, IME fires) but crashes in a
   `shape2d.dll` infinite recursion (`FUN_51406cf7` descent through an `atan2`,
   stack overflow) during the post-submit transition — before the login packet
   reaches the server. A separate deep lifted-engine bug, probably fed degenerate
   geometry from the same UI-init problem.

Net: the credential **input path works end to end up to submit** (proven by the
stored field strings). Reaching character select needs the shape2d transition
crash fixed (and, for a visible demo, the field/button render).

## End-to-end login works: credentials reach the server (step 6 → login)

The shape2d "crash" on submit was a misdiagnosis: the runtime was aborting on
unimplemented imports the login path calls (the backtrace's `atan2` frames are
mis-symbolised trampolines). The login submit calls, in order:

- `netapi32.dll!Netbios` — reads the adapter MAC for the login HWID;
- `kernel32.dll!CreateToolhelp32Snapshot` + `Process32First/Next`,
  `Module32First/Next`, `Thread32First/Next` — a GameGuard-style scan of the
  running processes/modules/threads for blacklisted tools.

Stubbed all of them (runtime/stubs.c): `Netbios` reports one LANA and a fixed
fake MAC (never the host's real address); the Toolhelp32 snapshot reports one
benign process and an otherwise empty enumeration, so the scan comes back clean.

With those in place the full login round-trip completes against the real
`MapleStoryServer62`:

```
Recieved packet loginRequest (49 bytes)
Packet: [01 00  0A 00 "hero213644"  06 00 "secret"  00 00 ...]
Login - hero213644
Registered User - hero213644
Send: failure(reason: LoginError.licenseAgreement)
```

The client encrypts and sends the typed username/password, the server decrypts
them (correct per-connection IV), auto-registers the account, and replies with
`licenseAgreement` — the normal v62 response for a brand-new account (it must
accept the EULA before proceeding). The client receives it and returns to the
login screen without crashing (exit 0).

Caveat: filling the **Password** field still relies on the local
`RECOMP_LOGIN_INJECT` focus hook (gitignored `build/gen`), because the click→focus
dispatch that would move focus between fields is still broken (see above). The ID
field fills natively. Next steps: handle the `licenseAgreement` step to reach
world/character select, and fix the real click→focus + field/button render.

## Login → world select works end to end against the real server

Two fixes took the recompiled client from "login screen" to the **world-select
screen**, driven entirely by `colemancda/maplestory`'s `MapleStoryServer62`:

1. **`MsgWaitForMultipleObjects` must wake on posted messages, not just input.**
   The earlier fix returned `WAIT_OBJECT_0 + nCount` only when raw input was
   queued. But the client receives socket data through `WSAAsyncSelect`, which
   `rt_post_message`s an `FD_READ` window message when the server replies. With
   the main loop only waking on input, the client sent the login but never woke to
   read the response. Added `rt_messages_pending(CPU*)` (runtime/user32.c) and
   made `MsgWaitForMultipleObjects` also return `nCount` when the thread's message
   queue is non-empty (runtime/extra.c). Now the client pumps the `FD_READ`,
   reads the reply, and advances.

2. **Server (`autoregister`) now accepts the EULA for new accounts** so a fresh
   login does not stop at `licenseAgreement` (a dev-only change on the
   autoregister path).

Full observed flow (server log):

```
Recieved packet loginRequest (49 bytes)  -> Login - heroNNNN -> Registered User
Send: success(username: "heroNNNN")       (client reads it)
Recieved packet serverListRequest (2 bytes)
Send: world(... "Scania", 20 channels ...) + serverList end
```

The recompiled client then renders the **"Select a world/channel"** screen with
the Scania world banner. So login and the world-list exchange are complete; the
client reaches world select.

Caveat unchanged: filling the Password field and submitting still use the local
`RECOMP_LOGIN_INJECT` focus hook (gitignored `build/gen`), because the UI
click→focus dispatch and the field/button text render are still the open
lifted-code bugs. Next: select a channel (needs the same click fix) → character
list / world entry.

## Full pre-game flow works: login → world → channel → character select

With real clicks (no injection hook) the recompiled GMS v62 client now walks the
entire pre-game flow against `MapleStoryServer62`:

```
loginRequest  -> success        (type ID, click the Password field, type, Enter)
afterLogin    -> success
serverListRequest -> world "Scania" + 20 channels
  (click the Scania world slot; the channel grid CH.1..CH.20 renders)
serverStatusRequest -> Server Status World 0 Channel 0
characterListRequest -> CharacterListResponse(characters: [], maxCharacters: 9)
  (click CH.1, click "Enter Channel")
```

The client then renders the **character-select screen** ("Select a character",
"Selected world: SCANIA, CH. CHANNEL 1"), loading the empty character list for
the new account. Clean exit, no crash.

Two corrections/fixes made this work:

1. **Clicks were never broken.** The earlier "click→focus dispatch is broken"
   finding was a misread of the focus pointer (I logged `[0x9784c0]+0x80`, but the
   char path reads `[0x978364]+0x80`). Clicking the Password field *does* move
   focus; login succeeds with no injection. The login-screen text just isn't
   *rendered* (a cosmetic bug local to that screen; the channel grid and character
   screen render fine). Note: world/channel controls are sampled sparsely, so
   synthetic clicks need a long button hold (down, hold ~25 frames, up) to land.

2. **Lifter missed vtable-only `__thiscall` methods.** Selecting a channel crashed
   calling a virtual method whose vtable slot pointed at `0x5797eb` — a 13-byte
   function (`push ecx; mov ecx,[ecx+0xCC]; call ...; ret`) right after another
   function's `ret`, reached only through the vtable. `tools/data_code_pointers.py`
   rejected it because its `PROLOGUES` set lacked the push-reg opcodes; `push ecx`
   (0x51) is a very common MSVC "reserve one local" prologue. Added 0x50–0x54 to
   the set (+138 data-pointer functions total) and re-lifted; the method is now a
   real entry and the crash is gone. This fixes the whole class of missed
   vtable-only methods, not just this one.

Remaining: the empty character list shows "Connecting to server" then empty slots;
character *creation* and world entry are the next steps. The login-screen text
render is still open (cosmetic).
