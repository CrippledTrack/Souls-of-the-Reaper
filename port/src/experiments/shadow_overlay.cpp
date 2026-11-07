// Experiment (gpu-layer-experiment branch): records ReXGlue's ImGui overlays for the Vulkan
// renderer. See shadow_overlay.h.
#include "experiments/shadow_overlay.h"

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace d3::shadow::overlay {
namespace {

std::mutex mutex;
std::shared_ptr<const Frame> latest;
std::vector<Texture> created_textures;
std::vector<uint32_t> released_textures;
std::atomic<uint32_t> next_texture{1};
std::atomic<bool> request_pending{false};
rex::ui::ImGuiDrawer *imgui_drawer = nullptr;  // UI thread
rex::ui::ImmediateDrawer *recorder = nullptr;  // UI thread

class RecordedTexture : public rex::ui::ImmediateTexture {
 public:
  RecordedTexture(uint32_t width, uint32_t height, uint32_t id)
      : ImmediateTexture(width, height), id(id) {}
  ~RecordedTexture() override {
    std::lock_guard lock(mutex);
    released_textures.push_back(id);
  }
  const uint32_t id;
};

class Recorder : public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter,
                                                           bool is_repeated,
                                                           const uint8_t *data) override {
    if (!data || !width || !height)
      return nullptr;
    Texture texture;
    texture.id = next_texture.fetch_add(1);
    texture.width = width;
    texture.height = height;
    texture.linear = filter == rex::ui::ImmediateTextureFilter::kLinear;
    texture.repeat = is_repeated;
    texture.rgba.assign(data, data + size_t(width) * height * 4);
    const uint32_t id = texture.id;
    {
      std::lock_guard lock(mutex);
      created_textures.push_back(std::move(texture));
    }
    return std::make_unique<RecordedTexture>(width, height, id);
  }

  void Begin(rex::ui::UIDrawContext &context, float coordinate_space_width,
             float coordinate_space_height) override {
    ImmediateDrawer::Begin(context, coordinate_space_width, coordinate_space_height);
    frame_ = std::make_shared<Frame>();
    frame_->target_width = context.render_target_width();
    frame_->target_height = context.render_target_height();
    const float w = coordinate_space_width > 0 ? coordinate_space_width
                                               : float(context.render_target_width());
    const float h = coordinate_space_height > 0 ? coordinate_space_height
                                                : float(context.render_target_height());
    frame_->coordinate_size_inv[0] = w > 0 ? 1.0f / w : 0.0f;
    frame_->coordinate_size_inv[1] = h > 0 ? 1.0f / h : 0.0f;
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch &batch) override {
    if (!frame_)
      return;
    batch_vertex_base_ = uint32_t(frame_->vertices.size());
    batch_index_base_ = uint32_t(frame_->indices.size());
    const size_t at = frame_->vertices.size();
    frame_->vertices.resize(at + size_t(batch.vertex_count));
    static_assert(sizeof(Vertex) == sizeof(rex::ui::ImmediateVertex));
    if (batch.vertex_count)
      std::memcpy(&frame_->vertices[at], batch.vertices,
                  size_t(batch.vertex_count) * sizeof(Vertex));
    if (batch.indices && batch.index_count)
      frame_->indices.insert(frame_->indices.end(), batch.indices,
                             batch.indices + batch.index_count);
    batch_indexed_ = batch.indices && batch.index_count;
  }

  void Draw(const rex::ui::ImmediateDraw &draw) override {
    if (!frame_ || draw.count <= 0)
      return;
    Command command;
    if (!ScissorToRenderTarget(draw, command.scissor[0], command.scissor[1], command.scissor[2],
                               command.scissor[3]))
      return;  // nothing visible
    command.texture = draw.texture ? static_cast<RecordedTexture *>(draw.texture)->id : 0;
    command.lines = draw.primitive_type == rex::ui::ImmediatePrimitiveType::kLines;
    command.indexed = batch_indexed_;
    command.count = uint32_t(draw.count);
    command.first_index = batch_index_base_ + uint32_t(draw.index_offset);
    command.base_vertex = int32_t(batch_vertex_base_) + draw.base_vertex;
    frame_->commands.push_back(command);
  }

  void EndDrawBatch() override {}

  void End() override {
    ImmediateDrawer::End();
    finished_ = std::move(frame_);
  }

  // Around one ImGuiDrawer::Draw: whatever it recorded (or nothing) becomes the latest frame.
  void BeginUIFrame() { finished_.reset(); }
  void FinishUIFrame() {
    std::lock_guard lock(mutex);
    latest = std::move(finished_);
  }

 private:
  std::shared_ptr<Frame> frame_, finished_;
  uint32_t batch_vertex_base_ = 0, batch_index_base_ = 0;
  bool batch_indexed_ = false;
};

}  // namespace

std::unique_ptr<rex::ui::ImmediateDrawer> CreateRecorder() {
  auto created = std::make_unique<Recorder>();
  recorder = created.get();
  return created;
}

void SetImGuiDrawer(rex::ui::ImGuiDrawer *drawer) { imgui_drawer = drawer; }

void RequestFrame(rex::ui::WindowedAppContext &context, uint32_t width, uint32_t height) {
  if (!recorder || request_pending.exchange(true))
    return;
  context.CallInUIThread([width, height] {
    if (recorder && imgui_drawer) {
      auto *r = static_cast<Recorder *>(recorder);
      r->BeginUIFrame();
      rex::ui::AppUIDrawContext draw_context(width, height);
      imgui_drawer->Draw(draw_context);
      r->FinishUIFrame();
    }
    request_pending = false;
  });
}

std::shared_ptr<const Frame> LatestFrame() {
  std::lock_guard lock(mutex);
  return latest;
}

void TakeTextureChanges(std::vector<Texture> &created, std::vector<uint32_t> &released) {
  std::lock_guard lock(mutex);
  created.swap(created_textures);
  created_textures.clear();
  released.swap(released_textures);
  released_textures.clear();
}

}  // namespace d3::shadow::overlay
