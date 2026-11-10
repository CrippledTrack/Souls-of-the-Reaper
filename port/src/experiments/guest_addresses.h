// Experiment (Shadow renderer): the guest functions it hooks, for each supported executable.
//
// The base disc and the title update differ only in where things are linked: the Direct3D
// library sits 0x81C0 lower on the base disc and the game's own code 0x3C30 lower, with
// identical function bodies apart from global data addresses (matched by normalized body,
// 2026-11-09). Hooks name their function through these macros:
//
//   D3_DECLARE(D3_FN_DRAW_INDEXED);                    declares sub_<address> and its original
//   D3_HOOK(D3_FN_DRAW_INDEXED) { ...; D3_ORIGINAL(D3_FN_DRAW_INDEXED)(ctx, base); }
//   D3_ADDRESS(D3_FN_DRAW_INDEXED)                     the address as a uint32_t
#pragma once

#ifdef SOULS_TITLE_UPDATE
#include "generated/update/diablo3_pch.h"

// Direct3D library.
#define D3_FN_DRAW_INDEXED 82D621B0      // D3DDevice_DrawIndexedVertices
#define D3_FN_DRAW 82D61D98              // D3DDevice_DrawVertices (non-indexed)
#define D3_FN_PRESENT 82D58DD8           // ends a frame (calls VdSwap)
#define D3_FN_CLEAR 82D57638             // D3DDevice_Clear
#define D3_FN_RESOLVE 82D5B9D0           // D3DDevice_Resolve
#define D3_FN_VERTEX_SHADER_INIT 82D692A0
#define D3_FN_PIXEL_SHADER_INIT 82D66188
#define D3_D3D_LIBRARY_START 0x82D40000u
#define D3_D3D_LIBRARY_END 0x82DB0000u
// Other Direct3D entry points that draw (draw_trace.cpp warns when the game uses them).
#define D3_DRAW_VARIANTS                                                                   \
  0x82D59AD8u, 0x82D59D30u, 0x82D5A600u, 0x82D612B0u, 0x82D617B8u, 0x82D62628u,           \
      0x82D62AC8u, 0x82D71B28u, 0x82D72700u, 0x82D73030u, 0x82D73450u, 0x82D7A1C8u
// Game code.
#define D3_FN_DISPLAY_MODE 82579690      // picks the display size from XGetVideoMode
#define D3_CALL_RENDERER_DRAW 0x82584464u  // return address of the renderer's indexed draw
#define D3_RANGE_HOOKS "d3d_range_hooks.inc"

#else  // base disc
#include "generated/base/diablo3_pch.h"

#define D3_FN_DRAW_INDEXED 82D59FF0
#define D3_FN_DRAW 82D59BD8
#define D3_FN_PRESENT 82D50C18
#define D3_FN_CLEAR 82D4F478
#define D3_FN_RESOLVE 82D53810
#define D3_FN_VERTEX_SHADER_INIT 82D610E0
#define D3_FN_PIXEL_SHADER_INIT 82D5DFC8
#define D3_D3D_LIBRARY_START 0x82D37E40u
#define D3_D3D_LIBRARY_END 0x82DA7E40u
#define D3_DRAW_VARIANTS                                                                   \
  0x82D51918u, 0x82D51B70u, 0x82D52440u, 0x82D590F0u, 0x82D595F8u, 0x82D5A468u,           \
      0x82D5A908u, 0x82D69968u, 0x82D6A540u, 0x82D6AE70u, 0x82D6B290u, 0x82D72008u
#define D3_FN_DISPLAY_MODE 82575A60
#define D3_CALL_RENDERER_DRAW 0x82580834u
#define D3_RANGE_HOOKS "d3d_range_hooks_base.inc"
#endif

#define D3_DECLARE_(a) DECLARE_REX_FUNC(sub_##a)
#define D3_DECLARE(a) D3_DECLARE_(a)
#define D3_HOOK_(a) extern "C" void sub_##a(PPCContext &ctx, uint8_t *base)
#define D3_HOOK(a) D3_HOOK_(a)
#define D3_ORIGINAL_(a) __imp__sub_##a
#define D3_ORIGINAL(a) D3_ORIGINAL_(a)
#define D3_ADDRESS_(a) 0x##a##u
#define D3_ADDRESS(a) D3_ADDRESS_(a)
