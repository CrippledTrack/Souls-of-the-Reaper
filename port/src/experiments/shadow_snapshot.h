// Experiment (gpu-layer-experiment branch): builds a FrameRecord from the game's draw calls.
#pragma once

#include "shadow_frame.h"
#include "shadow_textures.h"

#include <cstdint>
#include <memory>

namespace d3::shadow {

// Used on the game's render thread only: draws and presents come from the same thread.
class SnapshotBuilder {
 public:
  // sub_82D621B0(device, primitive, base vertex, start index, index count).
  void RecordIndexedDraw(const uint8_t *base, uint32_t device, uint32_t primitive,
                         uint32_t base_vertex, uint32_t start_index, uint32_t index_count);
  // D3DDevice_Resolve(device, flags, rect, destination texture, ...), before it runs.
  void RecordResolve(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t texture);
  // Ends the frame and hands its draws over.
  std::unique_ptr<FrameRecord> Finish(uint64_t guest_frame);

 private:
  std::unique_ptr<FrameRecord> frame_;
  TextureCache textures_;
};

}  // namespace d3::shadow
