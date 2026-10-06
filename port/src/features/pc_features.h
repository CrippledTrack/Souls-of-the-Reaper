#pragma once

#include <filesystem>

namespace d3::features {
// Called before the graphics backend creates resolution-dependent resources.
void ApplySavedRenderScale(const std::filesystem::path &user_data_root);
} // namespace d3::features
