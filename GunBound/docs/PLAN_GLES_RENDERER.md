# Plan: an OpenGL ES 3 renderer for the recompilation's Direct3D 7 layer

For a separate working session. The software rasteriser stays as the reference
and the default; this adds a GPU backend beside it, selected with
`--renderer soft|gles`. Read `recomp/README.md` first, then `runtime/ddraw.c`
(the fake COM objects, `draw()` at about line 1069), `runtime/raster.c` and
`runtime/video.c` (the present path).

## Why

The software rasteriser is about half of battle CPU on the Retroid Pocket 5
(`rt_raster_triangle` 24 %, `texel` 22 % in the device profile). A GPU path
frees that core (guest threads are confined to one CPU on ARM), gives filtered
scaling to the 1080p screen for free, and is the renderer iOS needs (see
`PLAN_IOS.md`). Target API: OpenGL ES 3.0 (RP5: Adreno, GLES 3.2 through Mesa
freedreno; iOS: native ES 3.0; host: Mesa, including llvmpipe for tests).

## What the game really does (facts, from the code and earlier traces)

- Rendering is DirectDraw 7 surfaces (RGB565 primary and back buffer) with a
  Direct3D 7 device on top. Every frame: `BeginScene`, `Clear`, a few dozen
  `DrawPrimitive` calls (42 in 120 menu frames; more in battle), `EndScene`,
  then `Blt` back buffer to primary. `Flip` is never called.
- Vertices are pre-transformed (`D3DFVF_XYZRHW`), diffuse colour, up to one
  texture coordinate set. Primitive types: triangle list, strip and fan
  (`draw()` rejects points, lines and untransformed vertices today).
- **The same back buffer is also written by the CPU.** The sprite blitters
  `Lock` the surface and store 16-bit pixels, between and after D3D draws. The
  dialog dimming is an untextured fan drawn over CPU-drawn pixels, and the
  dialog itself is CPU-drawn over that. This is the central design problem.
- Textures are DirectDraw surfaces in RGB565, ARGB1555 or ARGB4444, filled by
  the CPU, with an optional colour key. Texture stage 0 only (`draw()` reports
  a second stage as unsupported).
- Render states in use are listed by `draw()`: blend enable and factors, alpha
  test, cull, flat shading, texture factor, stage 0 colour and alpha ops and
  args, address modes, bilinear, z enable/write/func, colour key enable.

## Design

### 1. A backend interface (small refactor, soft path stays bit-identical)

Split `draw()` into (a) decode the vertex buffer and snapshot the state into a
`RasterState` plus `RasterVertex[]` (already done) and (b) hand the batch to a
backend:

```c
typedef struct RtGpu {
    int  (*init)(SDL_Window *);
    void (*surface_cpu_written)(Surface *s, RECT r);   /* Unlock, Blt, BltFast, Clear */
    void (*surface_cpu_wants)(Surface *s);             /* Lock: make the CPU copy current */
    void (*draw)(Surface *target, Surface *zbuf, Surface *tex, const RasterState *, const RasterVertex *v, int n);
    void (*clear)(Surface *target, Surface *zbuf, uint32_t flags, uint32_t color, float z, RECT r);
    void (*present)(Surface *primary);
} RtGpu;
```

`--renderer soft` uses the existing code through the same interface. Nothing in
the soft path may change its output; `test/frames.sh` must keep passing
byte-for-byte.

### 2. CPU copy is authoritative; the GPU holds a mirror

Each `Surface` that is ever a render target gets a state: `SYNCED`,
`CPU_NEWER`, `GPU_NEWER`, plus a dirty rectangle for `CPU_NEWER`.

- `Unlock` / `Blt` / `BltFast` / `Clear` (when done on the CPU) set `CPU_NEWER`
  and grow the dirty rect.
- Before any GPU draw into a surface that is `CPU_NEWER`: upload the dirty rect
  (`glTexSubImage2D`, RGB565) into the surface's colour texture, and attach it
  to the FBO. Draw. Mark `GPU_NEWER`.
- On `Lock` of a `GPU_NEWER` surface: read the FBO back (`glReadPixels`) into
  the guest memory the game sees, then mark `SYNCED`. Convert on the CPU if the
  readable format is not RGB565 (query `GL_IMPLEMENTATION_COLOR_READ_FORMAT`);
  480 000 pixels is cheap with NEON.
- `present`: if the primary's source is `GPU_NEWER`, blit the FBO to the window
  with the scaling shader; otherwise upload and blit.

The guest only ever sees a consistent surface, so a lost GL context (iOS
backgrounding) is recoverable: rebuild every GPU resource from the CPU copies.

**Phase 0 decides whether this is worth it:** instrument the soft renderer to
count CPU/GPU ownership switches per frame in the menu, the lobby, the ready
room and a battle (`--trace-d3d` summary). If a battle frame needs more than a
few readbacks, the readbacks, not the draws, become the cost, and the plan
changes (for example, keep the CPU rasteriser for fans over CPU content and
use the GPU only for textured effect quads). Report the numbers before building
the rest.

### 3. Shader

One uber-shader with uniforms, then variants only if profiling says so:

- Vertex: `ndc = (2x/W - 1, 1 - 2y/H)` for a target of W by H, plus the
  half-pixel offset described under pixel parity; depth from `z` in 0..1;
  pass diffuse (`flat` qualifier when `RasterState.flat`), `uv`, and use `rhw`
  for perspective-correct interpolation once `RasterVertex` carries it (today
  it does not; the game's 2D vertices have `rhw = 1`, so add the field and
  assert on other values first).
