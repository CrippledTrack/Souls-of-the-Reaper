# Custom renderer: design notes (Vulkan first)

The aim is a renderer that draws Diablo III through Vulkan from the game's own draw calls.
It uses the shaders XenosRecomp converted ahead of time (see README.md) instead of ReXGlue's
emulated Xenos GPU.

## What the game's Direct3D does (USA title update, measured 2026-11-06)

The game uses the XDK's Direct3D 9-style library, which is statically linked in. Most
`Set*` calls don't reach a hookable function. They write into the device's **shadow copy
of the GPU registers** and set dirty bits. Each draw then flushes the dirty registers into
the command buffer. The shadow is always current, so reading it at draw time gives the
complete state.

### Draw calls

- `sub_82D621B0(device, primitive, base vertex, start index, index count)` is an indexed
  draw, and the only draw used at the title screen. The game's renderer calls it from
  `0x82584478`. The base vertex goes to `VGT_INDX_OFFSET` (`0x2102`). The primitive is the
  Xenos type (4 triangle list, 6 triangle strip).
- 13 sibling functions share its structure: `sub_82D59AD8`, `59D30`, `5A600`, `612B0`,
  `617B8`, `61D98`, `62628`, `62AC8`, `71B28`, `72700`, `73030`, `73450` and `7A1C8`.
  Each calls `sub_82D7CE70` (commit pending state), `sub_82D7C0C0` (constants) and
  `sub_82D7BD28` (register blocks). These are the other `Draw*` variants.
