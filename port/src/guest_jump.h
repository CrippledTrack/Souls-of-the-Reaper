#pragma once
#include <cstdint>
#include <unordered_map>
// Guest jump state must be shared when codegen puts these functions in
// different files.

struct D3JmpState {
  uint64_t r1;
  uint64_t r13, r14, r15, r16, r17, r18, r19, r20, r21, r22, r23, r24, r25, r26,
      r27, r28, r29, r30, r31;
  uint64_t lr;
  uint64_t f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27,
      f28, f29, f30, f31;
  uint32_t val;
};
inline std::unordered_map<uint32_t, D3JmpState> &d3_jmp_regs() {
  static thread_local std::unordered_map<uint32_t, D3JmpState> m;
  return m;
}
