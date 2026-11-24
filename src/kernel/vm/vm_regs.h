// SPDX-License-Identifier: Apache-2.0
// The VM register file: one array per kernel, passed by pointer to every interpreter call.
// Ref: docs/decisions/0005-material-model.md item 3 (32 x f32 = 128 B, part of the stack split).
// The selection register (0005 items 31, 38): a surface kernel writes the event's lobe-choice
// uniform into r[kSelectReg] before the surface entry and reads it back afterwards. BRANCH_MIX
// rescales it in place, so the lobe choice continues with the uniform the Mix selection left
// (0004 D14: Mix selection and lobe choice share one uniform). The compiler never allocates it.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

struct VmRegs {
  float r[32];
};

static_assert(sizeof(VmRegs) == 128, "VmRegs is 128 B (0005 item 3)");

inline constexpr uint32_t kSelectReg = 31;

AR_NS_END