- The title-screen frame has 68 draws: 51 triangle lists and 17 triangle strips.
- **Which variants the game uses** (title, menu, loading, gameplay and the inventory, traced
  through the shared commit routine `sub_82D7CE70` and the variants' own hooks): the indexed
  draw above, and `sub_82D61D98(device, primitive, start vertex, vertex count)`, a
  non-indexed draw called next to it (`lr 0x82584464`). It writes the start vertex to
  `VGT_INDX_OFFSET` and splits counts above 65535 into several packets. So far it only draws
  4-vertex triangle strips (screen and UI quads). Both are captured by the Vulkan renderer
  (non-indexed draws get generated indices 0..n-1). The other 12 variants have not been
  seen; each logs a warning the first time it is called, since Vulkan-only mode would lose
  what it draws. `--trace_draws` hides functions called less than once per frame, which is
  why the non-indexed draw was missed at first.

### Device state, read at draw time

| Device offset | Contents |
|---|---|
| `+0` … `+40` | 64-bit dirty masks |
| `+1152` | fetch constants (register `0x4800`): 32 slots of 6 dwords, covering textures and vertex buffers |
| `+1920` | vertex shader constants (`0x4000`): 256 float4; written by `sub_82D65C78(device, start, data, count)` |
| `+6016` | pixel shader constants (`0x4400`): 256 float4; written by `sub_82D65D50` |
| `+10112` | boolean constants (`0x4900`): 8 dwords, vertex bools then pixel bools |
| `+10368`, `+10444`, `+10528`, `+10548`, `+10596` | register blocks `0x2000`, `0x2100`, `0x2180`, `0x2200`, `0x2280` |
| `+10680`, `+10832` | register blocks `0x2300`, `0x2380` |
| `+12240` | vertex declaration object (set by `sub_82D665E8`) |
| `+12812` | index buffer object |
| `+13068` | pixel shader object (set by `sub_82D661C0`); 0 for depth-only draws |
| `+13072` | vertex shader object (set by `sub_82D663C8`) |

The registers used so far are:

| Register | What it holds |
|---|---|
| `RB_DEPTHCONTROL` (`0x2200`) | depth test, write and function (same order as `VkCompareOp`) |
| `RB_BLENDCONTROL0` (`0x2201`) | blend factors and operations |
| `RB_COLOR_MASK` (`0x2104`) | colour write mask |
| `PA_CL_VPORT_*` (`0x210F`–`0x2114`) | viewport scale and offset |

### Structures

- **Vertex declaration:** the element count is at `+24`. 12-byte `D3DVERTEXELEMENT9`
  entries start at `+52`: stream and offset; type; method, usage and usage index.
- **Vertex format types:** the low six bits of the type are the Xenos vertex format. Bit 8
  means signed, bit 9 normalised, bit 11 D3DCOLOR (ARGB) order.
- **Vertex streams:** stream *s* reads vertex fetch constant 95 - *s*. Word 0 holds the
  physical address with type 3 in the low two bits. Word 1 holds the endian mode and the
  size in dwords. The stride isn't stored with the stream, because the XDK bakes it into
  per-layout patched vertex shaders, so the capture assumes tightly packed vertices.
- **Index buffer object:** the guest address is at `+24`, bit 31 of `+0` selects 32-bit
  indices, and the size is at `+28`.
- **Title-screen layouts:** four declarations, all single-stream and tightly packed. They
  use `32_32_32_FLOAT` or 16-bit × 4 positions, 8-bit × 4 colours, normals and texcoords,
  and 32-bit float texcoords.

### Shaders

Each shader object holds an unchanged copy of its container header (+40 for pixel, +872
for vertex). Hashing that header gives the file name `--capture_shaders` writes. All 33
shaders the title frame binds were found in the capture.

## Approach: read state at draw time

Hooking every `Set*` the way Unleashed Recompiled does isn't possible here, because most of
them are inlined. Hooking the draw functions works instead. At each draw the renderer reads
the bound shader objects (hashed to find the XenosRecomp output), the constant and register
shadows, the vertex declaration and the index buffer. Render targets and resolves (EDRAM →
texture copies) need hooks of their own, and those functions aren't identified yet.

## Status (2026-11-07)

`--shadow_renderer=true --shadow_shader_dir=<XenosRecomp output with .spv>` opens a second
window drawn by `port/src/experiments/shadow_renderer.cpp`. It runs on its own Vulkan 1.3
device: buffer device address, descriptor indexing, 64-bit integers and dynamic rendering.
The window comes from ReXGlue (`rex::ui::Window`, with no presenter). ReXGlue is a DLL with
its own copy of SDL, so the port can't create SDL windows itself. On Windows the Vulkan
surface comes from the window's HWND; other platforms aren't done yet.

- **Milestone 1 (done):** the window, the swapchain, and a clear on every guest present.
- **Milestone 2 (done):** title-screen geometry. All 68 draws per frame are replayed with
  the XenosRecomp shaders, the guest's constants and its viewport, depth and blend state,
  and nothing is skipped. Text and quads land exactly where the game draws them
  (`milestone2-vulkan.png` next to `milestone2-rexglue.png`). Everything is white because
  every sampler reads a 1x1 white placeholder.
- **Milestone 3 (done):** textures (`milestone3-vulkan.png`). The logo, glow, text and
  Blizzard logo draw with their real textures and blending. The 3D background is still
  black: the game renders it into a render target and composites the resolved copy, and
  render targets and resolves aren't done yet.

- **Milestone 4 (done): render targets, resolves and clears.** The title screen, castle
  scene included, now closely matches ReXGlue (`milestone4-vulkan.png`,
  `milestone4-compare.png`: ReXGlue left, Vulkan right).
  - Draws render into host images. Each colour target is keyed by EDRAM base and format
    (`RB_COLOR_INFO`) and each depth buffer by its base (`RB_DEPTH_INFO`), all 2048x1536 in
    guest pixel coordinates. Target contents persist between passes (load, not clear).
  - `D3DDevice_Resolve` (`sub_82D5B9D0`, hooked) copies targets into host images that
    replace the destination textures. Depth resolves go through the arena into
    `R32_SFLOAT`. The window shows the frame's last resolve, the game's front buffer.
  - `D3DDevice_Clear` (`sub_82D57638`, hooked):
    `(device, rect count, rects, flags, ARGB colour, z in f1, stencil)`. Flags `0xF` are
    colour targets, `0x10` depth and `0x20` stencil. Clears and resolves carry a sequence
    number and replay in game order between draws. The scene clears to `0x152430` with
    Z = 0 (reversed Z).
  - **Bugs found on the way:**
    - **Colour correction:** the white background came from the colour-correction pass
      (draw 45), which samples the scene through a 64x64x64 tiled 3D LUT. 3D textures are now
      decoded (`TiledOffset3D`) and bound through the Texture3D array; their ids carry
      `kTexture3DBit`.
    - **Vertex formats:** `D3DDECLTYPE` bit 9 means *integer* (unnormalised), not normalised:
      `UBYTE4` is `0x1A2286` and `UBYTE4N` is `0x1A2086`. Also, a Xenos `8_8_8_8` vertex
      component x is the dword's low byte. The castle meshes use `SHORT4` fixed-point
      positions and `UBYTE4` texcoords rebuilt in the shader as `hi + lo/256`, so both mattered.
    - **Packed mips:** a texture with packed mips (fetch dword 5 bit 11) whose shorter side
      is 16 texels or less stores its base level 16 texels into the mip-tail tile. The castle's
      8x8 lightmap read as black without that offset, and everything came out fog-coloured.
    - **Normals:** XenosRecomp declares NORMAL, TANGENT and BINORMAL as `uint4` but reads
      them through `tfetchR11G11B10()`, which is `asfloat(value)` without the packed-normal
      spec constant, so they need float bits, not raw integers. Only BLENDINDICES is integer.
      With raw integers the lantern glass meshes (draws 20-25) got zero fresnel edge alpha.
      They drew nothing, and because their alpha is the bloom mask (the bright pass in draw 35
      multiplies by scene alpha), the bloom vanished too.
    - **Debug views:** resolve cycling shows a slot's contents at the end of the frame, not
      at that resolve. Slot 9 is written three times per frame, which explained the
      disagreement with the after-draw view.
  - **Half-pixel offset:** the small copyright text came out with broken glyphs because it
    sampled its tightly packed font atlas half a texel off. D3D9 puts pixel centres at
    integer coordinates and Vulkan at +0.5; XenosRecomp's vertex shaders add
    `g_HalfPixelOffset * w` (shared constants `+264`) to correct for it, and that is now set
    to half a pixel of the viewport: `(0.5 / scale_x, -0.5 / scale_y)`.
  - **Remaining differences:** none visible on the title screen.
  - **Known gaps:**
    - EDRAM aliasing between formats and pitches isn't modelled (not needed by this frame).
    - Exponent bias is ignored: with float targets, the `16_16_16_16` target bias (+5) and
      the texture read cancel out here.
    - 2x MSAA renders as 1x.
    - Clear rectangles are ignored, so clears cover the whole image.

- **Milestone 5 (done): the main menu.** The screen after "press any button" (hero, banner,
  moonlit scene, menu) now matches ReXGlue apart from edge aliasing, because MSAA is still
  rendered as 1x (`milestone5-compare.png`: ReXGlue left, Vulkan right). The menu frame
  has 110 draws: a 768x768 shadow map, the scene at 1120x584 with 2x MSAA, bloom, and the UI.
  - **Alpha test:** `RB_COLORCONTROL` bit 3 enables it, bits 0-2 hold the function, and
    `RB_ALPHA_REF` (`0x210E`) holds the reference. XenosRecomp compiles it into every pixel
    shader as `clip(oC0.w - g_AlphaThreshold)` behind spec constant bit 1, so pipelines now
    set that constant (constant id 0) and the threshold goes to shared `+272`. Only
    GREATER/GREATEREQUAL fit that form; others are logged. Without it, the banner was a solid
    rectangle instead of a cloth with a notched hem.
  - **Alpha to coverage:** `RB_COLORCONTROL` bit 4. The hero's hair and cloak rely on it to
    drop low-alpha texels on the 2x MSAA target. Until MSAA exists it's approximated as an
    extra alpha cut at 0.5.
  - **Cube textures:** the character and prop shaders take ambient light from an irradiance
    cube map (a 32x32 `8_8_8_8` tiled cube with 4 levels here). Without it they sampled the
    white placeholder and lost the scene's teal ambient. Cube faces follow ReXGlue's layout:
    each level holds the 6 faces in turn, each face 32-block aligned and 4 KiB aligned, and
    the next level starts after all six. Ids carry `kTextureCubeBit` (`0x4000`) and the
    TextureCube index goes to shared `+128 + r*4`.
  - **XenosRecomp light loop (fork fix):** the lit vertex shaders loop over point lights with
    a predicated *backward* jump (`cf 15: (!p0) jmp 12`). XenosRecomp decided a jump was
    backward from the instruction's direction bit, which is 0 on these, so it emitted the
    loop body once as an empty `if`. Only the first light was applied, and the hero missed
    her key light. Backward jumps are now detected by comparing the target with the current
    index, which switches the shader to XenosRecomp's general `pc`/`switch` form. That
    affects 346 of the 1,437 shaders.
  - **XenosRecomp predicated exec blocks (fork fix):** `CondExecPred` blocks (13 shaders)
    were emitted without their condition and always ran; they're now wrapped in
    `if (p0)` / `if (!p0)`. `XENOSRECOMP_CF_COMMENTS=1` adds a `// cf` comment per
    control-flow instruction to the output, which is how the loop was found.
  - **Culling:** `PA_SU_SC_MODE_CNTL` bit 0 culls front faces, bit 1 back faces, and bit 2
    makes clockwise the front. It's set per draw for triangle primitives.
  - **Input for testing:** rexglue reads the keyboard through SDL, which ignores posted
    window messages, so automated runs briefly bring the game window to the front and send a
    real Enter (the A button).
  - **Remaining differences:** edge aliasing only.

Shader debugging: `XenosRecomp --hlsl <in.hlsl> <out.spv> <ps|vs>` (fork) recompiles an
edited HLSL output with the `--batch` settings. Copying the shader directory and editing one
pixel shader to output intermediate values (fog, UVs, lighting) located the last two bugs.

D3DDevice_Resolve, from the title frame:

- **Arguments:** `(device, flags, rect, destination texture, point, level, face, clear colour, ...)`.
  Flags `0x14` is a depth resolve; `0x28000000` probably encodes an exponent bias in the
  top six bits.
- **Destination:** the texture object's fetch constant is at `+28`.
- **Copy registers:** after the call, `RB_COPY_DEST_BASE` is physical and
  `RB_COPY_DEST_PITCH` is `width | height << 16`.

Debug options (`port/src/experiments/shadow_renderer.cpp`):

| Option | What it does |
|---|---|
| `--shadow_debug_resolves` | shows each resolve in turn |
| `--shadow_debug_after_draw=<n>` | shows the target right after draw *n* |
| `--shadow_debug_flags` | 1 depth test off, 2 blending off, 4 all colour writes on, 8 no half-pixel offset, 16 culling off |
| `--shadow_dump_textures=<dir>` | writes decoded uncompressed 8- and 32-bit textures as PGM/PAM |
| F9, `--shadow_compare_every=<s>`, `--shadow_compare_dir` | side-by-side captures: ReXGlue's presenter output (`CaptureGuestOutput`) and the Shadow renderer's shown image (read back as RGBA8), plus a ReXGlue \| Vulkan \| difference x4 image at ReXGlue's size; the log gives the mean difference and the share of pixels off by more than 16. Uncompressed PNGs (`shadow_compare.cpp`) |
| `--shadow_debug_layout` | logs targets, state (including colour and mode control), textures and resolves per draw |
| `--trace_sequence=<n>` | logs the D3D calls the game makes in frame *n* |

How it works:

- **Capture (`shadow_snapshot.cpp`, game thread):** for each draw it records the shader
  hashes, the 512 constants, the booleans and the registers above. It converts the
  referenced vertices on the CPU to four 32-bit components per attribute (floats, or uints
  for the inputs XenosRecomp declares as `uint4`), and rebases indices to the lowest one
  used.
- **Replay (`shadow_renderer.cpp`, renderer thread):** one host-visible arena per frame in
  flight holds vertices, indices and constants. Constants reach the shaders through the
  three buffer-device-address push constants XenosRecomp expects. Shader input locations
  and types come from the SPIR-V (`in.var.<SEMANTIC>`). Pipelines are cached per shader
  pair, layout, topology, blend and colour mask; depth state, viewport and cull are dynamic.

- **Textures (`shadow_textures.cpp`, game thread):** for pixel sampler slots 0-15, each
  texture fetch constant is decoded once. The decoder reads the format, size, pitch,
  tiling, endian swap and swizzle, untiles the base level with `GetTiledOffset2D` (copied
  from ReXGlue/Xenia), keeps DXT and DXN data as BC blocks, and assigns an id. The cache
  key is the fetch fields plus a 16-point sample of the contents. The renderer uploads new
  textures (32 MiB per frame), applies the Xenos swizzle through the image view's component
  mapping, and writes slot ids into XenosRecomp's shared constants: the Texture2D index at
  `r*4` and the sampler index at `192 + r*4`. Samplers come from the fetch constant's clamp
  and filter fields, including the mip filter (`2` = base map only) and LOD bias.
- **Mip levels (2D):** levels 1 to `mip_max_level` (fetch dword 4 bits 6-9) are read from
  `mip_address` (dword 5); a zero mip address means no mips. Following ReXGlue's
  `GetGuestTextureLayout`:
  - Each level below the packed tail is laid out as that level of a power-of-two-sized
    texture: pitch and height aligned to 32 blocks, linear rows to 256 bytes, levels to 4 KiB.
  - With packed mips (dword 5 bit 11), the levels from the one whose shorter side is 16 texels
    or less share one tile, each at its `GetPackedMipOffset` position (also used for a packed
    base level).
  - Uploads carry all levels with per-level offsets.
- **Texture identity:** textures up to 256 KiB are hashed whole, larger ones at 16 points.
- **Cube textures:** six faces per level, see milestone 5.
- **Not done yet:** mips of 3D textures, stacked textures, `mip_min_level`, vertex
  texture fetch, and formats such as `24_8_FLOAT` and `DXT3A`.

## Vulkan only, automatic shaders and performance (2026-11-07)

### Automatic shaders

The player no longer prepares shaders by hand. With either renderer option on:

1. The shader creation hooks (`shader_capture.cpp`) hand every shader the game creates to
   `shadow_shaders.cpp`, which saves it as `<user data>/shader_cache/xenos/<vs|ps>_<hash>.bin`.
2. Background workers (half the CPU threads, at most 6, below-normal priority,
   `--shadow_shader_workers`) run the bundled XenosRecomp in its new `--spirv` mode on the
   ones without SPIR-V, 32 per process, writing `<user data>/shader_cache/spirv/*.spv`
   (written under a temporary name, then renamed). A shader that kills the converter twice
   is given up and logged.
3. The renderer looks for SPIR-V in that folder and retries missing shaders whenever a batch
   lands (a generation counter), instead of remembering them as missing.
4. `spirv/converter.txt` records the size and time of the bundled `XenosRecomp.exe` and
   `shader_common.h`. A different converter discards the converted shaders, so fork fixes
   such as the light loop reach existing caches.

The build stages the fork next to the game (`<build>/xenosrecomp/`: `XenosRecomp.exe`,
`dxcompiler.dll`, `dxil.dll`, `shader_common.h`) when `tools/XenosRecomp/build` exists
(`SOULS_XENOSRECOMP_DIR`). Without it, shaders are only captured.

On the first run, all 1,437 shaders converted with no failures during startup; later runs
find them all converted. `--shadow_shader_dir=<dir>` still selects a hand-made folder.

### Vulkan only (`--shadow_renderer_only=true`)

- **The Vulkan renderer presents into the game window itself.** At `OnPreSetup` the game turns
  the SDK's `host_presentation` off, so `ReXApp` never attaches the GPU plugin's presenter to
  the window and takes its detached-overlay path instead, while the plugin keeps executing the
  guest's GPU commands. The renderer creates its Vulkan swapchain on the game window (it
  follows resizes and fullscreen), letterboxes the picture, and holds 60 FPS in gameplay.
  ReXGlue's rendering (`gpu_host_rendering`) is off from the start.
