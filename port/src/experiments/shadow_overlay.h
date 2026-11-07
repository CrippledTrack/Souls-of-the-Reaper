// Experiment (gpu-layer-experiment branch): ReXGlue's ImGui overlays (settings, console, toasts,
// debug overlay) for the Vulkan renderer when it presents into the game window itself
// (--shadow_renderer_only with the SDK's host_presentation off).
//
// ReXGlue draws its overlays on the UI thread through a ui::ImmediateDrawer. The one here
// records each overlay frame (vertices, indices, draws with scissors, texture changes)
// instead of drawing, and the renderer thread replays the latest recording on top of the
// game image before presenting.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace d3::shadow::overlay {

struct Vertex {  // same layout as ui::ImmediateVertex
  float x, y, u, v;
  uint32_t color;  // R8G8B8A8
};

struct Command {
  uint32_t texture = 0;  // 0: white
  bool lines = false;
  bool indexed = false;
  uint32_t count = 0;
  uint32_t first_index = 0;  // indexed: in Frame::indices
  int32_t base_vertex = 0;   // in Frame::vertices
  uint32_t scissor[4] = {};  // x, y, width, height in render target pixels
};

struct Frame {
  uint32_t target_width = 0, target_height = 0;
  float coordinate_size_inv[2] = {};
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<Command> commands;
};

struct Texture {
  uint32_t id = 0;
  uint32_t width = 0, height = 0;
  bool linear = false, repeat = false;
  std::vector<uint8_t> rgba;
};

// The app's OnCreateImmediateDrawer (UI thread).
std::unique_ptr<rex::ui::ImmediateDrawer> CreateRecorder();
// The app's OnCreateDialogs: the drawer whose dialogs are recorded.
void SetImGuiDrawer(rex::ui::ImGuiDrawer *drawer);

// Renderer thread: asks the UI thread to record an overlay frame for a render target of this
// size (once at a time), and returns the most recent finished recording (null before the
// first, or when nothing is shown).
void RequestFrame(rex::ui::WindowedAppContext &context, uint32_t width, uint32_t height);
std::shared_ptr<const Frame> LatestFrame();
// Renderer thread: textures created and released since the last call.
void TakeTextureChanges(std::vector<Texture> &created, std::vector<uint32_t> &released);

}  // namespace d3::shadow::overlay
