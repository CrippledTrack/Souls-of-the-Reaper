// Experiment (Shadow renderer): side-by-side captures. See shadow_compare.h.
#include "experiments/shadow_compare.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace d3::shadow::compare {
namespace {

uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static const bool ready = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)ready;
  crc = ~crc;
  for (size_t i = 0; i < size; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBe32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void Chunk(FILE *file, const char type[4], const std::vector<uint8_t> &data) {
  std::vector<uint8_t> header;
  PutBe32(header, uint32_t(data.size()));
  header.insert(header.end(), type, type + 4);
  std::fwrite(header.data(), 1, header.size(), file);
  if (!data.empty())
    std::fwrite(data.data(), 1, data.size(), file);
  uint32_t crc = Crc32(reinterpret_cast<const uint8_t *>(type), 4);
  crc = Crc32(data.data(), data.size(), crc);
  std::vector<uint8_t> tail;
  PutBe32(tail, crc);
  std::fwrite(tail.data(), 1, tail.size(), file);
}

// RGB PNG with "stored" (uncompressed) DEFLATE blocks: no zlib needed, large but simple.
bool WritePng(const std::filesystem::path &path, const Image &image) {
  FILE *file = std::fopen(path.string().c_str(), "wb");
  if (!file)
    return false;
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::fwrite(signature, 1, 8, file);
  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, image.width);
  PutBe32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  Chunk(file, "IHDR", ihdr);

  // Rows: filter byte 0, then RGB.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(image.height) * (image.width * 3 + 1));
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const uint8_t *row = &image.rgba[size_t(y) * image.width * 4];
    for (uint32_t x = 0; x < image.width; ++x)
      raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last)
      break;
  }
  PutBe32(z, (b << 16) | a);
  Chunk(file, "IDAT", z);
  Chunk(file, "IEND", {});
  std::fclose(file);
  return true;
}

// Box-filtered resample (the Vulkan picture may be drawn at another resolution).
Image Resample(const Image &source, uint32_t width, uint32_t height) {
  if (source.width == width && source.height == height)
    return source;
  Image out{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t y0 = uint32_t(uint64_t(y) * source.height / height);
    const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * source.height / height));
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t x0 = uint32_t(uint64_t(x) * source.width / width);
      const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * source.width / width));
      uint32_t sum[4] = {};
      for (uint32_t sy = y0; sy < y1; ++sy)
        for (uint32_t sx = x0; sx < x1; ++sx)
          for (int c = 0; c < 4; ++c)
            sum[c] += source.rgba[(size_t(sy) * source.width + sx) * 4 + c];
      const uint32_t n = (y1 - y0) * (x1 - x0);
      for (int c = 0; c < 4; ++c)
        out.rgba[(size_t(y) * width + x) * 4 + c] = uint8_t(sum[c] / n);
    }
  }
  return out;
}

void Save(std::filesystem::path dir, uint64_t frame, Image vulkan, Image rexglue) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  static std::atomic<uint32_t> counter{0};
  char stem[64];
  std::snprintf(stem, sizeof(stem), "%04u_frame%llu", counter.fetch_add(1),
                static_cast<unsigned long long>(frame));
  const std::string base = stem;
  WritePng(dir / (base + "_vulkan.png"), vulkan);
  if (rexglue.width == 0) {
    REXLOG_INFO("shadow_compare: saved {} (Vulkan only)", (dir / (base + "_vulkan.png")).string());
    return;
  }
  WritePng(dir / (base + "_rexglue.png"), rexglue);

  // ReXGlue | Vulkan | difference x4, all at ReXGlue's size.
  const uint32_t w = rexglue.width, h = rexglue.height;
  const Image v = Resample(vulkan, w, h);
  Image combined{w * 3, h, std::vector<uint8_t>(size_t(w) * 3 * h * 4, 255)};
  uint64_t total = 0, differing = 0;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t *r = &rexglue.rgba[(size_t(y) * w + x) * 4];
      const uint8_t *k = &v.rgba[(size_t(y) * w + x) * 4];
      uint8_t *row = &combined.rgba[(size_t(y) * w * 3) * 4];
      std::copy(r, r + 3, row + size_t(x) * 4);
      std::copy(k, k + 3, row + size_t(w + x) * 4);
      int largest = 0;
      for (int c = 0; c < 3; ++c) {
        const int d = std::abs(int(r[c]) - int(k[c]));
        largest = std::max(largest, d);
        total += uint64_t(d);
        row[size_t(2 * w + x) * 4 + c] = uint8_t(std::min(255, d * 4));
      }
      if (largest > 16)
        ++differing;
    }
  WritePng(dir / (base + "_compare.png"), combined);
  REXLOG_INFO("shadow_compare: saved {} (mean difference {:.2f}/255, {:.1f}% of pixels differ by "
              "more than 16; ReXGlue {}x{}, Vulkan {}x{})",
              (dir / (base + "_compare.png")).string(),
              double(total) / (double(w) * h * 3), 100.0 * double(differing) / (double(w) * h), w, h,
              vulkan.width, vulkan.height);
}

}  // namespace

void SaveAsync(const std::filesystem::path &dir, uint64_t guest_frame, Image vulkan,
               Image rexglue) {
  std::thread(Save, dir, guest_frame, std::move(vulkan), std::move(rexglue)).detach();
}

}  // namespace d3::shadow::compare
