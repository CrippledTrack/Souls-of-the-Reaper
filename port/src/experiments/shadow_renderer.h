// Experiment (gpu-layer-experiment branch): a Vulkan renderer that runs beside ReXGlue's
// emulated GPU and draws the game's frames into a second window (see
// experiments/gpu-layer/RENDERING.md). Off unless --shadow_renderer=true, or
// --shadow_renderer_only=true to draw only with it, inside the game window.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

struct PPCContext;

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class WindowedAppContext;
}

namespace d3::shadow {

// True when --shadow_renderer or --shadow_renderer_only is set.
bool Enabled();

// --shadow_renderer_only: true when the hooks must skip the game's own draws, clears and
// resolves themselves. Normally ReXGlue stops rendering through its gpu_host_rendering switch
// instead and this stays false; it is the fallback for SDKs without that switch.
bool SkipGuestRendering();

// True once the Vulkan renderer draws the game window (--shadow_renderer_only after its
// takeover); the window title then names it instead of ReXGlue's backend.
bool HasTakenOver();

// Called from the app's OnPreSetup. With --shadow_renderer_only it turns the SDK's
// host_presentation off, so the renderer presents into the game window itself (ReXGlue's
// presenter is never attached) and draws ReXGlue's overlays.
void PreSetup();
// The app's OnCreateImmediateDrawer: the overlay recorder when the renderer presents into the
// game window, else null.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();

// Called once from the app when its window exists (OnCreateDialogs); starts shader
// conversion (into <user data>/shader_cache) and the renderer thread when enabled.
void Initialize(rex::ui::WindowedAppContext &context, const std::filesystem::path &user_data_root,
                rex::ui::ImGuiDrawer *imgui_drawer);
void Shutdown();

// Called from the game's draw hook (sub_82D621B0) before the draw runs.
void OnGuestDraw(const uint8_t *base, uint32_t device, uint32_t primitive, uint32_t base_vertex,
                 uint32_t start_index, uint32_t index_count);

// Called from the non-indexed draw hook (sub_82D61D98) before the draw runs.
void OnGuestAutoIndexDraw(const uint8_t *base, uint32_t device, uint32_t primitive,
                          uint32_t start_vertex, uint32_t vertex_count);

// Called from the D3DDevice_Resolve hook (sub_82D5B9D0) before it runs.
void OnGuestResolve(const uint8_t *base, const PPCContext &ctx);

// Called from the game's present hook at the end of every guest frame.
// Called from the D3DDevice_Clear hook (sub_82D57638) before it runs.
void OnGuestClear(const uint8_t *base, const PPCContext &ctx);

void OnGuestPresent();

}  // namespace d3::shadow
