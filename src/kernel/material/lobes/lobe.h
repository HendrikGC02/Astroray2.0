// SPDX-License-Identifier: Apache-2.0
// Result types shared by every lobe. A lobe is a pure function of explicit
// uniforms: sample, eval and pdf all work in the local shading frame
// (core/frame.h, z = shading normal), and f excludes the cosine factor.
// Ref: docs/decisions/0005-material-model.md item 8 (TransportMode, rejection of
// samples below the geometric surface by the caller); 0004 D1 (f excludes cos);
// 0009 d14 (pure sample/eval/pdf functions).
#pragma once
#include <cstdint>

#include "core/frame.h"
#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "portability.h"

AR_NS_BEGIN

// Which quantity a path carries; selects the eta^2 factor and the adjoint
// shading-normal correction of lobes that have them (Veach 1997 5.2-5.3).
enum class TransportMode : uint32_t { Radiance, Importance };

// Bit flags of LobeSample::flags.
inline constexpr uint32_t LOBE_DELTA = 1u << 0;
inline constexpr uint32_t LOBE_REFLECTION = 1u << 1;
inline constexpr uint32_t LOBE_TRANSMISSION = 1u << 2;

// pdf = 0 means "no sample" (wo below the shading hemisphere); callers terminate
// the path then.
struct LobeSample {
  Real3 wi;
  Spec f;
  Real pdf;
  uint32_t flags;
};

struct LobeEval {
  Spec f;
  Real pdf;
};

AR_NS_END
