// Experiment (Shadow renderer): widescreen beyond 16:9 (Hor+) for the Vulkan-only mode.
//
// The game picks its display size in sub_82579690 (title update; sub_82575A60 on the base
// disc) from XGetVideoMode: 1280x720 for any HD widescreen mode (1024/852/768/640 otherwise),
// whatever the display really is, and stores an aspect ratio beside it. --shadow_guest_width
// and --shadow_guest_height change that choice, so the game itself builds a back buffer of
// that shape. Its vertical view is fixed: wider shows more at the sides (Hor+), taller trims
// them, like the PC version.
#include "experiments/guest_addresses.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <bit>
#include <cstdint>

REXCVAR_DEFINE_INT32(shadow_guest_width, 0, "Experiments",
                     "Width of the game's own display for the Vulkan-only renderer (e.g. 1680 "
                     "for 21:9 at 720 lines); 0: the game's 1280. Wider than 16:9 shows more "
                     "to the sides. Only with --shadow_renderer_only. Read at startup");
REXCVAR_DEFINE_INT32(shadow_guest_height, 0, "Experiments",
                     "Height of the game's own display for the Vulkan-only renderer (e.g. 800 "
                     "for 16:10 at 1280 wide); 0: the game's 720. Only with "
                     "--shadow_renderer_only. Read at startup");
REXCVAR_DECLARE(bool, shadow_renderer_only);

// sub_82579690(manager r3, out struct pointer r4, flag r5): allocates the 52-byte display
// description and fills it: +32 width, +36 height, +40 refresh rate, +44 bits, +48 aspect
// (float). The aspect is also kept in the manager at +1216.
D3_DECLARE(D3_FN_DISPLAY_MODE);
D3_HOOK(D3_FN_DISPLAY_MODE) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  D3_ORIGINAL(D3_FN_DISPLAY_MODE)(ctx, base);

  const uint32_t mode = REX_LOAD_U32(out);
  const uint32_t width = REX_LOAD_U32(mode + 32);
  const uint32_t height = REX_LOAD_U32(mode + 36);
  const float aspect = std::bit_cast<float>(REX_LOAD_U32(mode + 48));
  REXLOG_INFO("shadow_aspect: game display {}x{}, aspect {:.4f}", width, height, aspect);

  const int32_t wanted_width = REXCVAR_GET(shadow_guest_width);
  const int32_t wanted_height = REXCVAR_GET(shadow_guest_height);
  if ((wanted_width <= 0 && wanted_height <= 0) || !REXCVAR_GET(shadow_renderer_only) ||
      width == 0 || height == 0)
    return;
  // Even, and within what the renderer's guest-space targets hold (2048 x 1536).
  const uint32_t new_width =
      wanted_width > 0 ? std::clamp<uint32_t>(uint32_t(wanted_width) & ~1u, 640, 2048) : width;
  const uint32_t new_height =
      wanted_height > 0 ? std::clamp<uint32_t>(uint32_t(wanted_height) & ~1u, 480, 1536) : height;
  const float new_aspect =
      aspect * (float(new_width) / float(width)) * (float(height) / float(new_height));
  REX_STORE_U32(mode + 32, new_width);
  REX_STORE_U32(mode + 36, new_height);
  REX_STORE_U32(mode + 48, std::bit_cast<uint32_t>(new_aspect));
  REX_STORE_U32(manager + 1216, std::bit_cast<uint32_t>(new_aspect));
  REXLOG_INFO("shadow_aspect: changed to {}x{}, aspect {:.4f}", new_width, new_height, new_aspect);
}
