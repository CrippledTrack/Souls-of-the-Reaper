// Experiment (gpu-layer-experiment branch): capture each draw's state on the game thread for
// the side-by-side Vulkan renderer. Device layout and structures: see
// experiments/gpu-layer/RENDERING.md (USA title update).
#include "shadow_snapshot.h"

#include <chrono>
#include <mutex>
#include <vector>

#include <rex/logging.h>

#include <algorithm>
#include <cstring>

namespace d3::shadow {
namespace {

// Device field offsets (register shadow and bound objects).
constexpr uint32_t kFetchConstants = 1152;   // 0x4800: 32 slots x 6 dwords
constexpr uint32_t kVertexConstants = 1920;  // 0x4000: 256 float4
constexpr uint32_t kPixelConstants = 6016;   // 0x4400: 256 float4
constexpr uint32_t kBoolConstants = 10112;   // 0x4900: 8 dwords
constexpr uint32_t kRegs2000 = 10368;        // 0x2000..: RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO
constexpr uint32_t kRegs2100 = 10444;        // 0x2100..0x2114
constexpr uint32_t kRegs2200 = 10548;        // 0x2200..0x220B
constexpr uint32_t kVertexDeclaration = 12240;
constexpr uint32_t kIndexBuffer = 12812;
constexpr uint32_t kPixelShader = 13068;
constexpr uint32_t kVertexShader = 13072;

constexpr uint32_t kMaxVertices = 1u << 20;

uint32_t Be32(const uint8_t *base, uint32_t address) {
  const uint8_t *p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t *base, uint32_t address) {
  const uint8_t *p = base + address;
  return uint16_t((p[0] << 8) | p[1]);
}
float BeFloat(const uint8_t *base, uint32_t address) {
  const uint32_t bits = Be32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
float Half(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  int32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FF;
      bits = sign | uint32_t(exponent + 112) << 23 | mantissa << 13;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000 | mantissa << 13;
  } else {
    bits = sign | uint32_t(exponent + 112) << 23 | mantissa << 13;
  }
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Guest address of physical memory (fetch constants hold physical addresses).
uint32_t PhysicalToGuest(uint32_t physical) { return 0xA0000000u | (physical & 0x1FFFFFFFu); }

// Size in bytes of a Xenos vertex format (the low six bits of a D3DDECLTYPE).
uint32_t FormatSize(uint32_t format) {
  switch (format) {
    case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36: return 4;
    case 26: case 32: case 34: case 37: return 8;
    case 57: return 12;
    case 35: case 38: return 16;
    default: return 0;
  }
}

// XenosRecomp declares BLENDINDICES, NORMAL, TANGENT and BINORMAL as uint4. Only blend
// indices are used as integers. The other three go through tfetchR11G11B10(), which without
// the packed-normal spec constant is asfloat(value), so they take float bits like any input.
bool UintUsage(uint8_t usage) { return usage == 2; }

// Decode one element into four 32-bit components. D3DDECLTYPE bits used: 0-5 Xenos format,
// 8 signed, 9 integer (not normalised: UBYTE4 0x1A2286 vs UBYTE4N 0x1A2086, SHORT4 0x1A235A vs
// SHORT4N 0x1A215A), 11 D3DCOLOR (ARGB) order. Values are read big-endian per
// component, which is what they mean to the guest CPU whatever the GPU endian swap.
void Decode(const uint8_t *base, uint32_t address, uint32_t type, bool as_uint, uint32_t out[4]) {
  const uint32_t format = type & 0x3F;
  const bool is_signed = type & 0x100;
  const bool normalized = !(type & 0x200);
  float f[4] = {0, 0, 0, 1};
  uint32_t u[4] = {0, 0, 0, 1};
  switch (format) {
    case 6: {  // 8_8_8_8
      const uint32_t v = Be32(base, address);
      // Xenos 8_8_8_8: x is the low byte of the dword (the last byte in guest memory).
      uint32_t c[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24};
      if (type & 0x800) {  // D3DCOLOR: dword 0xAARRGGBB, used as RGBA
        const uint32_t rgba[4] = {(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, v >> 24};
        std::copy(rgba, rgba + 4, c);
      }
      for (int i = 0; i < 4; ++i) {
        u[i] = c[i];
        if (is_signed) {
          const int8_t s8 = int8_t(c[i]);
          f[i] = normalized ? std::max(float(s8) / 127.0f, -1.0f) : float(s8);
        } else {
          f[i] = normalized ? float(c[i]) / 255.0f : float(c[i]);
        }
      }
      break;
    }
    case 25: case 26: {  // 16_16, 16_16_16_16
      const int n = format == 25 ? 2 : 4;
      for (int i = 0; i < n; ++i) {
        const uint16_t raw = Be16(base, address + i * 2);
        u[i] = raw;
        if (is_signed) {
          const int16_t s = int16_t(raw);
          f[i] = normalized ? std::max(float(s) / 32767.0f, -1.0f) : float(s);
        } else {
          f[i] = normalized ? float(raw) / 65535.0f : float(raw);
        }
      }
      if (n == 2) { f[2] = 0; f[3] = 1; }
      break;
    }
    case 31: case 32: {  // 16_16_FLOAT, 16_16_16_16_FLOAT
      const int n = format == 31 ? 2 : 4;
      for (int i = 0; i < n; ++i)
        f[i] = Half(Be16(base, address + i * 2));
      break;
    }
    case 33: case 34: case 35: {  // 32, 32_32, 32_32_32_32 (integer)
      const int n = format == 33 ? 1 : format == 34 ? 2 : 4;
      for (int i = 0; i < n; ++i) {
        u[i] = Be32(base, address + i * 4);
        f[i] = is_signed ? float(int32_t(u[i])) : float(u[i]);
      }
      break;
    }
    case 36: case 37: case 38: case 57: {  // 32_FLOAT .. 32_32_32_32_FLOAT, 32_32_32_FLOAT
      const int n = format == 36 ? 1 : format == 37 ? 2 : format == 57 ? 3 : 4;
      for (int i = 0; i < n; ++i)
        f[i] = BeFloat(base, address + i * 4);
      break;
    }
    default:
      break;  // 2_10_10_10 and 10_11_11 variants: not decoded yet (zeros)
  }
  if (as_uint) {
    std::memcpy(out, u, 16);
  } else {
    std::memcpy(out, f, 16);
  }
}

uint64_t ShaderHash(const uint8_t *base, uint32_t object) {
  if (object == 0)
    return 0;
  for (uint32_t offset = 0; offset < 1024; offset += 4) {
    if ((Be32(base, object + offset) & 0xFFFFFF00) != 0x102A1100)
      continue;
    const uint32_t size = Be32(base, object + offset + 4);
    if (size == 0 || size > (1u << 20))
      return 0;
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < size; ++i) {
      hash ^= base[object + offset + i];
      hash *= 0x100000001B3ull;
    }
    return hash;
  }
  return 0;
}

}  // namespace

namespace {
std::mutex recycle_mutex;
std::vector<std::unique_ptr<FrameRecord>> recycled;
}  // namespace

std::unique_ptr<FrameRecord> NewFrameRecord() {
  {
    std::lock_guard lock(recycle_mutex);
    if (!recycled.empty()) {
      auto frame = std::move(recycled.back());
      recycled.pop_back();
      return frame;
    }
  }
  return std::make_unique<FrameRecord>();
}

void RecycleFrameRecord(std::unique_ptr<FrameRecord> frame) {
  if (!frame)
    return;
  frame->Reset();
  std::lock_guard lock(recycle_mutex);
  if (recycled.size() < 4)
    recycled.push_back(std::move(frame));
}

namespace {
// Capture phase timing (game thread only): indices, vertices, constants, textures.
uint64_t phase_ns[4] = {};
struct PhaseTimer {
  int phase;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  void Next(int next) {
    const auto now = std::chrono::steady_clock::now();
    phase_ns[phase] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
    phase = next;
    start = now;
  }
  ~PhaseTimer() { Next(phase); }
};
}  // namespace

void SnapshotBuilder::RecordDraw(const uint8_t *base, uint32_t device, uint32_t primitive,
                                 uint32_t base_vertex, uint32_t start_index, uint32_t index_count,
                                 bool indexed) {
  if (!frame_)
    frame_ = NewFrameRecord();
  FrameRecord &frame = *frame_;
  auto skip = [&](const char *why) {
    if (frame.skipped++ < 3)
      REXLOG_INFO("shadow_renderer: skipped a draw: {}", why);
  };

  DrawRecord draw;
  draw.vertex_shader = ShaderHash(base, Be32(base, device + kVertexShader));
  draw.pixel_shader = ShaderHash(base, Be32(base, device + kPixelShader));
  draw.primitive = primitive;
  if (!draw.vertex_shader)
    return skip("no vertex shader");

  // Vertex declaration: element count at +24, 12-byte D3DVERTEXELEMENT9 entries from +52.
  const uint32_t decl = Be32(base, device + kVertexDeclaration);
  if (!decl)
    return skip("no vertex declaration");
  const uint32_t element_count = std::min<uint32_t>(Be32(base, decl + 24), 16);
  struct Element {
    uint32_t stream, offset, type;
    uint8_t usage, index;
  };
  std::vector<Element> elements;
  uint32_t strides[16] = {};
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t at = decl + 52 + i * 12;
    const uint32_t a = Be32(base, at), type = Be32(base, at + 4), b = Be32(base, at + 8);
    Element e{a >> 16, a & 0xFFFF, type, uint8_t((b >> 16) & 0xFF), uint8_t((b >> 8) & 0xFF)};
    if (e.stream >= 16 || !FormatSize(type & 0x3F))
      continue;
    // The stride is not stored with the stream; assume tightly packed vertices.
    strides[e.stream] = std::max(strides[e.stream], e.offset + FormatSize(type & 0x3F));
    elements.push_back(e);
  }
  if (elements.empty())
    return skip("empty vertex declaration");

