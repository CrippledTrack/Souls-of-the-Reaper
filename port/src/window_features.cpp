#include "window_features.h"
#include "generated/linux/diablo3_pch.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <iomanip>
#include <rex/cvar.h>
#include <sstream>
namespace {
std::atomic<uint64_t> swaps{0};
std::atomic<uint64_t> dimensions{0};
class Title final : public rex::ui::ImGuiDialog {
public:
  Title(rex::ui::ImGuiDrawer *drawer, rex::ui::Window *window)
      : ImGuiDialog(drawer), window_(window) {
    window_->SetTitle("Souls of the Reaper | Starting | Vulkan");
    const auto shared = std::stoi(rex::cvar::GetFlagByName("resolution_scale"));
    const auto axis = [&](const char *name) {
      return std::clamp(rex::cvar::HasNonDefaultValue(name)
                            ? std::stoi(rex::cvar::GetFlagByName(name))
                            : shared,
                        1, 7);
    };
    scale_x_ = axis("draw_resolution_scale_x");
    scale_y_ = axis("draw_resolution_scale_y");
  }

protected:
  void OnDraw(ImGuiIO &) override {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration<double>(now - previous_).count();
    if (elapsed < 0.5)
      return;
    const auto count = swaps.load(std::memory_order_relaxed);
    const auto size = dimensions.load(std::memory_order_relaxed);
    std::ostringstream title;
    title << "Souls of the Reaper | " << std::fixed << std::setprecision(1)
          << double(count - previous_count_) / elapsed << " FPS";
    if (size)
      title << " | Render " << (size >> 32) * scale_x_ << 'x'
            << uint32_t(size) * scale_y_;
    title << " | Window " << window_->GetActualPhysicalWidth() << 'x'
          << window_->GetActualPhysicalHeight() << " | Vulkan";
    window_->SetTitle(title.str());
    previous_ = now;
    previous_count_ = count;
  }

private:
  rex::ui::Window *window_;
  std::chrono::steady_clock::time_point previous_ =
      std::chrono::steady_clock::now();
  uint64_t previous_count_ = 0;
  unsigned scale_x_ = 1, scale_y_ = 1;
};
} // namespace
// Preserve the SDK import implementation while observing guest frame swaps.
extern "C" void __imp__VdSwap(PPCContext &ctx, uint8_t *base) {
  using Original = void (*)(PPCContext &, uint8_t *);
  static const auto original =
      reinterpret_cast<Original>(dlsym(RTLD_NEXT, "__imp__VdSwap"));
  if (!original)
    REX_FATAL("Unable to resolve SDK VdSwap for frame telemetry");
  const auto width_pointer = REX_LOAD_U32(ctx.r1.u32 + 0x54);
  const auto height_pointer = REX_LOAD_U32(ctx.r1.u32 + 0x5C);
  const auto width = REX_LOAD_U32(width_pointer),
             height = REX_LOAD_U32(height_pointer);
  original(ctx, base);
  dimensions.store((uint64_t(width) << 32) | height, std::memory_order_relaxed);
  swaps.fetch_add(1, std::memory_order_relaxed);
}
std::unique_ptr<rex::ui::ImGuiDialog>
CreateDiabloWindowTitle(rex::ui::ImGuiDrawer *drawer, rex::ui::Window *window) {
  return std::make_unique<Title>(drawer, window);
}
