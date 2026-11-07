# Shadow renderer: an experimental Vulkan renderer

The Shadow renderer draws Diablo III with Vulkan instead of ReXGlue's emulated Xenos GPU,
in the style of Unleashed Recompiled's graphics layer. It hooks the game's Direct3D calls,
captures each frame's draws, and replays them through shaders converted by a fork of
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (MIT). The name comes from its
first milestone, when it only ran in the shadow of ReXGlue's renderer, drawing the same
frames side by side.

It is experimental. It has been tested on Windows only, with the verified USA title update
build only (the hooks use that executable's addresses), from the title screen through the
main menu, the inventory and early Act I gameplay.

## Using it

Both modes are off unless asked for on the command line:

| Option | What it does |
|---|---|
| `--shadow_renderer=true` | Opens a second window drawn by the Shadow renderer, beside ReXGlue's normal one. Useful for comparing the two. |
| `--shadow_renderer_only=true` | Draws the game window with the Shadow renderer alone. ReXGlue keeps running the game's GPU command stream on a null backend (no D3D12 or other host graphics API) and its overlays (settings, console, debug, toasts) are drawn by the Shadow renderer. The window title ends in "Vulkan Experimental". |
| `--shadow_shader_dir=<dir>` | Uses a hand-made folder of `<vs\|ps>_<hash>.spv` instead of the automatic conversion. |
| `--shadow_shader_workers=<n>` | Number of XenosRecomp processes converting in parallel (default: half the CPU threads, at most 6). |

For example, from the title update build folder:

```
diablo3.exe --game_data_root "<game>" --update_data_root "<game>\update" --user_data_root "<saves>" --shadow_renderer_only=true
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
patch set changes); and the XenosRecomp fork, built by hand as described below.

## Building the shader converter

The XenosRecomp fork is not stored in this repository, only its changes
(`xenosrecomp-diablo3.patch`). Without it the game still captures shaders but cannot convert
them, and the Shadow renderer draws nothing.

**This is a manual step for now.** Unlike the ReXGlue SDK patches, nothing applies the
XenosRecomp patch for you: the build scripts and the launcher do not fetch, patch or build
XenosRecomp, so it has to be done by hand once (and again whenever the patch changes). From
the repository root, in a Visual Studio developer shell with LLVM's clang-cl on `PATH`:

```
git clone --recursive https://github.com/hedge-dev/XenosRecomp tools/XenosRecomp
cd tools/XenosRecomp
git checkout 990d03b
git submodule update --init --recursive
git apply ../../experiments/gpu-layer/xenosrecomp-diablo3.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --target XenosRecomp
```

Then rebuild the title update executable (`python scripts/build_windows.py --title-update usa
--skip-codegen`). The build copies `XenosRecomp.exe`, its DXC libraries and `shader_common.h`
into `<build>/xenosrecomp/` when `tools/XenosRecomp/build` exists.

## Status

[RENDERING.md](RENDERING.md) has the full design notes, milestone by milestone, including
the performance work and the debug options. In short:

- **Matches ReXGlue** on the title screen, main menu, inventory and early gameplay, except
  for edge aliasing: the game's 2x MSAA is drawn at 1x.
- **60 FPS in gameplay** in both modes. Capturing a frame costs about 4.5 ms on the game
  thread; static meshes are converted once and kept on the GPU.
- **Not done yet:** MSAA, stencil, a few alpha-test functions, stacked textures and 3D
  mipmaps, window surfaces for Linux and macOS, dynamic resolution and ultrawide.
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

`port/src/experiments/shader_capture.cpp` fixes this. It is compiled into title update
builds only, because the addresses are for the USA update executable. It hooks the
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
