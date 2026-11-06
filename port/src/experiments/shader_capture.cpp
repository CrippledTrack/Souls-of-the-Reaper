// Experiment (gpu-layer-experiment branch): save every shader the game creates, as a
// complete container (header followed by the physical part: literal constants, microcode).
//
//   --capture_shaders=<dir>   write <dir>/<vs|ps>_<fnv64>.bin once per unique container
//
// The game never holds a complete container in memory. Its loader (sub_82585100 for vertex
// shaders, sub_82585280 for pixel shaders) copies a prebuilt D3D object (object fields, then
// the container header) out of a resource record, copies the physical part out of a
// separate data stream into GPU memory, and then calls the Direct3D in-place init:
//   sub_82D692A0(object, physical)   vertex shader (832 bytes of patch tables before the header)
//   sub_82D66188(object, physical)   pixel shader
// Vertex shaders are patched per vertex layout after this, so these hooks save the pieces
// just before the init runs, while both are still the originals.
//
// Addresses are for the verified USA title update executable (update-usa.json), so this
// file is compiled into title update builds only.
#include "generated/update/diablo3_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

DECLARE_REX_FUNC(sub_82D692A0);
DECLARE_REX_FUNC(sub_82D66188);

REXCVAR_DEFINE_STRING(capture_shaders, "", "Experiments",
                      "Directory to save every shader container the game creates (empty: off)");

namespace {

constexpr uint32_t kMagicMask = 0xFFFFFF00;
constexpr uint32_t kMagic = 0x102A1100;
constexpr uint32_t kMaxPart = 1u << 20;
// The header follows the D3D object fields: 40 bytes for pixel shaders, 872 for vertex
// shaders. Search a little further in case the layout differs.
constexpr uint32_t kHeaderSearch = 1024;

uint32_t LoadBe32(const uint8_t *base, uint32_t address) {
  const uint8_t *p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Fnv1a64(const uint8_t *data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

void Capture(const uint8_t *base, uint32_t object, uint32_t physical, const char *hook) {
  const std::string &folder = REXCVAR_GET(capture_shaders);
  if (folder.empty() || object == 0 || physical == 0)
    return;
  uint32_t header = 0;
  for (uint32_t offset = 0; offset < kHeaderSearch; offset += 4) {
    if ((LoadBe32(base, object + offset) & kMagicMask) == kMagic) {
      header = object + offset;
      break;
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4)
    REXLOG_INFO("capture_shaders: {} object={:#x} physical={:#x} header offset={}", hook, object,
                physical, header ? int64_t(header - object) : -1);
  if (header == 0)
    return;
  const uint32_t flags = LoadBe32(base, header);
  const uint32_t virtual_size = LoadBe32(base, header + 4);
  const uint32_t physical_size = LoadBe32(base, header + 8);
  if (virtual_size == 0 || virtual_size > kMaxPart || physical_size > kMaxPart)
    return;
  std::vector<uint8_t> blob(base + header, base + header + virtual_size);
  blob.insert(blob.end(), base + physical, base + physical + physical_size);
  const uint64_t hash = Fnv1a64(blob.data(), blob.size());

  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  static bool folder_ready = false;
  std::lock_guard lock(mutex);
  if (!seen.insert(hash).second)
    return;
  if (!folder_ready) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    folder_ready = true;
  }
  char name[64];
  std::snprintf(name, sizeof(name), "%s_%016llx.bin", (flags & 1) ? "vs" : "ps",
                static_cast<unsigned long long>(hash));
  const auto path = std::filesystem::path(folder) / name;
  if (FILE *file = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(blob.data(), 1, blob.size(), file);
    std::fclose(file);
  } else {
    REXLOG_WARN("capture_shaders: cannot write {}", path.string());
  }
}

} // namespace

extern "C" void sub_82D692A0(PPCContext &ctx, uint8_t *base) {
  Capture(base, ctx.r3.u32, ctx.r4.u32, "vertex init");
  __imp__sub_82D692A0(ctx, base);
}

extern "C" void sub_82D66188(PPCContext &ctx, uint8_t *base) {
  Capture(base, ctx.r3.u32, ctx.r4.u32, "pixel init");
  __imp__sub_82D66188(ctx, base);
}
