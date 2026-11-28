// SPDX-License-Identifier: Apache-2.0
// CLAMP_F32 a, b, imm lo, imm hi: r[a] = min(max(r[b], lo), hi), the immediates the bits of two f32.
// Two comparisons, not a min and a max: a NaN fails both and passes through, so STORE_SLOT refuses
// it as it would an unclamped value (0005 item 32) and the clamp hides nothing.
// Ref: docs/decisions/0005-material-model.md items 31, 38 (a per-point Mix factor and opacity
// clamped to [0, 1] where they enter weights and emission, as BRANCH_MIX and the opacity decision
// clamp them).
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN float op_clamp_f32(float x, float lo, float hi) {
  const float v = x < lo ? lo : x;
  return v > hi ? hi : v;
}

AR_FN void vm_op_clamp_f32(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1)) return;
  vm.r[vm.a] = op_clamp_f32(vm.r[vm.b], bit_cast<float>(vm_imm(vm, 0)), bit_cast<float>(vm_imm(vm, 1)));
  vm.pc += 3;
}
AR_NS_END
#define handle_CLAMP_F32() ::ar::vm_op_clamp_f32(vm)
