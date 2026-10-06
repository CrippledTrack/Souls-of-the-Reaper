#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#include "window_features.h"
#ifdef SOULS_TITLE_UPDATE_2
#include "generated/tu2/diablo3_pch.h"
#elif defined(_WIN32)
#include "generated/default/diablo3_pch.h"
#else
#include "generated/linux/diablo3_pch.h"
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <rex/cvar.h>
#include <sstream>
namespace {
std::atomic<uint64_t> swaps{0};
std::atomic<uint64_t> dimensions{0};
#ifdef _WIN32
constexpr std::string_view backend = "D3D12";
#else
constexpr std::string_view backend = "Vulkan";
#endif

using OriginalSwap = void (*)(PPCContext &, uint8_t *);
OriginalSwap ResolveOriginalSwap() {
#ifdef _WIN32
  // Find the export in a loaded DLL, independent of the SDK's configuration
  // suffix (Debug / Release / RelWithDebInfo) and installation directory.
  const auto modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (modules == INVALID_HANDLE_VALUE)
    return nullptr;
  MODULEENTRY32W module{};
  module.dwSize = sizeof(module);
  OriginalSwap original = nullptr;
  if (Module32FirstW(modules, &module)) {
    do {
      // The executable defines our wrapper; only inspect DLLs for the SDK.
      if (module.hModule == GetModuleHandleW(nullptr))
        continue;
      original = reinterpret_cast<OriginalSwap>(
          GetProcAddress(module.hModule, "__imp__VdSwap"));
      if (original)
        break;
    } while (Module32NextW(modules, &module));
  }
  CloseHandle(modules);
  return original;
#else
  return reinterpret_cast<OriginalSwap>(dlsym(RTLD_NEXT, "__imp__VdSwap"));
#endif
}
class Title final : public rex::ui::ImGuiDialog {
public:
  Title(rex::ui::ImGuiDrawer *drawer, rex::ui::Window *window)
      : ImGuiDialog(drawer), window_(window) {
    window_->SetTitle("Souls of the Reaper | Starting | " + std::string(backend));
  }

protected:
  void OnDraw(ImGuiIO &) override {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration<double>(now - previous_).count();
    if (elapsed < 0.5)
      return;
    // Read after setup too: optional saved settings are applied after dialogs
    // are created. Windows' older SDK only exposes the per-axis scale flags.
#ifdef _WIN32
    const auto axis = [](const char *name) {
      return std::clamp(std::stoi(rex::cvar::GetFlagByName(name)), 1, 7);
    };
#else
    const auto shared = std::stoi(rex::cvar::GetFlagByName("resolution_scale"));
    const auto axis = [&](const char *name) {
      return std::clamp(rex::cvar::HasNonDefaultValue(name)
                            ? std::stoi(rex::cvar::GetFlagByName(name))
                            : shared, 1, 7);
    };
#endif
    const auto scale_x = axis("draw_resolution_scale_x");
    const auto scale_y = axis("draw_resolution_scale_y");
    const auto count = swaps.load(std::memory_order_relaxed);
    const auto size = dimensions.load(std::memory_order_relaxed);
    std::ostringstream title;
    title << "Souls of the Reaper | " << std::fixed << std::setprecision(1)
          << double(count - previous_count_) / elapsed << " FPS";
    if (size)
      title << " | Render " << (size >> 32) * scale_x << 'x'
            << uint32_t(size) * scale_y;
    title << " | Window " << window_->GetActualPhysicalWidth() << 'x'
          << window_->GetActualPhysicalHeight() << " | " << backend;
    window_->SetTitle(title.str());
    previous_ = now;
    previous_count_ = count;
  }

private:
  rex::ui::Window *window_;
  std::chrono::steady_clock::time_point previous_ =
      std::chrono::steady_clock::now();
  uint64_t previous_count_ = 0;
};
} // namespace
// Preserve the SDK import implementation while observing guest frame swaps.
extern "C" void __imp__VdSwap(PPCContext &ctx, uint8_t *base) {
  static const auto original = ResolveOriginalSwap();
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