- **ReXGlue's overlays** (settings F4, debug F3, console, toasts, achievements) are drawn by the
  Vulkan renderer. `shadow_overlay.cpp` is the app's `OnCreateImmediateDrawer`: on the UI
  thread, where ReXGlue draws them, it records each overlay frame (vertices, indices, draws
  with scissors, texture uploads and releases) instead of drawing. The renderer asks the UI
  thread for a recording each frame and replays the latest one on top of the game image with
  ReXGlue's own immediate-drawer SPIR-V (vendored in `overlay_shaders/`, same interface:
  combined image sampler at set 0 binding 0, inverse coordinate size as a push constant).
- **First run:** until every shader the game has created is converted, the window shows black
  with a progress dialog ("Converting shaders: n of m") instead of a half-drawn game.
  Converting all 1,437 takes under 5 seconds here, during the boot logos.
- **No D3D12 (or other host API) underneath.** With `host_presentation` off the GPU plugin
  creates its **null backend** (`src/graphics/null/`, added by the same common patch, also
  selectable as backend `null`): the shared command processor still executes the guest's GPU
  commands (register writes, events, fences, interrupts, swaps) and the vblank timer runs, but
  there is no device, presenter, shader translation or GPU resource, and draws and copies are
  dropped. So Vulkan-only mode creates no D3D12 device, and works the same on platforms
  without D3D12. Checked by logging loaded modules at each step: neither `d3d12.dll` nor
  `D3D12Core.dll` is loaded through ReXGlue's setup, the plugin's setup or the renderer's Vulkan
  instance and device creation. They appear at `vkCreateSwapchainKHR`, together with
  `dcomp.dll`: NVIDIA's Windows Vulkan driver presents through DXGI/DirectComposition (D3D12
  internally) by default, as for any Vulkan game. That is the driver's present path, not a
  second renderer; NVIDIA's control panel ("Vulkan/OpenGL present method") selects it.