  VertexLayout layout;
  for (const Element &e : elements)
    layout.elements.push_back({e.usage, e.index});
  auto found = std::find(frame.layouts.begin(), frame.layouts.end(), layout);
  draw.layout = uint32_t(found - frame.layouts.begin());
  if (found == frame.layouts.end())
    frame.layouts.push_back(layout);

  // Indices.
  PhaseTimer timer{0};
  if (index_count == 0)
    return skip("no vertices");
  std::vector<uint32_t> indices(index_count);
  uint32_t lowest = UINT32_MAX, highest = 0;
  if (!indexed) {
    // Non-indexed: vertices 0..count-1 from the start vertex (passed as base_vertex).
    for (uint32_t i = 0; i < index_count; ++i)
      indices[i] = i;
    lowest = 0;
    highest = index_count - 1;
  } else {
    const uint32_t index_buffer = Be32(base, device + kIndexBuffer);
    if (!index_buffer)
      return skip("no index buffer");
    const bool wide = Be32(base, index_buffer) & 0x80000000u;
    const uint32_t index_base = Be32(base, index_buffer + 24);
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint32_t index = wide ? Be32(base, index_base + (start_index + i) * 4)
                                  : Be16(base, index_base + (start_index + i) * 2);
      indices[i] = index;
      lowest = std::min(lowest, index);
      highest = std::max(highest, index);
    }
  }
  const uint32_t vertex_count = highest - lowest + 1;
  if (vertex_count > kMaxVertices)
    return skip("vertex range too large");

