// Experiment (gpu-layer-experiment branch): decode Xenos textures for the side-by-side renderer.
//
// Reads a texture fetch constant (layout: xe_gpu_texture_fetch_t in rex/graphics/xenos.h),
// untiles the base level, applies the endian swap and keeps compressed formats as BC blocks.
// Each distinct texture gets an id once; the renderer uploads it and indexes it bindlessly.
#include "shadow_textures.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

REXCVAR_DEFINE_STRING(shadow_dump_textures, "", "Experiments",
                      "Directory to write decoded 8-bit and 32-bit textures to as PGM/PAM files (empty: off)");

namespace d3::shadow {
namespace {

// Debugging: writes the base level of an uncompressed 1- or 4-byte texture as a PGM or PAM.
void DumpTexture(const TextureUpload &upload, uint32_t bytes_per_block, uint32_t xenos_format) {
  const std::string dir = REXCVAR_GET(shadow_dump_textures);
  if (dir.empty() || (bytes_per_block != 1 && bytes_per_block != 4))
    return;
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  const auto path = std::filesystem::path(dir) /
                    fmt::format("tex{}_{}x{}_fmt{}.{}", upload.id, upload.width, upload.height,
                                xenos_format, bytes_per_block == 1 ? "pgm" : "pam");
  FILE *file = std::fopen(path.string().c_str(), "wb");
  if (!file)
    return;
  if (bytes_per_block == 1)
    std::fprintf(file, "P5\n%u %u\n255\n", upload.width, upload.height);
  else
    std::fprintf(file, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                 upload.width, upload.height);
  std::fwrite(upload.data.data(), 1, size_t(upload.width) * upload.height * bytes_per_block, file);
  std::fclose(file);
}

uint32_t Be32(const uint8_t *base, uint32_t address) {
  const uint8_t *p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Offset in bytes of block (x, y, z) in a tiled volume, pitch and height in blocks. From
// ReXGlue's GetTiledOffset3D (Xenia, reconstructed from XGRAPHICS::TileVolume).
int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                      uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  height = (height + 31) & ~31u;
  int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  int32_t macro = ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF) << 1;
  int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                    ((z & 3) << (bytes_per_block_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

uint32_t Log2Ceil(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v)
    ++r;
  return r;
}

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v >> (r + 1))
    ++r;
  return r;
}

uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v); }

