// Experiment (gpu-layer-experiment branch): Xenos texture decoding for the side-by-side renderer.
#pragma once

#include "shadow_frame.h"

#include <cstdint>
#include <set>
#include <unordered_map>

namespace d3::shadow {

// Fast 64-bit content hash (8 bytes per step), used for texture and vertex identity.
uint64_t HashBytes(const void *data, size_t size, uint64_t hash = 0xCBF29CE484222325ull);

// Game render thread only. Ids are stable for the life of the renderer: texture id 0 and
// sampler id 0 are the renderer's placeholders.
class TextureCache {
 public:
  // Id of the texture a fetch constant describes, decoding it into frame.new_textures the
  // first time; 0 when it is not a 2D texture in a supported format.
  uint16_t Texture(const uint8_t *base, const uint32_t fetch[6], FrameRecord &frame);
  // Called at the end of every guest frame: lookups are remembered for the rest of a frame,
  // so each texture's contents are hashed once per frame rather than once per draw.
  void EndFrame() { frame_memo_.clear(); }
  // Called once per guest frame when it is handed over: frees textures unused for a while
  // into frame.freed_textures.
  void FinishFrame(FrameRecord &frame);
  // Id of the sampler state (clamp and filter fields) of a fetch constant.
  uint16_t Sampler(const uint32_t fetch[6], FrameRecord &frame);
  // Physical base address -> resolve slot, for textures the game fills by resolving.
  std::unordered_map<uint32_t, uint32_t> resolve_targets;

 private:
  uint16_t FindOrDecode(const uint8_t *base, const uint32_t fetch[6], FrameRecord &frame);
  uint16_t AllocateId(int kind);  // 0 2D, 1 3D, 2 cube; 0 when the table is full
  void ForgetId(uint16_t id);

  // A texture is found by its contents (textures_: content key -> id). The same texture
  // (address and description, "identity") with new contents, such as cutscene video frames,
  // alternates between two ids whose images are updated in place, so an image is never
  // rewritten while the previous frame may still draw with it.
  struct Live {
    uint16_t ids[2] = {};
    uint8_t current = 0;
    bool dynamic = false;  // seen changing in place: its contents are hashed whole
  };
  struct IdInfo {
    uint64_t content = 0, identity = 0;
    uint64_t last_used = 0;
  };
  std::unordered_map<uint64_t, uint16_t> textures_;
  std::unordered_map<uint64_t, Live> live_;
  std::unordered_map<uint16_t, IdInfo> ids_;
  std::vector<uint16_t> free_ids_[3];
  uint64_t frame_number_ = 0;
  std::unordered_map<uint64_t, uint16_t> frame_memo_;  // whole fetch constant -> id, this frame
  std::unordered_map<uint32_t, uint16_t> samplers_;
  std::set<uint32_t> unsupported_formats_;
  uint32_t next_texture_ = 1;
  uint32_t next_texture_3d_ = 1;
  uint32_t next_texture_cube_ = 1;
  uint32_t next_sampler_ = 1;
  bool full_warned_ = false;
};

}  // namespace d3::shadow