  // Vertices: stream s reads vertex fetch constant 95 - s.
  timer.Next(1);
  const uint32_t stride = layout.stride();
  draw.vertex_count = vertex_count;
  const size_t vertex_bytes = size_t(vertex_count) * stride;
  uint32_t stream_bases[16] = {};
  bool stream_used[16] = {};
  for (const Element &e : elements) {
    const uint32_t fetch = 95 - e.stream;
    const uint32_t word0 = Be32(base, device + kFetchConstants + (fetch / 3) * 24 + (fetch % 3) * 8);
    if ((word0 & 3) != 3)
      return skip("vertex fetch constant not set");
    stream_bases[e.stream] = PhysicalToGuest(word0 & 0xFFFFFFFCu);
    stream_used[e.stream] = true;
  }

  // Most of a frame is static meshes, so converted vertices are kept across frames, keyed by
  // where they come from and how they are read, and checked against the source bytes: whole
  // for small ranges, at sampled points for large ones.
  uint64_t key = 0xCBF29CE484222325ull;
  for (const Element &e : elements) {
    const uint32_t parts[6] = {stream_bases[e.stream], strides[e.stream], e.offset, e.type,
                               uint32_t(e.usage) << 8 | e.index, e.stream};
    key = HashBytes(parts, sizeof(parts), key);
  }
  const uint32_t range[2] = {base_vertex + lowest, vertex_count};
  key = HashBytes(range, sizeof(range), key);
  CachedVertices &cached = vertex_cache_[key];
  cached.last_used = frame_number_;
  const bool dynamic = cached.changes >= 2;
  uint64_t signature = 0;
  if (!dynamic) {
    constexpr size_t kWholeHashBytes = 16 * 1024;
    constexpr size_t kSamples = 64;
    for (uint32_t st = 0; st < 16; ++st) {
      if (!stream_used[st])
        continue;
      const uint8_t *source = base + stream_bases[st] + size_t(base_vertex + lowest) * strides[st];
      const size_t bytes = size_t(vertex_count) * strides[st];
      if (bytes <= kWholeHashBytes) {
        signature = HashBytes(source, bytes, signature);
      } else {
        for (size_t k = 0; k < kSamples; ++k)
          signature = HashBytes(source + (bytes - 16) * k / (kSamples - 1), 16, signature);
      }
    }
    if (cached.block && cached.signature == signature) {
      draw.vertex_block = cached.block;  // unchanged: reuse the converted block
    } else if (cached.block) {
      // Changed in place. Retire the old block (the renderer may still be drawing it).
      freed_blocks_.push_back(cached.block);
      cached.block = 0;
      ++cached.changes;
    }
  }

