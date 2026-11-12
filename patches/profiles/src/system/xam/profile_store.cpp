/**
 * Souls of the Reaper: local profiles. See profile_store.h.
 */

#include <rex/system/xam/profile_store.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <random>
#include <sstream>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>

REXCVAR_DECLARE(int32_t, systemlink_instance);

namespace rex::system::xam {
namespace {

constexpr uint64_t kClassicXuid = 0xB13EBABEBABEBABEull;
// New profiles keep these high bits: they pass Diablo III's XUID check (it
// refuses users with any of 0x00C0000000000000 set).
constexpr uint64_t kXuidHigh = 0xB13EBABE00000000ull;
constexpr const char* kProfilesDir = "profiles";
constexpr const char* kNameExtension = ".name";

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (auto& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower;
}

// A 16-digit hex folder or file stem, as save folders are named.
std::optional<uint64_t> ParseXuid(const std::string& text) {
  if (text.size() != 16 ||
      !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
    return std::nullopt;
  }
  const uint64_t xuid = std::strtoull(text.c_str(), nullptr, 16);
  // XUID 0 holds content shared by every user, not a profile.
  return xuid ? std::optional<uint64_t>(xuid) : std::nullopt;
}

}  // namespace

ProfileStore::ProfileStore(std::filesystem::path root) : root_(std::move(root)) {}

uint64_t ProfileStore::DefaultXuid() {
  const int32_t instance = REXCVAR_GET(systemlink_instance) & 0xFFFF;
  return kClassicXuid + (static_cast<uint64_t>(instance) << 2);
}

std::string ProfileStore::DefaultName() {
  std::string name;
  if (const char* custom = std::getenv("SOTR_USERNAME")) {
    name = custom;
#ifdef _WIN32
    // The narrow environment uses the ANSI code page; the name is needed as UTF-8.
    if (const wchar_t* wide = _wgetenv(L"SOTR_USERNAME")) {
      try {
        name = rex::string::to_utf8(std::u16string_view(reinterpret_cast<const char16_t*>(wide)));
      } catch (const std::exception&) {
        name.clear();
      }
    }
#endif
  }
  name = CleanName(name);
  if (!name.empty()) return name;
  const int32_t instance = REXCVAR_GET(systemlink_instance) & 0xFFFF;
  return instance ? fmt::format("User{}", instance) : std::string("User");
}

std::string ProfileStore::CleanName(std::string_view raw) {
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
  std::string name(raw);
  // Cut at a character boundary: half a UTF-8 sequence would make every later
  // conversion of the name to UTF-16 throw.
  size_t length = std::min(name.size(), kMaxNameBytes);
  while (length > 0 && length < name.size() &&
         (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) {
    --length;
  }
  name.resize(length);
  while (!name.empty() && name.back() == ' ') name.pop_back();
  for (char c : name) {
    if (static_cast<unsigned char>(c) < 0x20) return {};
  }
  try {
    static_cast<void>(rex::string::to_utf16(name));  // throws unless valid UTF-8
  } catch (const std::exception&) {
    return {};
  }
  return name;
}

std::string ProfileStore::XuidText(uint64_t xuid) { return fmt::format("{:016X}", xuid); }

std::filesystem::path ProfileStore::NameFile(uint64_t xuid) const {
  return root_ / kProfilesDir / (XuidText(xuid) + kNameExtension);
}

std::optional<std::string> ProfileStore::ReadName(uint64_t xuid) const {
  std::ifstream file(NameFile(xuid), std::ios::binary);
  if (!file) return std::nullopt;
  std::stringstream text;
  text << file.rdbuf();
  auto name = CleanName(text.str());
  if (name.empty()) return std::nullopt;
  return name;
}

bool ProfileStore::WriteName(uint64_t xuid, const std::string& name) {
  std::error_code ec;
  std::filesystem::create_directories(root_ / kProfilesDir, ec);
  // Written beside the name file and moved over it, so a crash never leaves half a name.
  const auto target = NameFile(xuid);
  auto temporary = target;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << name;
    if (!file) return false;
  }
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    REXSYS_WARN("profiles: cannot save the name of {}: {}", XuidText(xuid), ec.message());
    return false;
  }
  return true;
}

void ProfileStore::Migrate() {
  std::error_code ec;
  const uint64_t base = DefaultXuid();
  for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
    if (!entry.is_directory(ec)) continue;
    const auto xuid = ParseXuid(entry.path().filename().string());
    if (!xuid || ReadName(*xuid)) continue;
    std::string name;
    if (*xuid == base) {
      name = DefaultName();
    } else if (*xuid > base && *xuid <= base + 3) {
      // The fixed XUIDs co-op guests had before profiles.
      name = fmt::format("Guest{}", *xuid - base + 1);
    } else {
      name = fmt::format("Player {:04X}", static_cast<uint32_t>(*xuid & 0xFFFF));
    }
    // Keep names unique: a later one gets a number.
    std::string unique = name;
    for (int n = 2; NameTaken(unique, *xuid); ++n) {
      const std::string suffix = fmt::format(" {}", n);
      unique = CleanName(name.substr(0, kMaxNameBytes - suffix.size())) + suffix;
    }
    WriteName(*xuid, unique);
  }
}

