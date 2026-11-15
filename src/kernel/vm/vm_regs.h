// SPDX-License-Identifier: Apache-2.0
// The VM register file: one array per kernel, passed by pointer to every interpreter call.
// Ref: docs/decisions/0005-material-model.md item 3 (32 x f32 = 128 B, part of the stack split).
#pragma once

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

struct VmRegs {
  float r[32];
};

static_assert(sizeof(VmRegs) == 128, "VmRegs is 128 B (0005 item 3)");

AR_NS_END
