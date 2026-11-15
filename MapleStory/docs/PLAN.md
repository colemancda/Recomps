# Plan: a static recompilation of MapleStory (GMS v62)

The same method as the GunBound and Age of Mythology recompilations in this
repository: lift the x86 Windows client to C and run it on Linux (and later
aarch64). The lifter (`lift/`) and the runtime (`runtime/`) are shared with
those projects and adapted here.

Nothing derived from the game is committed. The client, its DLLs and the `.wz`
data live in the git-ignored `orig/`; build output in `build/`.

## The input binary

MapleStory v62 (Global, build 2008-11-11) ships three forms of the client in
`orig/`:

- **`MapleStory.exe`** (retail, 3.2 MB) is **Themida-packed**: the code section
  is encrypted on disk and some functions are kept as "stolen code" run by
  Themida's virtual machine. Even dumped from memory after unpacking, those
  functions never exist as plain x86. **Not a lift target.**
- **`GMSv62.exe`** (6.4 MB) is the retail client **unpacked**, with a rebuilt
  import table. Its entry point and 99.6% of its code section are byte-identical
  to the live unpacked retail image (verified by dumping `MapleStory.exe` under
  Wine with `tools/dump_unpacked.py` and diffing). The 0.4% that differs is
  exactly the Themida VM stubs and import thunks — i.e. `GMSv62.exe` holds the
  real code where retail holds protection. **This is the lift target.**
- **`GMSv62_4GB.exe`** is `GMSv62.exe` with the large-address-aware flag; the
  runtime owns the guest address space, so it is ignored.

## Images to lift

The Wizet engine is split across DLLs, loaded by the client (COM-style, bases
at 0x50000000+). The lift set, with suggested guest bases:

| Image            | Role                               | Base       |
|------------------|------------------------------------|------------|
| GMSv62.exe       | the client                         | 0x00400000 |
| Canvas.dll       | bitmap/canvas objects              | 0x50000000 |
| Gr2D_DX8.dll     | 2D rendering on Direct3D 8         | 0x50400000 |
| NameSpace.dll    | the UOL/namespace object model     | 0x50800000 |
| PCOM.dll         | the component (COM-lite) framework | 0x50c00000 |
| ResMan.dll       | resource manager                   | 0x51000000 |
| Shape2D.dll      | 2D geometry                        | 0x51400000 |
| Sound_DX8.dll    | sound on DirectSound 8             | 0x51800000 |
| ijl15.dll        | Intel JPEG Library (.wz images)    | 0x60000000 |
| WzMss.dll        | networking / BBS SOAP client       | 0x10000000 |

Not lifted / stubbed:

- **npkcrypt.dll, npkpdb.dll** — nProtect KeyCrypt (GameGuard). Stub the seven
  `NPK*` imports; the game runs without the driver.
- **GameGuard.des** — the GameGuard payload, not loaded by the recompilation.
- **ZLZ.dll, WzFlashRenderer.dll** — ZLZ (a crypto helper) and the Flash UI
  renderer; needed only if the features that use them are reached.

## Standard Win32 imports (runtime)

`GMSv62.exe` imports 265 symbols across kernel32, user32, gdi32, advapi32,
oleaut32, ole32, shell32, winmm, dinput8, ws2_32, wininet, netapi32,
iphlpapi. Most are already provided by the AoM runtime copied here. New work:
DirectInput 8 (keyboard+mouse), Winsock (as in GunBound), and `wininet` (the
launcher loads a web page; it can be stubbed to skip it).

## Steps

0. **Survey.** Unpack decision (done), image map (done), Ghidra function export
   for GMSv62.exe and each lifted DLL, net/packet notes.
1. **Lifter + loader.** Already multi-image from AoM. Add the MapleStory module
   table to `build.sh`; lift the exe and the engine DLLs; link and attach.
2. **Boot headless.** Stub GameGuard, satisfy start-up (registry, the `.ini`,
   wininet), reach the client's main loop with the null renderer.
3. **`.wz` data + JPEG.** The client reads `List.wz`/`Base.wz` and the data
   `.wz` files through ResMan/NameSpace; images are JPEG via ijl15. Get the
   login screen to load its UI from `UI.wz`.
4. **Direct3D 8 rendering.** Reuse AoM's `d3d8gl.c` for Gr2D_DX8's device; draw
   the login and world.
5. **Sound.** Sound_DX8 on the runtime's DirectSound mixer (as AoM).
6. **Networking.** A local v62 server speaks the MapleStory protocol; the client
   connects through the runtime's Winsock. (A server is out of scope for the
   recompilation itself; interop is the test.)
7. **aarch64 + handheld**, as for AoM.

## Risks

1. **COM-lite object model (PCOM).** The engine passes objects by vtable across
   DLL boundaries; the loader must link these like AoM's COM activation.
2. **Scale.** 25,828 functions in the exe alone (baseline lift), more in the
   DLLs — the largest of the three games.
3. **GameGuard side effects.** If the client checks that the driver loaded
   beyond the seven imports, stub those checks too.
4. **Networking determinism** is less critical than GunBound/AoM (no lockstep),
   but packet handling must be byte-exact against a server.
