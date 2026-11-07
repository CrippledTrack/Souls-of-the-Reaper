// Experiment (gpu-layer-experiment branch): Xenos texture decoding for the side-by-side renderer.
#pragma once

#include "shadow_frame.h"

#include <cstdint>
#include <set>
#include <unordered_map>

namespace d3::shadow {

// Game render thread only. Ids are stable for the life of the renderer: texture id 0 and
// sampler id 0 are the renderer's placeholders.
class TextureCache {
 public:
  // Id of the texture a fetch constant describes, decoding it into frame.new_textures the
  // first time; 0 when it is not a 2D texture in a supported format.
  uint16_t Texture(const uint8_t *base, const uint32_t fetch[6], FrameRecord &frame);
  // Id of the sampler state (clamp and filter fields) of a fetch constant.
  uint16_t Sampler(const uint32_t fetch[6], FrameRecord &frame);
  // Physical base address -> resolve slot, for textures the game fills by resolving.
  std::unordered_map<uint32_t, uint32_t> resolve_targets;

 private:
  std::unordered_map<uint64_t, uint16_t> textures_;
  std::unordered_map<uint32_t, uint16_t> samplers_;
  std::set<uint32_t> unsupported_formats_;
  uint32_t next_texture_ = 1;
  uint32_t next_sampler_ = 1;
  bool full_warned_ = false;
};

}  // namespace d3::shadow
