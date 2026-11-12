# Shadow renderer: a Vulkan renderer (beta)

The Shadow renderer draws Diablo III with Vulkan instead of ReXGlue's emulated Xenos GPU,
in the style of Unleashed Recompiled's graphics layer. It hooks the game's Direct3D calls,
captures each frame's draws, and replays them through shaders converted by a fork of
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (MIT). The name comes from its
first milestone, when it only ran in the shadow of ReXGlue's renderer, drawing the same
frames side by side.

It is in beta, and part of every build since it moved out of a separate experimental
profile. It has been tested mostly on Windows (on Linux, under Wayland, only the
title screen has been run so far, in both modes; on macOS, through MoltenVK on Apple
silicon, only `--shadow_renderer_only=true` has been run, from the intro into early Act I
gameplay, built with `scripts/build_macos.py`), with the verified USA title update
(title screen through the main menu, the inventory and early Act I gameplay) and the base
disc (title screen into early Act I gameplay). The hooks use each executable's own addresses
(`port/src/experiments/guest_addresses.h`); other executables are not supported.

## Using it

The Shadow renderer is compiled into every build (`SOULS_SHADOW_RENDERER` in
`port/CMakeLists.txt`). In the launcher it is the **Shadow (beta)** choice under Render Path
(Linux) or Renderer (Windows), offered when the build's shader converter is staged beside it.
On macOS it is the only renderer: the launcher always passes it and shows no choice.

Both modes are off unless asked for on the command line, so a normal launch is unaffected:

| Option | What it does |
|---|---|
| `--shadow_renderer=true` | Opens a second window drawn by the Shadow renderer, beside ReXGlue's normal one. Useful for comparing the two. |
| `--shadow_renderer_only=true` | Draws the game window with the Shadow renderer alone. ReXGlue keeps running the game's GPU command stream on a null backend (no D3D12 or other host graphics API) and its overlays (settings, console, debug, toasts) are drawn by the Shadow renderer. The window title ends in "Vulkan Experimental". |
| `--shadow_render_height=<n>` | Test option: draws the game at another vertical resolution, e.g. `1080` or `1440` (0, the default, is the game's own 720). Any value works: everything is scaled by n / 720. Read at startup. |
| `--shadow_dynamic_resolution=true` | Lowers the vertical resolution of the 3D picture while the GPU cannot keep up with the game's frame rate and raises it again when it can; `--shadow_render_height` is then the most it draws at. The last stretch of each frame, where the game composites its 3D picture and draws the interface, stays at the full resolution, so text and menus do not change. `--shadow_dynamic_min_height=<n>` sets the floor (default: the game's own height; never below 720 lines). Needs GPU timestamp queries. Read at startup. |
| `--shadow_guest_width=<n>` | Vulkan-only test option: sets the width of the game's own display (default 1280), e.g. `1680` for 21:9 at 720 lines; wider than 16:9 shows more at the sides and the HUD moves to the window edges. At most 2048. Read at startup. |
| `--shadow_guest_height=<n>` | Vulkan-only test option: sets the height of the game's own display (default 720), e.g. `800` for 16:10 at 1280 wide. Like the PC version, a taller shape trims the sides rather than showing more above and below. `--shadow_render_height` scales from this height. At most 1536. Read at startup. |
| **F9** / `--shadow_compare_every=<s>` | Side-by-side capture (with `--shadow_renderer=true`): saves ReXGlue's frame, the Shadow renderer's, and a combined ReXGlue \| Vulkan \| difference image (difference brightened 4x) to `<user data>/shadow_compare` (`--shadow_compare_dir`), and logs how much they differ. The two renderers run independently, so pause the game for exact comparisons, and give both the same resolution. |
| `--shadow_shader_dir=<dir>` | Uses a hand-made folder of `<vs\|ps>_<hash>.spv` instead of the automatic conversion. |
| `--shadow_shader_workers=<n>` | Number of XenosRecomp processes converting in parallel (default: half the CPU threads, at most 6). |