std::vector<ProfileEntry> ProfileStore::ListLocked() {
  std::vector<ProfileEntry> profiles;
  std::error_code ec;
  auto add = [&](uint64_t xuid) {
    if (std::any_of(profiles.begin(), profiles.end(), [&](const auto& p) { return p.xuid == xuid; })) {
      return;
    }
    if (auto name = ReadName(xuid)) profiles.push_back({xuid, *name});
  };
  for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
    if (!entry.is_directory(ec)) continue;
    if (auto xuid = ParseXuid(entry.path().filename().string())) add(*xuid);
  }
  for (const auto& entry : std::filesystem::directory_iterator(root_ / kProfilesDir, ec)) {
    if (entry.path().extension() != kNameExtension) continue;
    if (auto xuid = ParseXuid(entry.path().stem().string())) add(*xuid);
  }
  const uint64_t base = DefaultXuid();
  std::sort(profiles.begin(), profiles.end(), [base](const auto& a, const auto& b) {
    if ((a.xuid == base) != (b.xuid == base)) return a.xuid == base;
    return Lower(a.name) < Lower(b.name);
  });
  return profiles;
}

bool ProfileStore::NameTaken(std::string_view name, uint64_t except) {
  const auto wanted = Lower(name);
  for (const auto& profile : ListLocked()) {
    if (profile.xuid != except && Lower(profile.name) == wanted) return true;
  }
  return false;
}

std::vector<ProfileEntry> ProfileStore::List() {
  std::lock_guard lock(mutex_);
  Migrate();
  return ListLocked();
}

std::optional<ProfileEntry> ProfileStore::Find(uint64_t xuid) {
  for (auto& profile : List()) {
    if (profile.xuid == xuid) return profile;
  }
  return std::nullopt;
}

std::optional<ProfileEntry> ProfileStore::FindByName(std::string_view name) {
  const auto wanted = Lower(CleanName(name));
  for (auto& profile : List()) {
    if (Lower(profile.name) == wanted) return profile;
  }
  return std::nullopt;
}

std::string ProfileStore::Create(std::string_view raw_name, ProfileEntry* created) {
  const auto name = CleanName(raw_name);
  if (name.empty()) return "Enter a name.";
  std::lock_guard lock(mutex_);
  Migrate();
  const auto profiles = ListLocked();
  if (NameTaken(name, 0)) return "A profile with that name already exists.";
  // A random low half: Diablo III sometimes passes only the low 32 bits of an
  // XUID, so they must differ between profiles. Random values also keep
  // profiles made on different PCs apart in System Link.
  std::random_device random;
  const uint64_t base = DefaultXuid();
  uint64_t xuid = 0;
  for (int attempt = 0; attempt < 64 && !xuid; ++attempt) {
    const uint64_t candidate = kXuidHigh | random();
    const auto low = static_cast<uint32_t>(candidate);
    const bool clash =
        low == 0 || (candidate >= kClassicXuid && candidate <= base + 3) ||
        std::any_of(profiles.begin(), profiles.end(),
                    [low](const auto& p) { return static_cast<uint32_t>(p.xuid) == low; });
    if (!clash) xuid = candidate;
  }
  if (!xuid || !WriteName(xuid, name)) return "The profile could not be saved.";
  if (created) *created = {xuid, name};
  REXSYS_INFO("profiles: created {} ({})", name, XuidText(xuid));
  return {};
}

ProfileEntry ProfileStore::EnsureDefault() {
  const uint64_t xuid = DefaultXuid();
  std::lock_guard lock(mutex_);
  if (auto name = ReadName(xuid)) return {xuid, *name};
  std::string unique = DefaultName();
  for (int n = 2; NameTaken(unique, xuid); ++n) {
    const std::string suffix = fmt::format(" {}", n);
    unique = CleanName(DefaultName().substr(0, kMaxNameBytes - suffix.size())) + suffix;
  }
  WriteName(xuid, unique);
  return {xuid, unique};
}

std::string ProfileStore::Rename(uint64_t xuid, std::string_view raw_name) {
  const auto name = CleanName(raw_name);
  if (name.empty()) return "Enter a name.";
  std::lock_guard lock(mutex_);
  if (NameTaken(name, xuid)) return "A profile with that name already exists.";
  if (!WriteName(xuid, name)) return "The profile could not be saved.";
  REXSYS_INFO("profiles: {} renamed to {}", XuidText(xuid), name);
  return {};
}

}  // namespace rex::system::xam
