/**
 * Souls of the Reaper: local profiles.
 *
 * A profile is an XUID with a name. The XUID never changes and names the
 * profile's save folder (<root>/<XUID>/...); the name is only a label kept in
 * <root>/profiles/<XUID>.name, so renaming a profile never moves or hides its
 * saves. The launcher reads and writes the same files.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::system::xam {

struct ProfileEntry {
  uint64_t xuid = 0;
  std::string name;
};

class ProfileStore {
 public:
  // XAM name buffers hold 16 bytes with the terminator.
  static constexpr size_t kMaxNameBytes = 15;

  explicit ProfileStore(std::filesystem::path root);

  // The XUID of the profile that existed before profiles: 0xB13EBABEBABEBABE,
  // moved by --systemlink_instance so copies of the game on one LAN differ.
  static uint64_t DefaultXuid();
  // The name that profile had: SOTR_USERNAME, else "User" (or "User<instance>").
  static std::string DefaultName();
  // A usable name: surrounding spaces trimmed, cut to 15 bytes on a character
  // boundary. Empty when nothing usable is left or the text is not UTF-8.
  static std::string CleanName(std::string_view raw);
  static std::string XuidText(uint64_t xuid);

  // Every profile: each XUID save folder (named once, see Migrate) and each
  // name file whose profile has no saves yet. The default profile comes first,
  // then the rest by name.
  std::vector<ProfileEntry> List();
  std::optional<ProfileEntry> Find(uint64_t xuid);
  std::optional<ProfileEntry> FindByName(std::string_view name);

  // Both return an empty string on success, otherwise why not (for the picker).
  std::string Create(std::string_view name, ProfileEntry* created);
  std::string Rename(uint64_t xuid, std::string_view name);
  // The default profile (DefaultXuid), named DefaultName when it has no name yet.
  ProfileEntry EnsureDefault();

 private:
  std::filesystem::path NameFile(uint64_t xuid) const;
  std::optional<std::string> ReadName(uint64_t xuid) const;
  bool WriteName(uint64_t xuid, const std::string& name);
  // Names the XUID folders that have no name file yet (saves made before
  // profiles): the default profile keeps its old name, the old co-op guest
  // folders become Guest2-4, anything else "Player XXXX".
  void Migrate();
  std::vector<ProfileEntry> ListLocked();
  bool NameTaken(std::string_view name, uint64_t except);

  std::filesystem::path root_;
  std::mutex mutex_;
};

}  // namespace rex::system::xam
