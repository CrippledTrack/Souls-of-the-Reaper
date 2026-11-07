# Custom graphics layer: experiment notes

This is a private experiment on branch `gpu-layer-experiment`, which has not been pushed.

The goal is to replace ReXGlue's emulated Xenos GPU with a layer like Unleashed
Recompiled's. That layer hooks the game's Direct3D calls and draws through a native
renderer. Its shaders are converted ahead of time by [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)
(MIT). Diablo III needs a fork of it, which lives locally at `tools/XenosRecomp`
(git-ignored like the SDK) on branch `diablo3` with pushing disabled. Its changes are kept
here as `xenosrecomp-diablo3.patch`.

## 1. Getting the shaders

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

Each hook writes the header followed by the physical part, which is the same layout as a
container on disk. It does this before the init runs, so vertex shaders are still
unpatched.

```
diablo3.exe ... --capture_shaders=<dir>
```

The 2026-11-06 capture at the title screen:

- **1,437 unique shaders:** 650 vertex and 787 pixel, exactly the number of shader objects
  the game holds.
- **All 1,437 pass a control-flow sanity check** (`check_containers.py`).
- **Pixel shaders match the GPU byte for byte:** 110 of the 111 pixel shaders the GPU
  executed (ReXGlue's `--dump_shaders`) are identical to a capture (`match_ucode.py`).
  Vertex shaders differ, as expected, because the GPU only sees patched copies.
- **The whole set appears to load at startup.** Gameplay hasn't been captured yet, so it
  isn't proven complete.

## 2. XenosRecomp on the real shaders

`run_batch.py` sends every shader through HLSL → DXIL and HLSL → SPIR-V.

| Step | Vertex | Pixel | Notes |
|---|---:|---:|---|
| Upstream `990d03b` | 438 / 650 | 545 / 787 | 983 of 1,437 (68%). No crashes or asserts. |
| + FOG interpolator | | | Diablo III passes fog in `FOG0`, missing from XenosRecomp's fixed list (454 shaders). |
| + pixel boolean constants | 650 / 650 | 787 / 787 | Pixel shaders address booleans as `b128`–`b255`; the constant table numbers them from 0 (12 shaders). |

**Every shader now compiles to DXIL and SPIR-V.** Compiling doesn't prove the output is
right, though. The rendering is untested.

Two control-flow fixes found on the main menu (see RENDERING.md, milestone 5): backward
jumps are detected by target address rather than the direction bit (346 shaders now use the
general `pc`/`switch` form), and `CondExecPred` blocks are wrapped in their predicate check.

`XenosRecomp --spirv <list> <out> <header>` converts SPIR-V only (no HLSL or DXIL files) and is
what the game runs on the player's machine (RENDERING.md, "Automatic shaders").

Other fork changes are for testing only:
- a failed assert throws instead of aborting;
- an unknown vertex fetch reports its address;
- `XenosRecomp --batch <list> <out> <header>` mode.

## 3. Known gaps before anything renders

- **Unleashed assumptions in the generated code are still there.** Texcoord swizzling
  (`g_SwappedTexcoords`), packed-normal decoding (`tfetchR11G11B10`), and the hardcoded
  Vulkan input locations (`USAGE_LOCATIONS`) all need checking against Diablo III's vertex
  formats.
- **XenosRecomp ignores the formats in vertex fetches.** The renderer has to build native
  input layouts from the game's vertex declarations at draw time.
- **The Direct3D side hasn't been started.** The library sits around
  `0x82D5xxxx`–`0x82D8xxxx`:
  - `sub_82D58DD8` presents (calls `VdSwap`);
  - `sub_82D5E328` sets up the ring buffer;
  - `sub_82D69128` / `sub_82D69398` create shaders from a whole container (not used by the
    game's loader);
  - `sub_82D660F8` returns a container's sizes.

  A layer has to identify and hook the device, state, resource and draw entry points.

## Reproducing

1. Build the update executable (`python scripts/build_windows.py --title-update usa --game-dir game --skip-codegen`).
2. Run it with `--capture_shaders=<dir>` until the title screen.
3. In `tools/XenosRecomp`, check out `990d03b` and run `git apply ../../experiments/gpu-layer/xenosrecomp-diablo3.patch`.
4. Build with clang-cl and Ninja.
5. Run `python -I run_batch.py <XenosRecomp.exe> <XenosRecomp/shader_common.h> <dir> <out> 4`.

Run the batch mode rather than one process per shader. Launching thousands of copies of a
freshly built, unsigned exe got it blocked by Windows Defender.

Captured shaders are game data and are not stored in the repository.