For example, from the title update build folder:

```
diablo3.exe --game_data_root "<game>" --update_data_root "<game>\update" --user_data_root "<saves>" --shadow_renderer_only=true
```

or from the base disc build folder (no update data root):

```
diablo3.exe --game_data_root "<game>" --user_data_root "<saves>" --shadow_renderer_only=true
```

**Shaders convert automatically.** The game's shaders can't be shipped, so every shader the
game creates is saved to `<user data>/shader_cache/xenos` and converted in the background by
the XenosRecomp build staged next to the executable (`xenosrecomp/`), into
`<user data>/shader_cache/spirv`. On a first run in Vulkan-only mode the window shows a
progress box until the conversion finishes (a few seconds). The cache is rebuilt
automatically when the converter changes.

**Requirements:** a Vulkan 1.3 GPU with buffer device address, descriptor indexing,
`shaderInt64` and dynamic rendering; an SDK built with
`patches/common/rexglue-host-rendering.patch` (the build scripts rebuild the SDK when the
patch set changes); and the XenosRecomp fork, which the build makes for you (below).

## Building the shader converter

The XenosRecomp fork is not stored in this repository, only its changes
(`xenosrecomp-diablo3.patch`). Without it the game still captures shaders but cannot convert
them, and the Shadow renderer draws nothing.

**The build does this for you.** `python scripts/build_client.py ...` (what the launcher
runs) treats the converter like the ReXGlue SDK: a pinned component built once, after the
SDK and before the game ("Build shader converter (one-time)"). It fetches the commit pinned
in `xenosrecomp.lock.json` with its submodules (about 0.5 GB, including prebuilt DXC),
applies the patch and builds it, and is refused by `--no-sdk-download` until the download is
approved. A failure fails the build. It is rebuilt when the pin or the patch changes, and
every game build gets it copied into `<build>/xenosrecomp/`. `build_linux.py`,
`build_windows.py` and `build_macos.py` on their own do not build it; they stage it when
`tools/XenosRecomp/build` exists. On macOS `build_client.py` (and so the launcher) builds it too.

To do it by hand instead (note that an automatic build resets `tools/XenosRecomp` to the
pinned commit plus the patch, discarding local edits there): from
the repository root, on Windows in a Visual Studio developer shell with LLVM's clang-cl on
`PATH` (on Linux use `clang` and `clang++` for the two compiler options instead):

```
git clone --recursive https://github.com/hedge-dev/XenosRecomp tools/XenosRecomp
cd tools/XenosRecomp
git checkout 990d03b
git submodule update --init --recursive
git apply ../../experiments/gpu-layer/xenosrecomp-diablo3.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --target XenosRecomp
```

Then rebuild the executable (`python scripts/build_windows.py --skip-codegen` for the base
disc, with `--title-update usa` for the title update; `scripts/build_linux.py` on Linux).
The build copies `XenosRecomp.exe` (`XenosRecomp` on Linux), its DXC libraries and
`shader_common.h` into `<build>/xenosrecomp/` when `tools/XenosRecomp/build` exists.

## Status

[RENDERING.md](RENDERING.md) has the full design notes, milestone by milestone, including
the performance work and the debug options. In short:

- **Matches ReXGlue** on the title screen, main menu, inventory and early gameplay, except
  for edge aliasing: the game's 2x MSAA is drawn at 1x.
- **60 FPS in gameplay** in both modes. Capturing a frame costs about 4.5 ms on the game
  thread; static meshes are converted once and kept on the GPU.
- **Not done yet:** MSAA, stencil, a few alpha-test functions, stacked textures and 3D
  mipmaps and ultrawide beyond about 2.84:1. Dynamic
  resolution is a first version: it is driven by GPU time only, has been tried on the title
  screen and menus (not yet through gameplay), and a resolution change recreates the
  resolve images, which costs a moment each time.
- **Draw coverage:** the game has been seen drawing through two of its 14 Direct3D draw
  functions, both captured. The others log a warning the first time they are called.

## Background: getting the shaders

