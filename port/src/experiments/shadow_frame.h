// Experiment (gpu-layer-experiment branch): one guest frame's draws, captured on the game
// thread at each draw call and handed to the side-by-side Vulkan renderer at present.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace d3::shadow {

// Each vertex attribute is converted on the CPU to four 32-bit components (floats, or
// unsigned integers for inputs XenosRecomp declares as uint4), so the renderer never deals
// with Xenos vertex formats, byte order or component order.
struct VertexLayout {
  struct Element {
    uint8_t usage = 0;  // D3DDECLUSAGE
    uint8_t index = 0;
  };
  std::vector<Element> elements;  // attribute i lives at byte offset 16 * i
  uint32_t stride() const { return uint32_t(elements.size()) * 16; }
  bool operator==(const VertexLayout &other) const {
    if (elements.size() != other.elements.size())
      return false;
    for (size_t i = 0; i < elements.size(); ++i)
      if (elements[i].usage != other.elements[i].usage || elements[i].index != other.elements[i].index)
        return false;
    return true;
  }
};

// Xenos registers the renderer reads, copied from the device's register shadow.
struct DrawState {
  uint32_t depth_control = 0;   // RB_DEPTHCONTROL (0x2200)
  uint32_t blend_control = 0;   // RB_BLENDCONTROL0 (0x2201)
  uint32_t color_control = 0;   // RB_COLORCONTROL (0x2202)
  uint32_t mode_control = 0;    // PA_SU_SC_MODE_CNTL (0x2205)
  uint32_t vte_control = 0;     // PA_CL_VTE_CNTL (0x2206)
  uint32_t color_mask = 0;      // RB_COLOR_MASK (0x2104)
  uint32_t surface_info = 0;    // RB_SURFACE_INFO (0x2000): pitch, MSAA
  uint32_t color_info = 0;      // RB_COLOR_INFO (0x2001): EDRAM base (bits 0-11), format (16-19)
  uint32_t depth_info = 0;      // RB_DEPTH_INFO (0x2002): EDRAM base (bits 0-11), format (16)
  std::array<float, 6> viewport{};  // PA_CL_VPORT_X/Y/Z SCALE, OFFSET (0x210F-0x2114)
};

// Host texture formats the capture converts Xenos textures into.
enum class TextureFormat : uint8_t {
  kRGBA8, kR8, kRG8, kBC1, kBC2, kBC3, kBC4, kBC5, kRGBA16F, kRGBA16, kRGB10A2, kR5G6B5,
  kA1RGB5, kRGBA4, kR32F, kRG16F, kR16F, kRG16, kR16,
};

// A texture seen for the first time this frame: base level only, untiled and byte-swapped,
// tightly packed rows of blocks (4x4 blocks for BC formats).
struct TextureUpload {
  uint32_t id = 0;  // slot in the renderer's bindless Texture2D array (0: white placeholder)
  uint32_t width = 0, height = 0;
  TextureFormat format = TextureFormat::kRGBA8;
  uint32_t swizzle = 0;  // Xenos fetch swizzle: 3 bits per output component (0-3 xyzw, 4 zero, 5 one)
  std::vector<uint8_t> data;
};

// A sampler state seen for the first time (fetch constant clamp and filter fields).
struct SamplerDesc {
  uint32_t id = 0;  // slot in the renderer's bindless sampler array
  uint8_t clamp[3] = {};  // Xenos ClampMode
  uint8_t mag = 1, min = 1, mip = 1;  // Xenos TextureFilter
};

// Texture ids from this value up name the renderer's resolve targets (host images that
// stand in for textures the game fills with D3DDevice_Resolve): id = kResolveIdBase + slot.
constexpr uint32_t kResolveIdBase = 3000;
constexpr uint32_t kResolveSlots = 1000;

// A D3DDevice_Resolve: copy the bound render target (or depth) into a texture.
struct ResolveRecord {
  uint32_t before_draw = 0;  // runs before draws[before_draw] (after the last draw if == size)
  uint32_t slot = 0;         // resolve target slot (texture id kResolveIdBase + slot)
  bool depth = false;        // flags & 4: resolve the depth buffer
  uint32_t color_info = 0;   // render target at the time (RB_COLOR_INFO / RB_DEPTH_INFO)
  uint32_t depth_info = 0;
  uint32_t width = 0, height = 0;  // destination texture size
  uint32_t xenos_format = 0;       // destination texture format
};

// Pixel shader texture slots tf0-tf15 (D3D samplers s0-s15).
constexpr uint32_t kTextureSlots = 16;

struct DrawRecord {
  uint64_t vertex_shader = 0;  // FNV-1a of the shader header (capture file name)
  uint64_t pixel_shader = 0;   // 0: depth-only draw
  uint32_t primitive = 0;      // Xenos primitive type (4 triangle list, 6 triangle strip, ...)
  uint32_t layout = 0;         // index into FrameRecord::layouts
  uint32_t first_vertex = 0;   // byte offset in FrameRecord::vertices
  uint32_t vertex_count = 0;
  uint32_t first_index = 0;    // in FrameRecord::indices
  uint32_t index_count = 0;
  uint32_t constants = 0;      // in FrameRecord::constants, in float4 units
  uint32_t booleans = 0;       // XenosRecomp g_Booleans: vertex bools 0-15, pixel bools 0-15 << 16
  std::array<uint16_t, kTextureSlots> textures{};  // TextureUpload ids, 0: placeholder
  std::array<uint16_t, kTextureSlots> samplers{};  // SamplerDesc ids
  DrawState state;
};

struct FrameRecord {
  uint64_t guest_frame = 0;
  std::vector<DrawRecord> draws;
  std::vector<VertexLayout> layouts;
  std::vector<uint8_t> vertices;   // per draw: vertex_count * layout stride bytes
  std::vector<uint32_t> indices;   // relative to the draw's first vertex
  // Per draw: 256 vertex shader float4 constants, then 256 pixel shader float4 constants.
  std::vector<std::array<float, 4>> constants;
  uint32_t skipped = 0;            // draws the capture could not convert
  std::vector<TextureUpload> new_textures;
  std::vector<SamplerDesc> new_samplers;
  std::vector<ResolveRecord> resolves;
};

constexpr uint32_t kConstantsPerDraw = 512;

}  // namespace d3::shadow
