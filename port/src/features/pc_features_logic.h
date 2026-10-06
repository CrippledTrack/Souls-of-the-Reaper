#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace d3::features {

inline int ReadRenderScale(const std::filesystem::path &path, int fallback) {
  std::ifstream file(path);
  int value;
  if (!(file >> value) || value < 1 || value > 3)
    return fallback;
  file >> std::ws;
  return file.eof() ? value : fallback;
}

inline int ReadWindowMode(const std::filesystem::path &path, int fallback) {
  const auto value = ReadRenderScale(path, 0);
  return value == 1 || value == 2 ? value : fallback;
}

inline std::string_view WindowModeLabel(int mode) {
  return mode == 1 ? "Windowed" : "Borderless fullscreen";
}

void ReplaceRenderScaleFile(const std::filesystem::path &temporary,
                            const std::filesystem::path &destination);

// Replace the setting only after writing a complete temporary file.
inline void SaveRenderScale(const std::filesystem::path &path, int value) {
  if (value < 1 || value > 3)
    throw std::invalid_argument("Invalid render scale");
  auto temporary = path;
  temporary += ".tmp";
  try {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(temporary, std::ios::trunc);
    file << value << '\n';
    file.close();
    if (!file)
      throw std::runtime_error("Render scale write failed");
    ReplaceRenderScaleFile(temporary, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

inline void SaveWindowMode(const std::filesystem::path &path, int mode) {
  if (mode != 1 && mode != 2)
    throw std::invalid_argument("Invalid window mode");
  SaveRenderScale(path, mode);
}

inline std::string PcAutosaveText(std::string_view localized) {
  std::string text(localized);
  for (const auto phrase :
       {"Xbox 360 console", "Xbox 360 Console", "Xbox 360", "Xbox360"}) {
    std::size_t position = 0;
    while ((position = text.find(phrase, position)) != std::string::npos) {
      text.replace(position, std::char_traits<char>::length(phrase), "PC");
      position += 2;
    }
  }
  return text;
}

} // namespace d3::features
