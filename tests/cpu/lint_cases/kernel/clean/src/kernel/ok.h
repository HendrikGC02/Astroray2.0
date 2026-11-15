// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cfloat>
#include "core/real.h"
AR_NS_BEGIN
// Comments may say virtual, throw, new, std::vector, 0.5 and sqrt(x).
static constexpr Real kHalf = Real(0.5);
static const double kTwo = double(2.0);
static inline Real twice(Real x) { return x * Real(2.0) + float(1e-3) + 2.0f; }
static AR_FN Real root(Real x) { return ar::sqrt(x) + Real(1.0 / 3.0); }
Real step(Real x) {
  static int counter = 0;  // function scope is fine
  const char* s = "std::vector new 0.5 sqrt(";
  (void)s;
#ifdef AR_DEBUG
  printf("%d", counter);
#endif
  return x + Real(0x1e5) + 1'000;
}
AR_NS_END