  if (!draw.vertex_block) {
    std::vector<uint8_t> converted(vertex_bytes);
    for (size_t i = 0; i < elements.size(); ++i) {
      const Element &e = elements[i];
      const uint32_t stream_base = stream_bases[e.stream];
      for (uint32_t v = 0; v < vertex_count; ++v) {
        const uint32_t source =
            stream_base + (base_vertex + lowest + v) * strides[e.stream] + e.offset;
        uint32_t components[4];
        Decode(base, source, e.type, UintUsage(e.usage), components);
        std::memcpy(&converted[size_t(v) * stride + i * 16], components, 16);
      }
    }
    // Ranges seen for the first time go inline too: dynamic geometry is often written to a
    // new address every frame and would never be drawn from a cached block again.
    const bool first_sighting = !cached.seen || cached.signature != signature;
    cached.seen = true;
    if (cached.changes >= 2 || first_sighting) {
      cached.signature = signature;
      // Keeps changing, or not known to be static yet: send it inline with this frame only.
      const size_t vertex_start = frame.vertices.size();
      frame.vertices.insert(frame.vertices.end(), converted.begin(), converted.end());
      draw.first_vertex = uint32_t(vertex_start);
    } else {
      cached.block = next_vertex_block_++;
      cached.signature = signature;
      draw.vertex_block = cached.block;
      frame.new_vertex_blocks.push_back({cached.block, std::move(converted)});
    }
  }

  draw.first_index = uint32_t(frame.indices.size());
  draw.index_count = index_count;
  for (uint32_t index : indices)
    frame.indices.push_back(index - lowest);

  // Constants and booleans.
  timer.Next(2);
  draw.constants = uint32_t(frame.constants.size());
  frame.constants.resize(frame.constants.size() + kConstantsPerDraw);
  // Bulk copy and byte swap (vectorised by the compiler).
  auto copy_swapped = [&](uint32_t address, std::array<float, 4> *to) {
    uint32_t *words = reinterpret_cast<uint32_t *>(to);
    std::memcpy(words, base + address, 256 * 16);
    for (uint32_t w = 0; w < 256 * 4; ++w)
      words[w] = __builtin_bswap32(words[w]);
  };
  copy_swapped(device + kVertexConstants, &frame.constants[draw.constants]);
  copy_swapped(device + kPixelConstants, &frame.constants[draw.constants + 256]);
  // Textures: pixel shader sampler sN reads texture fetch constant N.
  timer.Next(3);
  for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
    uint32_t fetch[6];
    for (uint32_t w = 0; w < 6; ++w)
      fetch[w] = Be32(base, device + kFetchConstants + slot * 24 + w * 4);
    if ((fetch[0] & 3) != 2)
      continue;
    // Most draws keep the previous draw's textures: reuse its ids for an identical fetch
    // constant (only within the frame; textures are rechecked every frame).
    SlotMemo &memo = slot_memo_[slot];
    if (memo.valid && std::memcmp(memo.fetch, fetch, sizeof(fetch)) == 0) {
      draw.textures[slot] = memo.texture;
      draw.samplers[slot] = memo.sampler;
      continue;
    }
    draw.textures[slot] = textures_.Texture(base, fetch, frame);
    draw.samplers[slot] = textures_.Sampler(fetch, frame);
    std::memcpy(memo.fetch, fetch, sizeof(fetch));
    memo.texture = draw.textures[slot];
    memo.sampler = draw.samplers[slot];
    memo.valid = true;
  }
  draw.booleans = (Be32(base, device + kBoolConstants) & 0xFFFF) |
                  ((Be32(base, device + kBoolConstants + 16) & 0xFFFF) << 16);

  // Render state.
  DrawState &s = draw.state;
  s.depth_control = Be32(base, device + kRegs2200 + 0x0 * 4);
  s.blend_control = Be32(base, device + kRegs2200 + 0x1 * 4);
  s.color_control = Be32(base, device + kRegs2200 + 0x2 * 4);
  s.mode_control = Be32(base, device + kRegs2200 + 0x5 * 4);
  s.vte_control = Be32(base, device + kRegs2200 + 0x6 * 4);
  s.color_mask = Be32(base, device + kRegs2100 + 0x4 * 4);
  s.surface_info = Be32(base, device + kRegs2000 + 0x0 * 4);
  s.color_info = Be32(base, device + kRegs2000 + 0x1 * 4);
  s.depth_info = Be32(base, device + kRegs2000 + 0x2 * 4);
  for (uint32_t i = 0; i < 6; ++i)
    s.viewport[i] = BeFloat(base, device + kRegs2100 + (0xF + i) * 4);
  s.alpha_ref = BeFloat(base, device + kRegs2100 + 0xE * 4);

  frame.draws.push_back(draw);
}

