// Experiment (gpu-layer-experiment branch): builds a FrameRecord from the game's draw calls.
#pragma once

#include "shadow_frame.h"
#include "shadow_textures.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace d3::shadow {

// Frame records are recycled: the renderer hands finished ones back so the next frames reuse
// their memory instead of growing fresh vectors (and faulting in fresh pages) every frame.
std::unique_ptr<FrameRecord> NewFrameRecord();
void RecycleFrameRecord(std::unique_ptr<FrameRecord> frame);

// Used on the game's render thread only: draws and presents come from the same thread.
class SnapshotBuilder {
 public:
  // sub_82D621B0(device, primitive, base vertex, start index, index count).
  void RecordIndexedDraw(const uint8_t *base, uint32_t device, uint32_t primitive,
                         uint32_t base_vertex, uint32_t start_index, uint32_t index_count) {
    RecordDraw(base, device, primitive, base_vertex, start_index, index_count, true);
  }
  // sub_82D61D98(device, primitive, start vertex, vertex count): non-indexed; vertices are
  // read in order from the start vertex.
  void RecordAutoIndexDraw(const uint8_t *base, uint32_t device, uint32_t primitive,
                           uint32_t start_vertex, uint32_t vertex_count) {
    RecordDraw(base, device, primitive, start_vertex, 0, vertex_count, false);
  }
  // D3DDevice_Resolve(device, flags, rect, destination texture, ...), before it runs.
  void RecordResolve(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t texture);
  // D3DDevice_Clear(device, count, rects, flags, colour, z, stencil), before it runs.
  void RecordClear(const uint8_t *base, uint32_t device, uint32_t flags, uint32_t color,
                   float depth, uint32_t stencil);
  // Ends the frame and hands its draws over.
  std::unique_ptr<FrameRecord> Finish(uint64_t guest_frame);

 private:
  void RecordDraw(const uint8_t *base, uint32_t device, uint32_t primitive, uint32_t base_vertex,
                  uint32_t start_index, uint32_t index_count, bool indexed);

  // A vertex range seen before: the block holding its converted vertices, a signature of
  // the source bytes, and how often the bytes changed (ranges that keep changing, such as
  // UI and particles, are converted inline every frame instead of cached).
  struct CachedVertices {
    uint32_t block = 0;
    uint64_t signature = 0;
    uint32_t changes = 0;
    uint64_t last_used = 0;
    bool seen = false;  // converted inline once; cached when seen again unchanged
  };

  // Texture fetch constant of each slot at the previous draw, and the ids it gave.
  struct SlotMemo {
    uint32_t fetch[6] = {};
    uint16_t texture = 0, sampler = 0;
    bool valid = false;
  };

  std::unique_ptr<FrameRecord> frame_;
  TextureCache textures_;
  SlotMemo slot_memo_[kTextureSlots];
  std::unordered_map<uint64_t, CachedVertices> vertex_cache_;
  uint32_t next_vertex_block_ = 1;
  uint64_t frame_number_ = 0;
  std::vector<uint32_t> freed_blocks_;  // waiting for the next frame record
};

}  // namespace d3::shadow