- **Fallbacks for older SDKs:** without `host_presentation`, the picture goes into a child window
  over ReXGlue's (ReXGlue draws until the shaders are ready, then hands over); without
  `gpu_host_rendering`, the hooks skip the game's draw, clear and resolve calls. Both log a
  warning.
- **Known gaps:** the other `Draw*` variants still go to ReXGlue (none seen so far; a warning
  names any that appear). Anything that reads render results back on the CPU or through
  queries gets nothing. The window surface is only implemented for Windows so far.
- **SDK switches (`patches/common/rexglue-host-rendering.patch`, every platform and backend):**
  `host_presentation` (core, set at startup) keeps the plugin's presenter off the window, as
  above. `gpu_host_rendering` is a cvar (default true, changeable while running) in ReXGlue's shared
  command processor. When false, `ExecutePacketType3Draw` still consumes each draw packet and
  its register writes but does not call the backend's `IssueDraw`, which is also where the
  D3D12 and Vulkan backends perform copies (resolves and EDRAM clears); `XE_SWAP` skips
  `IssueSwap`, the presenter refresh. Everything else (events, fences, interrupts, swaps
  counting) runs as before. The game's own D3D code is untouched, so its state bookkeeping,
  fences and queries stay consistent. The renderer sets it with `SetFlagByName` at the
  takeover. With an SDK built before the patch, the hooks skip the game's draw, clear and
  resolve calls instead (a warning says so).
