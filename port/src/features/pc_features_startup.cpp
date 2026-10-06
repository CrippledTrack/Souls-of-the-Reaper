#include "pc_features.h"
#include "pc_features_logic.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    pc_use_saved_render_scale, true, "PC features",
    "Apply the scale saved in the game's Video options on startup");

namespace d3::features {
void ApplySavedRenderScale(const std::filesystem::path &user_data_root) {
  if (rex::cvar::GetFlagByName("pc_use_saved_render_scale") != "true")
    return;
  const auto scale = ReadRenderScale(user_data_root / "pc-render-scale.txt", 0);
  if (!scale)
    return;
  const auto value = std::to_string(scale);
  const bool x = rex::cvar::SetFlagByName("draw_resolution_scale_x", value);
  const bool y = rex::cvar::SetFlagByName("draw_resolution_scale_y", value);
  if (x && y)
    REXLOG_INFO("PC settings: applying saved render resolution {}x", scale);
  else
    REXLOG_ERROR("PC settings: unable to apply saved render resolution");
}
} // namespace d3::features
