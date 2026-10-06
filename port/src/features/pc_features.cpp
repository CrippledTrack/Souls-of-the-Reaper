// Optional base-disc hooks, adapted from the previous PC settings integration.
// See port/extra-features.md for the executable and address audit.
#ifdef _WIN32
#include "generated/default/diablo3_pch.h"
#else
#include "generated/linux/diablo3_pch.h"
#endif
#include "pc_features.h"
#include "pc_features_logic.h"

#include <rex/cvar.h>
#include <rex/runtime.h>

#include <algorithm>
#include <string>

DECLARE_REX_FUNC(__imp__sub_826FD808);
DECLARE_REX_FUNC(__imp__sub_826FBFF0);
DECLARE_REX_FUNC(__imp__sub_826FC030);
DECLARE_REX_FUNC(__imp__sub_826FC798);
DECLARE_REX_FUNC(__imp__sub_826FCE80);
DECLARE_REX_FUNC(__imp__sub_8282F0F0);
DECLARE_REX_FUNC(__imp__sub_823BD130);
DECLARE_REX_FUNC(sub_826FBD68);
DECLARE_REX_FUNC(sub_826FBE30);
DECLARE_REX_FUNC(sub_82700AD8);
DECLARE_REX_FUNC(sub_82CDAD58);
DECLARE_REX_FUNC(sub_82CDB0F0);
DECLARE_REX_FUNC(sub_82CD9520);
DECLARE_REX_FUNC(sub_82CDA018);
DECLARE_REX_FUNC(sub_82749C10);
DECLARE_REX_FUNC(sub_8246C520);
DECLARE_REX_FUNC(sub_825086E0);
DECLARE_REX_FUNC(sub_8246C4B0);

namespace {
constexpr uint32_t kRenderOption = 100;
constexpr uint32_t kOptionsOwner = 0x83302514;
// Original sub_826FC798: lis r11,-32248; addi r5,r11,-1628.
// The negative displacement borrows from the high half (0x82080000).
constexpr uint32_t kSelectorTextPath = 0x82080000u - 1628;
constexpr std::string_view kRenderKey = "D3PC:RenderResolution";
constexpr std::string_view kTooltipKey = "D3PC:RenderResolutionTooltip";
constexpr std::string_view kAutosaveKey =
    "ConsoleUI:AutosaveWarningScreenText_XBox360";

struct Settings {
  int active_x, active_y, selected;
  bool save_failed = false;
  std::filesystem::path path;

  Settings() {
#ifdef _WIN32
    // The older Windows SDK exposes the axis settings used by its launcher.
    active_x = std::stoi(rex::cvar::GetFlagByName("draw_resolution_scale_x"));
    active_y = std::stoi(rex::cvar::GetFlagByName("draw_resolution_scale_y"));
    const auto shared = active_x;
#else
    const auto shared = std::stoi(rex::cvar::GetFlagByName("resolution_scale"));
    const auto axis = [&](const char *name) {
      return rex::cvar::HasNonDefaultValue(name)
                 ? std::stoi(rex::cvar::GetFlagByName(name))
                 : shared;
    };
    active_x = axis("draw_resolution_scale_x");
    active_y = axis("draw_resolution_scale_y");
#endif
    // Use the resolved directory when the executable is launched directly too.
    path = rex::Runtime::instance()->user_data_root() / "pc-render-scale.txt";
    selected = d3::features::ReadRenderScale(path, std::clamp(shared, 1, 3));
  }

  std::string title() const {
    return "Render resolution (restart required)";
  }

  std::string tooltip() const {
    return std::string(save_failed ? "Could not save setting. " : "") +
           "Use left/right to select 1x, 2x, or 3x internal rendering scale. "
           "Current: " +
           std::to_string(active_x) + "x" + std::to_string(active_y) +
           ". Saved: " + std::to_string(selected) +
           "x. Close and relaunch the game to apply. Higher scales use more "
           "GPU memory and processing.";
  }

