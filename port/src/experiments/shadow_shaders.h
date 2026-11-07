// Experiment (gpu-layer-experiment branch): automatic shaders for the Vulkan renderer.
//
// The game's shaders can't be shipped, so they are converted on the player's machine. Every
// shader the game creates is saved (shader_capture.cpp) to <user data>/shader_cache/xenos,
// and background workers run the bundled XenosRecomp (<exe folder>/xenosrecomp) on the new
// ones, writing SPIR-V to <user data>/shader_cache/spirv. The renderer loads from there and
// retries missing shaders whenever a batch lands. A stamp of the converter build is kept
// with the SPIR-V, so a new XenosRecomp reconverts everything.
//
// --shadow_shader_dir=<dir> still points the renderer at a hand-made folder instead.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace d3::shadow::shaders {

// Starts capture into <root>/xenos and conversion into <root>/spirv.
void Start(const std::filesystem::path &root);
void Stop();

// From the shader creation hooks: a complete container (header, then the physical part),
// named by the hash of its header. Buffered until Start().
void OnShaderCreated(bool vertex, uint64_t header_hash, std::vector<uint8_t> container);

// Where the renderer loads <vs|ps>_<hash>.spv from.
std::filesystem::path SpirvDir();

// Changes every time converted shaders land; the renderer retries missing ones then.
uint32_t Generation();

// True once every shader seen so far has been converted (or failed), and the startup scan
// is done. With a hand-made --shadow_shader_dir it is always true.
bool Ready();

// For a progress display: shaders converted so far this run, and still waiting.
void Progress(uint32_t *converted, uint32_t *remaining);

}  // namespace d3::shadow::shaders