// First level stored in the packed mip tail: the level whose shorter side is 16 texels or less.
uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = std::min(Log2Ceil(width), Log2Ceil(height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Texel offset of 2D level `mip` inside the packed mip tail tile (ReXGlue's GetPackedMipOffset,
// Xenia, without the 3D part).
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t mip, uint32_t &x, uint32_t &y) {
  const uint32_t log2_width = Log2Ceil(width), log2_height = Log2Ceil(height);
  const uint32_t log2_size = std::min(log2_width, log2_height);
  const uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed_mip = mip - packed_mip_base;
  x = y = 0;
  if (packed_mip < 3) {
    (log2_width > log2_height ? y : x) = 16 >> packed_mip;
  } else {
    const uint32_t offset = log2_width > log2_height
                                ? (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2)
                                : (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
    (log2_width > log2_height ? x : y) = offset;
  }
}

// Offset in bytes of block (x, y) in a tiled 2D surface whose pitch is in blocks.
// From ReXGlue's src/graphics/pipeline/texture/util.cpp (Xenia, BSD licence), originally
// https://github.com/gildor2/UModel UnTexture.cpp.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

struct FormatInfo {
  TextureFormat format;
  uint32_t block;           // block width and height in texels (1 or 4)
  uint32_t bytes_per_block;
};

bool Lookup(uint32_t xenos_format, FormatInfo *info) {
  switch (xenos_format) {
    case 2: *info = {TextureFormat::kR8, 1, 1}; return true;           // k_8
    case 3: *info = {TextureFormat::kA1RGB5, 1, 2}; return true;       // k_1_5_5_5
    case 4: *info = {TextureFormat::kR5G6B5, 1, 2}; return true;       // k_5_6_5
    case 6: case 14: *info = {TextureFormat::kRGBA8, 1, 4}; return true;  // k_8_8_8_8(_A)
    case 7: *info = {TextureFormat::kRGB10A2, 1, 4}; return true;      // k_2_10_10_10
    case 10: *info = {TextureFormat::kRG8, 1, 2}; return true;         // k_8_8
    case 15: *info = {TextureFormat::kRGBA4, 1, 2}; return true;       // k_4_4_4_4
    case 18: *info = {TextureFormat::kBC1, 4, 8}; return true;         // k_DXT1
    case 19: *info = {TextureFormat::kBC2, 4, 16}; return true;        // k_DXT2_3
    case 20: *info = {TextureFormat::kBC3, 4, 16}; return true;        // k_DXT4_5
    case 24: *info = {TextureFormat::kR16, 1, 2}; return true;         // k_16
    case 25: *info = {TextureFormat::kRG16, 1, 4}; return true;        // k_16_16
    case 26: *info = {TextureFormat::kRGBA16, 1, 8}; return true;      // k_16_16_16_16
    case 30: *info = {TextureFormat::kR16F, 1, 2}; return true;        // k_16_FLOAT
    case 31: *info = {TextureFormat::kRG16F, 1, 4}; return true;       // k_16_16_FLOAT
    case 32: *info = {TextureFormat::kRGBA16F, 1, 8}; return true;     // k_16_16_16_16_FLOAT
    case 36: *info = {TextureFormat::kR32F, 1, 4}; return true;        // k_32_FLOAT
    case 49: *info = {TextureFormat::kBC5, 4, 16}; return true;        // k_DXN
    case 59: *info = {TextureFormat::kBC4, 4, 8}; return true;         // k_DXT5A
    default: return false;
  }
}

void Swap(uint8_t *data, size_t size, uint32_t endian) {
  switch (endian) {
    case 1:  // 8in16
      for (size_t i = 0; i + 1 < size; i += 2)
        std::swap(data[i], data[i + 1]);
      break;
    case 2:  // 8in32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case 3:  // 16in32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

uint64_t Fnv(const void *data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

constexpr uint32_t kMaxTextures = 4096;
constexpr uint32_t kMaxSamplers = 256;
constexpr uint32_t kMaxDimension = 8192;
constexpr size_t kFullHashBytes = 256 * 1024;

}  // namespace

// Content hash for texture identity: 8 bytes per step (FNV-style xor-multiply on 64-bit
// words, then a final mix), much cheaper than byte-wise FNV over hundreds of KiB.
uint64_t HashBytes(const void *data, size_t size, uint64_t hash) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t word;
    std::memcpy(&word, bytes + i, 8);
    hash = (hash ^ word) * 0x9E3779B97F4A7C15ull;
    hash ^= hash >> 29;
  }
  for (; i < size; ++i)
    hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  hash ^= hash >> 32;
  return hash * 0xD6E8FEB86659FD93ull;
}

uint16_t TextureCache::Texture(const uint8_t *base, const uint32_t fetch[6], FrameRecord &frame) {
  const uint64_t memo = Fnv(fetch, 6 * sizeof(uint32_t));
  if (auto found = frame_memo_.find(memo); found != frame_memo_.end())
    return found->second;  // already marked used this frame
  const uint16_t id = FindOrDecode(base, fetch, frame);
  frame_memo_.emplace(memo, id);
  return id;
}

uint16_t TextureCache::FindOrDecode(const uint8_t *base, const uint32_t fetch[6], FrameRecord &frame) {
  if ((fetch[0] & 3) != 2)  // not a texture fetch constant
    return 0;
  // Textures the game fills by resolving a render target come from the renderer's images.
  if (auto resolved = resolve_targets.find(fetch[1] & 0x1FFFF000u); resolved != resolve_targets.end())
    return uint16_t(kResolveIdBase + resolved->second);
  const uint32_t dimension = (fetch[5] >> 9) & 3;  // 0 1D, 1 2D, 2 3D or stacked, 3 cube
  const bool stacked = (fetch[1] >> 10) & 1;
  const bool volume = dimension == 2 && !stacked;
  const bool cube = dimension == 3;
  if (!(dimension == 1 && !stacked) && !volume && !cube)  // 2D, 3D and cube for now
    return 0;
  const uint32_t layers = cube ? 6 : 1;
  FormatInfo info;
  const uint32_t xenos_format = fetch[1] & 0x3F;
  if (!Lookup(xenos_format, &info)) {
    if (unsupported_formats_.insert(xenos_format).second)
      REXLOG_INFO("shadow_renderer: texture format {} not converted yet", xenos_format);
    return 0;
  }
  // size_2d: 13-bit width and height; size_3d: 11-bit width and height, 10-bit depth.
  const uint32_t width = volume ? (fetch[2] & 0x7FF) + 1 : (fetch[2] & 0x1FFF) + 1;
  const uint32_t height = volume ? ((fetch[2] >> 11) & 0x7FF) + 1 : ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t depth = volume ? ((fetch[2] >> 22) & 0x3FF) + 1 : 1;
  if (width > kMaxDimension || height > kMaxDimension)
    return 0;
  const uint32_t physical = fetch[1] & 0xFFFFF000u;
  const bool tiled = fetch[0] >> 31;
  const uint32_t pitch_texels = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t endian = (fetch[1] >> 6) & 3;

  const uint32_t blocks_w = (width + info.block - 1) / info.block;
  const uint32_t blocks_h = (height + info.block - 1) / info.block;
  const uint32_t pitch_blocks = std::max(blocks_w, (pitch_texels + info.block - 1) / info.block);
  const uint32_t bpb = info.bytes_per_block;
  uint32_t bpb_log2 = 0;
  while ((1u << bpb_log2) < bpb)
    ++bpb_log2;
  const uint32_t row_pitch = (pitch_blocks * bpb + 255) & ~255u;  // linear rows: 256-byte aligned
  const uint8_t *source = base + (0xA0000000u | (physical & 0x1FFFFFFFu));

  // Identity: the fetch fields that describe the image (not filtering), plus a sample of the
  // contents so a texture whose memory is reused gets a new id.
  const uint32_t identity[6] = {fetch[0] & 0xFFC00003u, fetch[1], fetch[2], fetch[3] & 0x1FFFu,
                                fetch[4] & 0x3FCu, fetch[5] & 0xFFFFFE00u};
  uint64_t key = Fnv(identity, sizeof(identity));
  const uint64_t identity_key = key;
  // Volumes: tiled ones use TiledOffset3D; linear slices are 4 KiB aligned.
  const uint32_t height_aligned = (blocks_h + 31) & ~31u;
  const size_t slice_pitch = (size_t(row_pitch) * blocks_h + 4095) & ~size_t(4095);
  // Cube faces of the base level: the 32x32-block-aligned face, 4 KiB aligned.
  const size_t face_pitch =
      ((tiled ? size_t((pitch_blocks + 31) & ~31u) * bpb : size_t(row_pitch)) * height_aligned +
       4095) & ~size_t(4095);
  // Packed mips: a base level whose shorter side is 16 texels or less lives in the packed mip
  // tail tile, 16 texels right (square or taller) or down (wider) of the tile origin. From
  // ReXGlue's GetPackedMipOffset for mip 0.
  const bool packed_mips = !volume && ((fetch[5] >> 11) & 1);
  const uint32_t packed_level = packed_mips ? PackedMipLevel(width, height) : UINT32_MAX;
  uint32_t tail_x = 0, tail_y = 0;
  if (packed_level == 0) {
    PackedMipOffset(width, height, 0, tail_x, tail_y);
    tail_x /= info.block;
    tail_y /= info.block;
  }
  auto offset = [&](uint32_t bx, uint32_t by, uint32_t z) -> size_t {
    bx += tail_x;
    by += tail_y;
    if (volume)
      return tiled ? size_t(TiledOffset3D(int32_t(bx), int32_t(by), int32_t(z), pitch_blocks,
                                          height_aligned, bpb_log2))
                   : z * slice_pitch + size_t(by) * row_pitch + size_t(bx) * bpb;
    return z * face_pitch +
           (tiled ? size_t(TiledOffset2D(int32_t(bx), int32_t(by), pitch_blocks, bpb_log2))
                  : size_t(by) * row_pitch + size_t(bx) * bpb);
  };
  const size_t span =
      volume ? offset(blocks_w - 1, blocks_h - 1, depth - 1) + bpb
      : face_pitch * (layers - 1) +
            (tiled ? size_t(TiledOffset2D(0, int32_t(blocks_h - 1), pitch_blocks, bpb_log2)) +
                         size_t(pitch_blocks) * 32 * bpb
                   : size_t(row_pitch) * blocks_h);
  // Small textures, and any texture seen changing in place (cutscene video frames, whose
  // letterbox bars and slow pans defeat a sparse sample), are hashed whole: the game edits
  // some in place (the font atlas gains glyphs over time). Other large ones are sampled at
  // 256 points.
  const auto live_entry = live_.find(identity_key);
  const bool dynamic = live_entry != live_.end() && live_entry->second.dynamic;
  if (span <= kFullHashBytes || dynamic) {
    key = HashBytes(source, span, key);
  } else {
    constexpr uint32_t kSamples = 256;
    for (uint32_t i = 0; i < kSamples; ++i) {
      const size_t at = (span / kSamples) * i;
      key = HashBytes(source + at, std::min<size_t>(16, span - at), key);
    }
  }
  if (auto found = textures_.find(key); found != textures_.end()) {
    ids_[found->second].last_used = frame_number_;
    return found->second;
  }
  // New contents. For a texture seen before with other contents, update the other of its two
  // images instead of allocating one more.
  const int kind = volume ? 1 : cube ? 2 : 0;
  Live &live = live_[identity_key];
  uint16_t id = 0;
  if (live.ids[live.current]) {
    live.dynamic = true;  // same texture, new contents
    const uint8_t next = live.current ^ 1;
    id = live.ids[next];
    if (id) {
      textures_.erase(ids_[id].content);  // that image's old contents are gone
    } else {
      id = live.ids[next] = AllocateId(kind);
    }
    if (id)
      live.current = next;
  } else {
    id = live.ids[live.current] = AllocateId(kind);
  }
  if (!id) {
    if (!full_warned_) {
      full_warned_ = true;
      REXLOG_WARN("shadow_renderer: texture table full ({} textures)", kMaxTextures);
    }
    return 0;
  }
  ids_[id] = {key, identity_key, frame_number_};

  TextureUpload upload;
  upload.id = id;
  upload.layers = layers;
  upload.width = width;
  upload.height = height;
  upload.depth = depth;
  upload.format = info.format;
  upload.swizzle = (fetch[3] >> 1) & 0xFFF;
  upload.data.resize(size_t(blocks_w) * blocks_h * depth * layers * bpb);
  for (uint32_t z = 0; z < depth * layers; ++z)
    for (uint32_t by = 0; by < blocks_h; ++by)
      for (uint32_t bx = 0; bx < blocks_w; ++bx) {
        uint8_t *to = &upload.data[((size_t(z) * blocks_h + by) * blocks_w + bx) * bpb];
        std::memcpy(to, source + offset(bx, by, z), bpb);
        Swap(to, bpb, endian);
      }
  // Mip levels (2D): from mip_address, laid out as levels of a power-of-two texture with
  // 32x32-block-aligned pitch and height and 4 KiB-aligned level strides (linear rows 256-byte
  // aligned). From the packed level on, all levels share the packed tail tile at their
  // PackedMipOffset positions. From ReXGlue's GetGuestTextureLayout.
  const uint32_t mip_physical = fetch[5] & 0xFFFFF000u;
  uint32_t max_level = 0;
  if (!volume && mip_physical)
    max_level = std::min((fetch[4] >> 6) & 0xF, Log2Floor(std::max(width, height)));
  const uint8_t *mip_source = base + (0xA0000000u | (mip_physical & 0x1FFFFFFFu));
  size_t level_start = 0;  // byte offset of the current storage level from mip_address
  for (uint32_t level = 1; level <= max_level; ++level) {
    const uint32_t stored = std::min(level, packed_level);  // storage level
    if (level > 1 && level <= packed_level) {
      // Advance past the previous level.
      const uint32_t prev = level - 1;
      const uint32_t prev_pitch =
          ((std::max(NextPow2(width) >> prev, 1u) + info.block - 1) / info.block + 31) & ~31u;
      const uint32_t prev_rows =
          ((std::max(NextPow2(height) >> prev, 1u) + info.block - 1) / info.block + 31) & ~31u;
      size_t prev_row_bytes = size_t(prev_pitch) * bpb;
      if (!tiled)
        prev_row_bytes = (prev_row_bytes + 255) & ~size_t(255);
      level_start += ((prev_row_bytes * prev_rows + 4095) & ~size_t(4095)) * layers;
    }
    const uint32_t level_pitch =
        ((std::max(NextPow2(width) >> stored, 1u) + info.block - 1) / info.block + 31) & ~31u;
    size_t level_row_bytes = size_t(level_pitch) * bpb;
    if (!tiled)
      level_row_bytes = (level_row_bytes + 255) & ~size_t(255);
    uint32_t off_x = 0, off_y = 0;
    if (level >= packed_level) {
      PackedMipOffset(width, height, level, off_x, off_y);
      off_x /= info.block;
      off_y /= info.block;
    }
    const uint32_t level_w = (std::max(width >> level, 1u) + info.block - 1) / info.block;
    const uint32_t level_h = (std::max(height >> level, 1u) + info.block - 1) / info.block;
    const uint32_t level_rows =
        ((std::max(NextPow2(height) >> stored, 1u) + info.block - 1) / info.block + 31) & ~31u;
    const size_t level_face_pitch = (level_row_bytes * level_rows + 4095) & ~size_t(4095);
    upload.level_offsets.push_back(uint32_t(upload.data.size()));
    const size_t at = upload.data.size();
    upload.data.resize(at + size_t(level_w) * level_h * layers * bpb);
    for (uint32_t face = 0; face < layers; ++face) {
      const uint8_t *level_source = mip_source + level_start + face * level_face_pitch;
      const size_t face_at = at + size_t(face) * level_w * level_h * bpb;
      for (uint32_t by = 0; by < level_h; ++by)
        for (uint32_t bx = 0; bx < level_w; ++bx) {
          const uint32_t x = bx + off_x, y = by + off_y;
          const size_t from =
              tiled ? size_t(TiledOffset2D(int32_t(x), int32_t(y), level_pitch, bpb_log2))
                    : size_t(y) * level_row_bytes + size_t(x) * bpb;
          uint8_t *to = &upload.data[face_at + (size_t(by) * level_w + bx) * bpb];
          std::memcpy(to, level_source + from, bpb);
          Swap(to, bpb, endian);
        }
    }
    upload.levels = level + 1;
  }
  if (volume)
    REXLOG_INFO("shadow_renderer: 3D texture {}x{}x{} format {} {} id {}", width, height, depth,
                xenos_format, tiled ? "tiled" : "linear", upload.id & 0x7FFF);
  if (cube)
    REXLOG_INFO("shadow_renderer: cube texture {}x{} format {} {} levels {} id {}", width, height,
                xenos_format, tiled ? "tiled" : "linear", upload.levels, upload.id & 0x3FFF);
  if (!volume && !cube && info.block == 1)
    DumpTexture(upload, bpb, xenos_format);
  textures_.emplace(key, uint16_t(upload.id));
  frame.new_textures.push_back(std::move(upload));
  return uint16_t(frame.new_textures.back().id);
}

uint16_t TextureCache::AllocateId(int kind) {
  if (!free_ids_[kind].empty()) {
    const uint16_t id = free_ids_[kind].back();
    free_ids_[kind].pop_back();
    return id;
  }
  switch (kind) {
    case 1:
      return next_texture_3d_ < kTexture3DSlots ? uint16_t(kTexture3DBit | next_texture_3d_++) : 0;
    case 2:
      return next_texture_cube_ < kTextureCubeSlots ? uint16_t(kTextureCubeBit | next_texture_cube_++)
                                                    : 0;
    default:
      return next_texture_ < kMaxTextures ? uint16_t(next_texture_++) : 0;
  }
}

void TextureCache::ForgetId(uint16_t id) {
  auto info = ids_.find(id);
  if (info == ids_.end())
    return;
  if (auto t = textures_.find(info->second.content); t != textures_.end() && t->second == id)
    textures_.erase(t);
  if (auto l = live_.find(info->second.identity); l != live_.end()) {
    for (uint16_t &slot : l->second.ids)
      if (slot == id)
        slot = 0;
    if (!l->second.ids[0] && !l->second.ids[1])
      live_.erase(l);
  }
  ids_.erase(info);
}

void TextureCache::FinishFrame(FrameRecord &frame) {
  ++frame_number_;
  // Every 5 seconds, free textures unused for 10 (at 60 frames per second). The renderer
  // releases their images; no frame in flight can still use them by then.
  constexpr uint64_t kCheckEvery = 300, kUnusedFor = 600;
  if (frame_number_ % kCheckEvery)
    return;
  std::vector<uint16_t> stale;
  for (const auto &[id, info] : ids_)
    if (frame_number_ - info.last_used > kUnusedFor)
      stale.push_back(id);
  for (uint16_t id : stale) {
    ForgetId(id);
    frame.freed_textures.push_back(id);
    free_ids_[(id & kTexture3DBit) ? 1 : (id & kTextureCubeBit) ? 2 : 0].push_back(id);
  }
}

uint16_t TextureCache::Sampler(const uint32_t fetch[6], FrameRecord &frame) {
  SamplerDesc desc;
  desc.clamp[0] = uint8_t((fetch[0] >> 10) & 7);
  desc.clamp[1] = uint8_t((fetch[0] >> 13) & 7);
  desc.clamp[2] = uint8_t((fetch[0] >> 16) & 7);
  desc.mag = uint8_t((fetch[3] >> 19) & 3);
  desc.min = uint8_t((fetch[3] >> 21) & 3);
  desc.mip = uint8_t((fetch[3] >> 23) & 3);
  desc.lod_bias = int16_t(int32_t(fetch[4] << 10) >> 22);  // signed 10 bits at 12
  const uint32_t key = desc.clamp[0] | desc.clamp[1] << 3 | desc.clamp[2] << 6 | desc.mag << 9 |
                       desc.min << 11 | desc.mip << 13 | uint32_t(desc.lod_bias & 0x3FF) << 15;
  if (auto found = samplers_.find(key); found != samplers_.end())
    return found->second;
  if (next_sampler_ >= kMaxSamplers)
    return 0;
  desc.id = next_sampler_++;
  samplers_.emplace(key, uint16_t(desc.id));
  frame.new_samplers.push_back(desc);
  return uint16_t(desc.id);
}

}  // namespace d3::shadow