- Tested from the title screen into gameplay: the takeover came at guest frame 96 with a warm
  cache, input reached the game, and the guest held 60 FPS.
- **Known gaps:** ReXGlue's own overlays (ImGui dialogs, toasts) draw under the child window
  and can't be seen. The other `Draw*` variants still go to ReXGlue. Anything that reads
  render results back on the CPU or through queries (occlusion queries, screenshots) gets
  nothing from the skipped work. Windows only.

### Performance

Gameplay (Act I, ~380-600 draws per frame) first ran at about 25 FPS with the renderer on.
Timing in the renderer's 300-frame log line (capture time on the game thread, render time on
the renderer thread) and a per-phase capture breakdown (`shadow_snapshot: capture per frame`)
showed the game thread spending ~36 ms per frame capturing, converting ~1.4 million vertices
(~22 MB) every frame. The renderer itself took ~2.4 ms.

| Change | Capture per gameplay frame |
|---|---:|
| Before | ~36 ms |
| Persistent vertex blocks | ~10-12 ms |
| Cache a range only when seen again unchanged | ~9.5 ms |
| Recycled frame records, bulk constant copy | ~4.5 ms |

- **Vertex blocks:** converted vertices are cached across frames, keyed by stream address,
  stride, element layout and vertex range, and checked against the source bytes (whole up
  to 16 KiB, 64 sampled points above that). Draws reference a block id; the renderer packs
  blocks into 32 MiB host-visible chunks (first fit, merged free ranges; two Vulkan
  allocations in gameplay rather than thousands) and frees retired blocks after the frames in
  flight. Ranges are cached only on their second unchanged sighting, because dynamic geometry
  is written to a new address every frame; ranges that change in place twice are always
  sent inline.