### What didn't work

My first attempt scanned memory at the title screen for container headers
(magic `0x102A11xx`). Those dumps were wrong. Diablo III never holds a complete container
in memory:

- **Header and microcode are stored apart.** The loader copies each header out of a resource
  record and the physical part (literal constants plus microcode) out of a separate stream,
  straight into GPU memory.
- **Vertex shaders are rewritten afterwards.** D3D patches them once per vertex layout, so
  the microcode the GPU receives doesn't match the original.

The bytes after each scanned header were unrelated data. The XenosRecomp numbers from that
first test (12% passing, crashes, out-of-memory errors) measured that junk, not XenosRecomp.

### Capture at creation

`port/src/experiments/shader_capture.cpp` fixes this. Its addresses come from
`guest_addresses.h` (the names here are the title update's). It hooks the
Direct3D in-place init functions that the game's loader (`sub_82585100` / `sub_82585280`)
calls with the D3D object and the GPU copy:

| Function | Shader | Header position in the object |
|---|---|---|
| `sub_82D692A0(object, physical)` | vertex | +872, after 832 bytes of per-layout patch tables |
| `sub_82D66188(object, physical)` | pixel | +40 |

Each hook saves the header followed by the physical part, which is the same layout as a
container on disk. It does this before the init runs, so vertex shaders are still
unpatched. The Shadow renderer uses these captures automatically; `--capture_shaders=<dir>`
also writes them to a folder of your choice.

The 2026-11-06 capture at the title screen:

- **1,437 unique shaders:** 650 vertex and 787 pixel, exactly the number of shader objects
  the game holds.
- **All 1,437 pass a control-flow sanity check** (`check_containers.py`).
- **Pixel shaders match the GPU byte for byte:** 110 of the 111 pixel shaders the GPU
  executed (ReXGlue's `--dump_shaders`) are identical to a capture (`match_ucode.py`).
  Vertex shaders differ, as expected, because the GPU only sees patched copies.
- **The whole set loads at startup.** Gameplay so far has not created any new shaders.

### XenosRecomp on the real shaders

| Step | Vertex | Pixel | Notes |
|---|---:|---:|---|
| Upstream `990d03b` | 438 / 650 | 545 / 787 | 983 of 1,437 (68%). No crashes or asserts. |
| + FOG interpolator | | | Diablo III passes fog in `FOG0`, missing from XenosRecomp's fixed list (454 shaders). |
| + pixel boolean constants | 650 / 650 | 787 / 787 | Pixel shaders address booleans as `b128`–`b255`; the constant table numbers them from 0 (12 shaders). |

Every shader compiles to DXIL and SPIR-V. Two control-flow fixes came from the main menu
(RENDERING.md, milestone 5): backward jumps are detected by target address rather than the
direction bit, which restores the per-light loop in 346 shaders, and `CondExecPred` blocks
are wrapped in their predicate check.

Other fork changes:
- `XenosRecomp --spirv <list> <out> <header>` converts to SPIR-V only; this is what the
  game runs on the player's machine.
- `XenosRecomp --batch <list> <out> <header>` also writes HLSL and DXIL (used by
  `run_batch.py`), and `--hlsl <in.hlsl> <out.spv> <ps|vs>` recompiles an edited HLSL file
  for shader debugging.
- `XENOSRECOMP_CF_COMMENTS=1` labels each control-flow instruction in the HLSL output.
- A failed assert throws instead of aborting, and an unknown vertex fetch reports its
  address.

## Tools in this folder

- `run_batch.py <XenosRecomp.exe> <shader_common.h> <capture dir> <out dir> [workers]`
  converts a capture folder by hand and summarises the results. Use the batch mode rather
  than one process per shader: launching thousands of copies of a freshly built, unsigned
  exe got it blocked by Windows Defender.
- `check_containers.py` and `match_ucode.py` validate captures.
- `gen_range_hooks.py` regenerates the Direct3D range hooks used by `--trace_draws`.

Captured shaders are game data and are not stored in the repository.