  void select(int next) {
    if (next < 1 || next > 3 || next == selected)
      return;
    try {
      d3::features::SaveRenderScale(path, next);
      selected = next;
      save_failed = false;
      REXLOG_INFO("PC settings: saved render resolution {}x; restart required",
                  selected);
    } catch (const std::exception &error) {
      save_failed = true;
      REXLOG_ERROR("PC settings: {}", error.what());
    }
  }
};

Settings &settings() {
  static Settings value;
  return value;
}

// Guest strings own their storage. Use the guest API for
// construction/assignment.
void ConstructText(PPCContext &ctx, uint8_t *base, uint32_t destination,
                   std::string_view text, bool assign) {
  const auto previous = ctx.r1.u64;
  const auto frame =
      ctx.r1.u32 - ((static_cast<uint32_t>(text.size()) + 96) & ~15u);
  REX_STORE_U32(frame, ctx.r1.u32);
  for (uint32_t i = 0; i <= text.size(); ++i) {
    REX_STORE_U8(frame + 80 + i, i == text.size() ? 0 : text[i]);
  }
  ctx.r1.u64 = frame;
  ctx.r3.u64 = destination;
  ctx.r4.u64 = frame + 80;
  if (assign)
    sub_82CDB0F0(ctx, base);
  else
    sub_82CDAD58(ctx, base);
  ctx.r1.u64 = previous;
}

std::string ReadText(uint8_t *base, uint32_t address, unsigned limit) {
  if (!address)
    return {};
  std::string result;
  for (unsigned i = 0; i < limit; ++i) {
    const auto byte = REX_LOAD_U8(address + i);
    if (!byte)
      return result;
    result.push_back(static_cast<char>(byte));
  }
  return {}; // Leave unterminated text untouched.
}

uint32_t CurrentDescriptor(uint8_t *base) {
  const auto owner = REX_LOAD_U32(kOptionsOwner);
  if (!owner)
    return 0;
  const auto vector = REX_LOAD_U32(owner + 296);
  const auto index = REX_LOAD_U32(owner + 12);
  if (!vector || index >= REX_LOAD_U32(vector + 8))
    return 0;
  // The vector stores its big-endian data pointer in the low word of a u64.
  const auto data = static_cast<uint32_t>(REX_LOAD_U64(vector));
  return data ? data + index * 44 : 0;
}
} // namespace

// The start screen formats the original build string into a 1024-byte buffer
// before passing it to Root.NormalLayer.ConsoleStart_main.LayoutRoot.Version.
// Match that call site and its "%s" format; all other formatting stays intact.
extern "C" void sub_823BD130(PPCContext &ctx, uint8_t *base) {
  const bool start_version = ctx.lr == 0x8247C004 && ctx.r4.u32 == 0x82003ED0;
  const auto destination = ctx.r3.u32;
  __imp__sub_823BD130(ctx, base);
  if (!start_version)
    return;
  constexpr std::string_view suffix = " + Extras";
  const auto text = ReadText(base, destination, 1024);
  if (text.empty() || text.ends_with(suffix) ||
      text.size() + suffix.size() >= 1024)
    return;
  for (uint32_t i = 0; i < suffix.size(); ++i)
    REX_STORE_U8(destination + static_cast<uint32_t>(text.size()) + i, suffix[i]);
  REX_STORE_U8(destination + static_cast<uint32_t>(text.size() + suffix.size()), 0);
}

// Strong definitions override only these weak generated aliases when linked.
extern "C" void sub_826FD808(PPCContext &ctx, uint8_t *base) {
  const auto owner = ctx.r3.u32;
  __imp__sub_826FD808(ctx, base);
  const auto saved = ctx;
  const auto frame = ctx.r1.u32 - 512;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = frame + 96;
  sub_826FBD68(ctx, base);
  ConstructText(ctx, base, frame + 160, kRenderKey, false);
  ConstructText(ctx, base, frame + 176, kTooltipKey, false);
  ctx.r3.u64 = frame + 96;
  ctx.r4.u64 = 1; // Native left/right selector used by volume and region.
  ctx.r5.u64 = frame + 160;
  ctx.r6.u64 = frame + 176;
  ctx.r7.u64 = kRenderOption;
  sub_826FBE30(ctx, base);
  ctx.r3.u64 = owner + 128; // Video options vector.
  ctx.r4.u64 = frame + 96;
  sub_82700AD8(ctx, base); // Copies and destroys the temporary descriptor.
  // The constructor consumes both key strings, and insertion consumes the
  // temporary descriptor. Do not destroy those temporaries a second time.
  ctx = saved;
  REXLOG_INFO("PC settings: render resolution added to Video options");
}