- **Frame records** are handed back by the renderer and reused, so their vectors keep their
  memory instead of growing fresh pages every frame (this, not the byte swap, was the cost
  of the 8 KiB of constants per draw).
- **Textures:** lookups are remembered for the rest of the frame (cleared at each resolve),
  identical fetch constants reuse the previous draw's ids, and the content hash works on
  8-byte words.

The guest now holds 60 FPS in gameplay with either option. Capture still costs ~4.5 ms of
game thread time (indices 0.6, vertices 1.3, constants 0.3, textures and draw state 1.4),
and the renderer ~1.2-1.5 ms on its own thread. A sampled check can miss a partial in-place
edit of a large vertex range.

### Render resolution (test option, 2026-11-08)

`--shadow_render_height=<n>` draws at n lines instead of 720, with a fractional scale
`n / 720` (1.5 for 1080, 2 for 1440), not an integer multiple. Every guest pixel quantity is
scaled by it: render target size, viewports, the full-target scissor and render area, and
resolve rectangles (so resolved textures, the front buffer included, come out at the higher
resolution and are sampled by UV as before). The half-pixel offset stays in guest pixels,
which is correct at any scale. Pixel shaders that read their screen position (`vPos`, 15 of
787) would get host pixels, so the XenosRecomp fork now emits
`iPos.xy * g_ResolutionScaleInv - 0.5` (a new shared constant at `+276`, `1.0` at native
resolution); changing the converter rebuilt the shader cache automatically. Checked on the
title screen and main menu at 1080 and 1440 (60 FPS, sharper text and models); gameplay at
higher resolutions and the `vPos` shaders specifically are not checked yet. Effects whose
texel offsets the game computes for 720p (blurs) keep their 720p footprint in screen terms.

