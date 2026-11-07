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

## Status (2026-11-06)

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

- **Milestone 4 (in progress): render targets and resolves.**
  - **Working:** draws render into host images instead of the window. Each colour target
    is keyed by EDRAM base and format (`RB_COLOR_INFO`) and each depth buffer by its base
    (`RB_DEPTH_INFO`), all 2048x1536 in guest pixel coordinates. `D3DDevice_Resolve`
    (`sub_82D5B9D0`, hooked) copies them into host images that replace the destination
    textures. Depth resolves go through the arena into `R32_SFLOAT`. The window shows the
    frame's last resolve, which is the game's front buffer. All 68 draws and 21 resolves per
    frame run.
  - **Not right yet:** the 3D background and post-processing come out white or black
    instead of the castle scene.
  - **Known gaps:**
    - EDRAM aliasing: the game resolves base 0 as `16_16_16_16` while it also holds the
      `8_8_8_8` frame.
    - The game's clears aren't captured: depth is cleared to the far value of its first
      draw's test, and this scene uses reversed Z (GEQUAL).
    - Resolve and render-target exponent bias, and 2x MSAA, are ignored.
    - The 768x768 shadow map (resolve slot 1) is black.
  - **Findings so far:**
    - The scene draws (1-31) render, which is visible with blending disabled.
    - Their alpha is `diffuse.a * Factor.w` with `Factor = (1,1,1,1)`, and the BC3 alpha
      decode checks out, so the scene layers are semi-transparent by design.
    - The two debug views (resolve cycling and after-draw snapshots) disagree about the
      target after draw 44. That needs resolving first.

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
| `--shadow_debug_flags` | 1 depth test off, 2 blending off, 4 all colour writes on |
| `--shadow_debug_layout` | logs targets, state, textures and resolves per draw |
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
  and filter fields.
- **Not done yet:** mip levels, cube and 3D textures, stacked textures, vertex texture
  fetch, and formats such as `24_8_FLOAT` and `DXT3A`.

Next:

1. **Finish render targets:**
   - Explain why the two debug views disagree.
   - Model EDRAM aliasing: one EDRAM store per base, reinterpreted by format.
   - Capture the game's clears.
   - Apply exponent bias.
   - Get the castle scene to match.
2. **The other `Draw*` variants**, culling and stencil.
3. **Mip levels and the remaining texture types.**
4. **Gameplay:** check the stride assumption and the remaining vertex formats
   (`2_10_10_10`, `10_11_11`).
5. **Replace ReXGlue's presenter** once the new renderer matches it.

## Tools (title update builds, `port/src/experiments/`)

- `--capture_shaders=<dir>` saves each shader at creation, named by header hash.
- `--trace_draws=true` counts calls to every function in the D3D library range, per frame,
  with callers. The hooks are generated by `gen_range_hooks.py`.
- `--trace_frame=<n>` logs every draw of frame *n*: arguments, bound shaders, the vertex
  declaration, the index buffer, vertex fetch constants and dirty masks.
- `--shadow_renderer=true` with `--shadow_shader_dir=<dir>` runs the side-by-side renderer.
