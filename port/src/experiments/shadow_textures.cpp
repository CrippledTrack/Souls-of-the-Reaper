// Experiment (gpu-layer-experiment branch): decode Xenos textures for the side-by-side renderer.
//
// Reads a texture fetch constant (layout: xe_gpu_texture_fetch_t in rex/graphics/xenos.h),
// untiles the base level, applies the endian swap and keeps compressed formats as BC blocks.
// Each distinct texture gets an id once; the renderer uploads it and indexes it bindlessly.
#include "shadow_textures.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>

namespace d3::shadow {
namespace {

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

}  // namespace

uint16_t TextureCache::Texture(const uint8_t *base, const uint32_t fetch[6], FrameRecord &frame) {
  if ((fetch[0] & 3) != 2)  // not a texture fetch constant
    return 0;
  // Textures the game fills by resolving a render target come from the renderer's images.
  if (auto resolved = resolve_targets.find(fetch[1] & 0x1FFFF000u); resolved != resolve_targets.end())
    return uint16_t(kResolveIdBase + resolved->second);
  const uint32_t dimension = (fetch[5] >> 9) & 3;  // 0 1D, 1 2D, 2 3D or stacked, 3 cube
  const bool stacked = (fetch[1] >> 10) & 1;
  const bool volume = dimension == 2 && !stacked;
  if (!(dimension == 1 && !stacked) && !volume)  // 2D and 3D for now
    return 0;
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
  const uint32_t identity[5] = {fetch[0] & 0xFFC00003u, fetch[1], fetch[2], fetch[3] & 0x1FFFu,
                                fetch[5] & 0xE00u};
  uint64_t key = Fnv(identity, sizeof(identity));
  // Volumes: tiled ones use TiledOffset3D; linear slices are 4 KiB aligned.
  const uint32_t height_aligned = (blocks_h + 31) & ~31u;
  const size_t slice_pitch = (size_t(row_pitch) * blocks_h + 4095) & ~size_t(4095);
  // Packed mips: a base level whose shorter side is 16 texels or less lives in the packed mip
  // tail tile, 16 texels right (square or taller) or down (wider) of the tile origin. From
  // ReXGlue's GetPackedMipOffset for mip 0.
  uint32_t tail_x = 0, tail_y = 0;
  if (!volume && ((fetch[5] >> 11) & 1)) {
    uint32_t log2_w = 0, log2_h = 0;
    while ((1u << log2_w) < width) ++log2_w;
    while ((1u << log2_h) < height) ++log2_h;
    if (std::min(log2_w, log2_h) <= 4)
      (log2_w > log2_h ? tail_y : tail_x) = 16 / info.block;
  }
  auto offset = [&](uint32_t bx, uint32_t by, uint32_t z) -> size_t {
    bx += tail_x;
    by += tail_y;
    if (volume)
      return tiled ? size_t(TiledOffset3D(int32_t(bx), int32_t(by), int32_t(z), pitch_blocks,
                                          height_aligned, bpb_log2))
                   : z * slice_pitch + size_t(by) * row_pitch + size_t(bx) * bpb;
    return tiled ? size_t(TiledOffset2D(int32_t(bx), int32_t(by), pitch_blocks, bpb_log2))
                 : size_t(by) * row_pitch + size_t(bx) * bpb;
  };
  const size_t span =
      volume ? offset(blocks_w - 1, blocks_h - 1, depth - 1) + bpb
      : tiled ? size_t(TiledOffset2D(0, int32_t(blocks_h - 1), pitch_blocks, bpb_log2)) +
                    size_t(pitch_blocks) * 32 * bpb
              : size_t(row_pitch) * blocks_h;
  for (uint32_t i = 0; i < 16; ++i) {
    const size_t at = (span / 16) * i;
    key = Fnv(source + at, std::min<size_t>(16, span - at), key);
  }
  if (auto found = textures_.find(key); found != textures_.end())
    return found->second;
  if (volume ? next_texture_3d_ >= kTexture3DSlots : next_texture_ >= kMaxTextures) {
    if (!full_warned_) {
      full_warned_ = true;
      REXLOG_WARN("shadow_renderer: texture table full ({} textures)", kMaxTextures);
    }
    return 0;
  }

  TextureUpload upload;
  upload.id = volume ? (kTexture3DBit | next_texture_3d_++) : next_texture_++;
  upload.width = width;
  upload.height = height;
  upload.depth = depth;
  upload.format = info.format;
  upload.swizzle = (fetch[3] >> 1) & 0xFFF;
  upload.data.resize(size_t(blocks_w) * blocks_h * depth * bpb);
  for (uint32_t z = 0; z < depth; ++z)
    for (uint32_t by = 0; by < blocks_h; ++by)
      for (uint32_t bx = 0; bx < blocks_w; ++bx) {
        uint8_t *to = &upload.data[((size_t(z) * blocks_h + by) * blocks_w + bx) * bpb];
        std::memcpy(to, source + offset(bx, by, z), bpb);
        Swap(to, bpb, endian);
      }
  if (volume)
    REXLOG_INFO("shadow_renderer: 3D texture {}x{}x{} format {} {} id {}", width, height, depth,
                xenos_format, tiled ? "tiled" : "linear", upload.id & 0x7FFF);
  textures_.emplace(key, uint16_t(upload.id));
  frame.new_textures.push_back(std::move(upload));
  return uint16_t(frame.new_textures.back().id);
}

uint16_t TextureCache::Sampler(const uint32_t fetch[6], FrameRecord &frame) {
  SamplerDesc desc;
  desc.clamp[0] = uint8_t((fetch[0] >> 10) & 7);
  desc.clamp[1] = uint8_t((fetch[0] >> 13) & 7);
  desc.clamp[2] = uint8_t((fetch[0] >> 16) & 7);
  desc.mag = uint8_t((fetch[3] >> 19) & 3);
  desc.min = uint8_t((fetch[3] >> 21) & 3);
  desc.mip = uint8_t((fetch[3] >> 23) & 3);
  const uint32_t key = desc.clamp[0] | desc.clamp[1] << 3 | desc.clamp[2] << 6 | desc.mag << 9 |
                       desc.min << 11 | desc.mip << 13;
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