### Base disc support (2026-11-09)

Everything above was measured on the USA title update. The base disc links the same code at
other addresses: matching every function body with addresses normalized away showed the
Direct3D library 0x81C0 lower (all 3094 functions in its range, and every hooked one
identical apart from global data addresses) and the game's own code 0x3C30 lower (the
shader loaders, the display mode function, the renderer's draw call site). The hooks now
name functions through `guest_addresses.h`, which holds both sets, and both executables
build the experiment; `gen_range_hooks.py --base` writes the base disc's
`d3d_range_hooks_base.inc`. Device and structure offsets inside the library are unchanged.
Checked in Vulkan-only mode from a fresh profile: the title screen into the attract mode's
Act I gameplay, with every draw drawn once its shaders converted. The widescreen options'
hook runs on the base disc but wider shapes were not re-checked there.

### Textures that change in place (2026-11-08)

Cutscene video frames are decoded into the same textures every frame. The cache identified
textures by their contents, so each video frame became a new texture with a new image that was
never freed: VRAM grew through every cutscene (and would have slowly through texture streaming),
until the 4,096-id table filled. Large textures were also only checked at 16 points, which a
letterboxed or slowly panning video can leave unchanged, so stale frames were shown again.

- A texture is now also tracked by its identity (address and description). New contents for
  the same identity alternate between two ids, and the renderer copies into the existing image
  when its shape is unchanged. The other id's image is never the one the previous frame used;
  the renderer also records the last submission that sampled each id and waits if a rewrite
  would race it (only possible when it skipped a frame).
- Ids unused for 10 seconds are freed (checked every 5) and reused; the renderer releases
  their images. A dropped frame's uploads now go before the next frame's.
- Textures seen changing in place are hashed whole; other large ones at 256 points.
- Measured with idle runs through the attract cutscenes: dedicated VRAM stays flat (about
  225 MB at 720, 650 MB at 1440) and the live texture count stays constant during video.

### Side-by-side review findings (2026-11-08)

From captures in Act I (the Captain Daltyn fight in the cellar, New Tristram, the cathedral):

- **Alpha test functions 1 and 3 (less, less-equal)** were used in gameplay but ignored, so
  draws that should keep only low-alpha pixels drew everything (a likely cause of the
  elite ground ring under Captain Daltyn showing blue instead of orange). The XenosRecomp
  fork now clips with `(alpha - threshold) * g_AlphaTestDirection` (shared `+280`, +1 or -1)
  and the renderer sets it; "never" skips the draw. Equal and not-equal are still logged as
  unsupported.
- **Skipped draws are now logged** once per reason, primitive type and shader pair
  (`shadow_renderer: skipped a draw (...)`). A gameplay run had none outside the first
  seconds of shader conversion.
- **Blend constant (`RB_BLEND_RED`..`ALPHA`, `0x2105`-`0x2108`) was never passed** (always zero).
  Gameplay frames use it: draws blend with `060c060c` (source constant colour, destination
  source alpha) into the buffer at EDRAM base 0 before the main scene, which is resolved to a
  1024x1024 texture (likely the top-down ground-effect map the terrain samples: skill glows
  such as Grasp of the Dead, elite rings, ground swirls). Now captured per draw and set with
  `vkCmdSetBlendConstants`. The user's captures of the missing effects predate this fix.
  Rechecked by the user (run3): Grasp of the Dead's green cracks, hands and area glow are
  now drawn as in ReXGlue, only dimmer, and they fade out faster. That remaining difference is in
  intensity (suspect: ignored exponent bias on the HDR render targets), not missing draws.