- Fragment: stage 0 colour/alpha operations (`D3DTOP_SELECTARG1/2`, `MODULATE`,
  `MODULATE2X`, `ADD`, `SUBTRACT`, `BLENDTEXTUREALPHA`, `BLENDFACTORALPHA`, as
  `raster.c` supports them; copy its list), args (`DIFFUSE`, `TEXTURE`,
  `TFACTOR`, `CURRENT`, with `COMPLEMENT`/`ALPHAREPLICATE` flags), `texture_factor`,
  alpha test by `discard` with `alpha_func`/`alpha_ref`, colour key.
- Colour key: convert textures to RGBA8 on upload and write alpha 0 for texels
  equal to the key. Check how `raster.c` treats keyed texels under bilinear
  filtering and match it exactly (it keeps a `keyed` flag in `texel()`).
- Textures: upload as RGBA8 (simple, parity-friendly) first; `GL_RGB565` /
  `GL_RGBA4` later if bandwidth matters. Address modes map to
  `GL_REPEAT` / `GL_CLAMP_TO_EDGE` / `GL_MIRRORED_REPEAT`. `bilinear` maps to
  `GL_LINEAR` / `GL_NEAREST`; no mipmaps.
- Fixed-function state to GL state: `D3DBLEND_*` to `glBlendFunc`, `D3DCMP_*`
  to `glDepthFunc`, cull and winding (D3D clockwise front faces; the software
  path culls by `D3DCULL_*`, copy its sign convention), depth write mask.
  Depth buffer: `GL_DEPTH_COMPONENT16` renderbuffer, cleared by `Clear`.
- Fans become triangles on the CPU (as `draw()` already indexes them); strips
  too. Batch consecutive draws with identical state into one `glDrawArrays`.

### 4. Texture cache

Textures are DirectDraw surfaces the CPU fills. Key the GPU texture by surface
id plus a version counter bumped on `Unlock`, `Blt` into it and `SetColorKey`.
Re-upload only changed versions, and only the dirty rectangle where known.
Per-frame upload volume is a metric to log.

### 5. Pixel parity (the verification that matters)

`raster.c` documents its rules at the top: top-left fill rule, pixel centres,
texel `(i, j)` covers `u * width - 0.5`, 28.4 fixed-point coverage. D3D7 and
GL differ by half a pixel at the origin; translate vertices by the right
offset and prove it with tests, not by eye.

- **Record and replay.** Add `--record-d3d FILE` to the soft path: every
  `Clear`, `DrawPrimitive` (state snapshot, vertices, the texture's pixels by
  content hash) and surface upload, as a binary log. Add
  `recomp/test/d3d_replay.c`: replays a log on either renderer headlessly
  (EGL pbuffer / `SDL_VIDEODRIVER=offscreen`, Mesa llvmpipe on the host) and
  writes the final frame. Record the menu, the lobby, the ready room and a
  full battle once; commit only small logs, never textures from the game
  (derive them at test time from the user's own files, or hash and skip).
- **Acceptance per frame:** identical coverage except edge pixels, at most
  0.1 % of pixels differing, per-channel difference at most 8 in 8-bit space
  (565 quantisation plus blend rounding), no pixel off by more than 40.
  Compare with `recomp/test/cursor.py`-style tooling or a new
  `compare_frames.py` that prints the worst case and writes a diff image.
- Everything unsupported must fall back, never mis-draw: sync the target to the
  CPU and call the soft rasteriser for that draw, and log it once like
  `unsupported()` does.

### 6. Present and scaling

`video.c` presents through `SDL_Renderer` with a streaming texture. A GL
backend needs its own context (`SDL_GL_SetAttribute` ES 3.0, double buffered)
and cannot share the window with `SDL_Renderer` using a different backend.
Reimplement the existing scaling options in a shader: `fit`, `integer`,
`stretch`; `sharp` (point sampling up to the next whole multiple, then bilinear
down to the picture size), `nearest`, `linear`. The window-readback path used
by `--dump-window` becomes `glReadPixels` on the default framebuffer.
Keep the SDL_Renderer path for `--renderer soft`.

### 7. Threading

A GL context is current on one thread. Find which guest thread calls the
Direct3D and DirectDraw methods and presents (it should be the main guest
thread; `rt_video_on_video_thread()` exists). Make the GL context current on
that thread and assert on every backend call. If any call can come from another
thread, marshal it.

## Phases and acceptance

0. Instrument and report ownership switches per scene (above). Gate for the rest.
1. Backend interface; soft path through it; `frames.sh` and `run.sh` tests still pass unchanged.
2. `--renderer gles` that presents through GL only (CPU surfaces uploaded, scaled in a shader). Menu and lobby look identical to soft; pad/touch/scale tests pass.
3. GPU draws for untextured fans (clear, wipes, dimming) with the CPU/GPU sync model. Replay tests green on a recorded menu.
4. Textured, blended, colour-keyed quads and the texture cache. Replay tests green on a recorded lobby, ready room and battle.
5. Device run on the RP5 (`--renderer gles --fullscreen`): a full two-player battle, profile before and after (`--profile`), CPU use of the guest thread, frame pacing at the game's 20 fps tick and 60 Hz present.
6. Robustness: context loss and recreate, unsupported-state fallback, `--renderer soft` remains the default until phase 5 numbers are good.

## Risks

- Too many readbacks per frame (decided at phase 0).
- Bilinear sampling and blending differences make a battle look slightly
  different from the software frames; the tolerance above is the contract, and
  the soft renderer stays available for exact output.
- RGB565 renderbuffers are not readable in every driver; fall back to RGBA8
  plus CPU conversion.
- Mesa llvmpipe is slow but fine for replay tests; keep logs short.

## Do not

Change soft-rasteriser output, add game assets to the repository, replace the
software path, or push without asking.