// Selector model: option ID at +4, count in vtable slot 0.
extern "C" void sub_826FBFF0(PPCContext &ctx, uint8_t *base) {
  if (REX_LOAD_U32(ctx.r3.u32 + 4) == kRenderOption) {
    ctx.r3.u64 = 3;
    return;
  }
  __imp__sub_826FBFF0(ctx, base);
}

extern "C" void sub_826FC030(PPCContext &ctx, uint8_t *base) {
  if (REX_LOAD_U32(ctx.r3.u32 + 4) != kRenderOption) {
    __imp__sub_826FC030(ctx, base);
    return;
  }
  const auto saved = ctx;
  ctx.r3 = saved.r4; // Resolved native selector control.
  ctx.r4.u64 = settings().selected - 1;
  ctx.r5.u64 = 0; // Initialize without firing the change callback.
  sub_82749C10(ctx, base);
  ctx = saved;
}

extern "C" void sub_826FC798(PPCContext &ctx, uint8_t *base) {
  const bool render = REX_LOAD_U32(ctx.r3.u32 + 4) == kRenderOption;
  const auto row = ctx.r5.u32;
  const auto index = ctx.r6.u32;
  // Preserve the game's selection highlighting and normal row setup.
  __imp__sub_826FC798(ctx, base);
  if (!render || index >= 3)
    return;
  const auto saved = ctx;
  const auto frame = ctx.r1.u32 - 256;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = frame + 80;
  ctx.r4.u64 = row;
  ctx.r5.u64 = kSelectorTextPath; // "SelectorTemplateText".
  sub_825086E0(ctx, base);
  ctx.r3.u64 = frame + 80;
  sub_8246C4B0(ctx, base);
  const auto label = ctx.r3.u32;
  const auto setter = REX_LOAD_U32(REX_LOAD_U32(label) + 132);
  const auto text = std::to_string(index + 1) + "x";
  for (uint32_t i = 0; i <= text.size(); ++i)
    REX_STORE_U8(frame + 112 + i, i == text.size() ? 0 : text[i]);
  ctx.r3.u64 = label;
  ctx.r4.u64 = frame + 112;
  ctx.r5.u64 = 0;
  REX_CALL_INDIRECT_FUNC(setter);
  ctx = saved;
}

extern "C" void sub_826FCE80(PPCContext &ctx, uint8_t *base) {
  const auto descriptor = CurrentDescriptor(base);
  if (!descriptor || REX_LOAD_U32(descriptor + 32) != kRenderOption) {
    __imp__sub_826FCE80(ctx, base);
    return;
  }
  const auto saved = ctx;
  sub_8246C520(ctx, base);
  const auto control = ctx.r3.u32;
  const auto index = REX_LOAD_U32(control + 472);
  if (index < 3)
    settings().select(static_cast<int>(index) + 1);
  // Restore the last saved value if saving failed, without another callback.
  ctx.r3.u64 = control;
  ctx.r4.u64 = settings().selected - 1;
  ctx.r5.u64 = 0;
  sub_82749C10(ctx, base);
  const auto owner = REX_LOAD_U32(kOptionsOwner);
  if (owner)
    REX_STORE_U32(owner + 344, 1); // Mark the options model dirty.
  ctx = saved;
}

extern "C" void sub_8282F0F0(PPCContext &ctx, uint8_t *base) {
  const auto name = ReadText(base, ctx.r4.u32, 64);
  const auto destination = ctx.r3.u32;
  if (name == kRenderKey || name == kTooltipKey) {
    const auto saved = ctx;
    ConstructText(
        ctx, base, destination,
        name == kRenderKey ? settings().title() : settings().tooltip(), false);
    ctx = saved;
    ctx.r3.u64 = destination;
    return;
  }
  __imp__sub_8282F0F0(ctx, base);
  if (name != kAutosaveKey)
    return;
  const auto saved = ctx;
  ctx.r3.u64 = destination;
  sub_82CD9520(ctx, base);
  const auto localized = ReadText(base, ctx.r3.u32, 16384);
  const auto replacement = d3::features::PcAutosaveText(localized);
  if (replacement != localized) {
    ConstructText(ctx, base, destination, replacement, true);
    REXLOG_INFO("PC settings: autosave notice uses PC wording");
  }
  ctx = saved;
}