- **Character shadows** under units are drawn by the Shadow renderer and missing in ReXGlue's
  D3D12 renderer; that is a ReXGlue issue (confirmed by the user), not a Vulkan one.
- **Still open (recheck with the blend constant fix):** large blue swirl or ring effects during
  the Captain Daltyn fight were missing in Vulkan, and bright glow effects (town portal, cathedral fire) come out brighter and fuller
  than in ReXGlue, which shows more of the scene through them. Suspects: render targets read
  through another format (EDRAM aliasing is not modelled) and the ignored exponent bias.
  Needs a log and capture from that fight.

### Wider than 16:9 (Hor+, 2026-11-09)

The 16:9 picture is the game's own choice, not only ReXGlue's letterboxing: `sub_82579690`
reads `XGetVideoMode` and picks 1280x720 for any HD widescreen mode (1024/852/768/640
otherwise), ignoring the real display width; `--video_mode_width=2560
--video_mode_height=1080` alone still gives a 1280x720 front buffer. It stores the size in a
52-byte display description (`+32` width, `+36` height, `+40` refresh, `+48` aspect float)
and the aspect again in the graphics manager at `+1216`.

`--shadow_guest_width=<n>` (Vulkan-only mode; `shadow_aspect.cpp`) wraps that function and
widens the width, scaling both aspect values to match. The engine follows completely: the
front buffer, render targets and resolves come out n wide, the 3D view is Hor+ (more of the
world at the sides, no stretching), and the HUD anchors to the new edges (orbs and skills
bottom-left, minimap bottom-right, quest tracker top-left). Checked at 1680 (21:9) on the
main menu and in New Tristram. The main menu's backdrop is a fixed 16:9 painting, so its
right edge smears past where the art ends. `--shadow_guest_height=<n>` does the
same for the height (both can be combined), and `--shadow_render_height` then scales from that
height instead of 720.

The engine keeps a fixed vertical field of view, so the view follows the aspect, not the
pixel count: 16:10 as 1152x720 or 1280x800 shows the same scene (sides trimmed, same as
the PC version, which players report "zooms in" at 16:10 rather than showing more
vertically), with the HUD scaled by height. 1280x800 is the better 16:10 choice (more pixels;
`--shadow_render_height=1600` is exactly 2x for 2560x1600). Checked 2026-11-09 in New Tristram.
Limits for now: read once at startup (no live resize); at most 2048 (the renderer's guest-space targets; 21:9 and 24:10 fit, 32:9 does
not); not applied side by side, where ReXGlue's 10 MiB EDRAM and its letterboxing still
assume the game's size.
Next:

1. **MSAA** (the menu scene is 2x), with real alpha to coverage; then EDRAM aliasing and
   exponent bias once a frame needs them.
2. **The other `Draw*` variants**, stencil, and the alpha test functions XenosRecomp lacks.
3. **The remaining texture types** (stacked, 3D mips).
4. **Gameplay:** check the stride assumption and the remaining vertex formats
   (`2_10_10_10`, `10_11_11`).
5. **Vulkan only:** the draw variants the game uses are captured, ReXGlue's rendering and
   presentation are switched off by a common SDK patch, and the renderer presents into the
   game window with ReXGlue's overlays. ReXGlue runs its null backend underneath (no
   D3D12). Next: window surfaces for Linux and macOS.
   Dynamic resolution and ultrawide come after that.
6. **Replace ReXGlue's presenter** once the new renderer matches it.

## Tools (`port/src/experiments/`)

- `--capture_shaders=<dir>` saves each shader at creation, named by header hash.
- `--trace_draws=true` counts calls to every function in the D3D library range, per frame,
  with callers. The hooks are generated by `gen_range_hooks.py`.
- `--trace_frame=<n>` logs every draw of frame *n*: arguments, bound shaders, the vertex
  declaration, the index buffer, vertex fetch constants and dirty masks.
- `--shadow_renderer=true` runs the Vulkan renderer in a second window beside ReXGlue's.
- `--shadow_renderer_only=true` draws with the Vulkan renderer alone, inside the game window
  (see "Vulkan only" below).
- Shaders convert automatically (see "Automatic shaders" below). `--shadow_shader_dir=<dir>`
  points the renderer at a hand-made folder of `<vs|ps>_<hash>.spv` instead, and
  `--shadow_shader_workers=<n>` sets how many XenosRecomp processes convert in parallel.
