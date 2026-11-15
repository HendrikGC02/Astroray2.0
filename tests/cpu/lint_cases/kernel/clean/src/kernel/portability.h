// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cmath>
#include <intrin.h>
namespace ar {
inline float root(float x) { return std::sqrt(x); }
inline int bits() {
  asm volatile("nop");
  return 0;
}
}  // namespace ar
