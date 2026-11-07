// Experiment (gpu-layer-experiment branch): a Vulkan renderer that runs beside ReXGlue's
// emulated GPU and draws the game's frames into a second window (see
// experiments/gpu-layer/RENDERING.md). Off unless --shadow_renderer=true.
#pragma once

#include <cstdint>

struct PPCContext;

namespace rex::ui {
class WindowedAppContext;
}

namespace d3::shadow {

// Called once from the app when its window exists; starts the renderer thread when enabled.
void Initialize(rex::ui::WindowedAppContext &context);
void Shutdown();

// Called from the game's draw hook (sub_82D621B0) before the draw runs.
void OnGuestDraw(const uint8_t *base, uint32_t device, uint32_t primitive, uint32_t base_vertex,
                 uint32_t start_index, uint32_t index_count);

// Called from the D3DDevice_Resolve hook (sub_82D5B9D0) before it runs.
void OnGuestResolve(const uint8_t *base, const PPCContext &ctx);

// Called from the game's present hook at the end of every guest frame.
// Called from the D3DDevice_Clear hook (sub_82D57638) before it runs.
void OnGuestClear(const uint8_t *base, const PPCContext &ctx);

void OnGuestPresent();

}  // namespace d3::shadow