void SnapshotBuilder::RecordResolve(const uint8_t *base, uint32_t device, uint32_t flags,
                                    uint32_t texture) {
  if (!frame_)
    frame_ = NewFrameRecord();
  if (!texture)
    return;
  // The destination texture object holds its fetch constant at +28.
  uint32_t fetch[6];
  for (uint32_t w = 0; w < 6; ++w)
    fetch[w] = Be32(base, texture + 28 + w * 4);
  if ((fetch[0] & 3) != 2)
    return;
  const uint32_t physical = fetch[1] & 0x1FFFF000u;
  // A resolve can turn an address into a resolve target mid-frame: forget remembered ids.
  textures_.EndFrame();
  for (SlotMemo &memo : slot_memo_)
    memo.valid = false;
  auto [it, added] = textures_.resolve_targets.emplace(
      physical, uint32_t(textures_.resolve_targets.size()));
  if (it->second >= kResolveSlots)
    return;
  ResolveRecord r;
  r.before_draw = uint32_t(frame_->draws.size());
  r.sequence = frame_->next_sequence++;
  r.slot = it->second;
  r.depth = flags & 4;
  r.color_info = Be32(base, device + kRegs2000 + 0x1 * 4);
  r.depth_info = Be32(base, device + kRegs2000 + 0x2 * 4);
  r.width = (fetch[2] & 0x1FFF) + 1;
  r.height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  r.xenos_format = fetch[1] & 0x3F;
  frame_->resolves.push_back(r);
}

void SnapshotBuilder::RecordClear(const uint8_t *base, uint32_t device, uint32_t flags,
                                  uint32_t color, float depth, uint32_t stencil) {
  if (!frame_)
    frame_ = NewFrameRecord();
  ClearRecord c;
  c.before_draw = uint32_t(frame_->draws.size());
  c.sequence = frame_->next_sequence++;
  c.flags = flags;
  c.color = color;
  c.depth = depth;
  c.stencil = stencil;
  c.color_info = Be32(base, device + kRegs2000 + 0x1 * 4);
  c.depth_info = Be32(base, device + kRegs2000 + 0x2 * 4);
  frame_->clears.push_back(c);
}

std::unique_ptr<FrameRecord> SnapshotBuilder::Finish(uint64_t guest_frame) {
  textures_.EndFrame();
  for (SlotMemo &memo : slot_memo_)
    memo.valid = false;
  auto frame = std::move(frame_);
  if (!frame)
    frame = NewFrameRecord();
  // Forget vertex ranges unused for ten seconds; their blocks are freed by the renderer.
  ++frame_number_;
  if (frame_number_ % 300 == 0) {
    for (auto it = vertex_cache_.begin(); it != vertex_cache_.end();) {
      if (frame_number_ - it->second.last_used > 600) {
        if (it->second.block)
          freed_blocks_.push_back(it->second.block);
        it = vertex_cache_.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (frame_number_ % 300 == 0) {
    REXLOG_INFO("shadow_snapshot: capture per frame: indices {:.2f} ms, vertices {:.2f} ms, "
                "constants {:.2f} ms, textures {:.2f} ms",
                phase_ns[0] / 300e6, phase_ns[1] / 300e6, phase_ns[2] / 300e6, phase_ns[3] / 300e6);
    std::fill(std::begin(phase_ns), std::end(phase_ns), 0);
  }
  frame->freed_vertex_blocks = std::move(freed_blocks_);
  freed_blocks_.clear();
  frame->guest_frame = guest_frame;
  return frame;
}

}  // namespace d3::shadow
