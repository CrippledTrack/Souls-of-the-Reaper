// Experiment (Shadow renderer): side-by-side captures for finding differences from ReXGlue.
//
// With --shadow_renderer=true (both renderers running), F9 or --shadow_compare_every=<s>
// saves the Shadow renderer's finished frame and ReXGlue's (its presenter's guest output)
// to <user data>/shadow_compare (or --shadow_compare_dir):
//   <n>_rexglue.png, <n>_vulkan.png, and <n>_compare.png (ReXGlue | Vulkan | difference,
//   the difference amplified 4x at ReXGlue's size).
// The two renderers run independently, so the images can be a frame apart; pause the game
// (the Start menu freezes the world) for exact comparisons.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace d3::shadow::compare {

struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // tightly packed R8G8B8A8
};

// Writes the images and logs how much they differ. vulkan is required; rexglue may be empty
// (Vulkan-only mode has no ReXGlue picture). Runs on a background thread.
void SaveAsync(const std::filesystem::path &dir, uint64_t guest_frame, Image vulkan,
               Image rexglue);

}  // namespace d3::shadow::compare
