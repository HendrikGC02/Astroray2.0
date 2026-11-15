// SPDX-License-Identifier: Apache-2.0
// EMIT_LE a: Le += lift_ILLUMINANT(r[a..a+2]) * r[a+3], the colour in three registers and the
// strength in the one after them. Le is a running sum so several emission nodes add; the entry's
// caller zeroes it first.
// Ref: docs/decisions/0005-material-model.md items 10 (ILLUMINANT lift), 31, 38.
#pragma once
#include "common.h"
#include "rgb_lift.h"

AR_NS_BEGIN
AR_FN void op_emit_le(const SceneTables& t, const float* rgb, float strength, const Lambda& lam, Spec* le) {
  const Spec s = op_rgb_lift(t, rgb, SpectrumSemantic::ILLUMINANT, lam);
  for (int i = 0; i < kLanes; ++i) le->v[i] += s.v[i] * Real(strength);
}

AR_FN void vm_op_emit_le(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 4)) return;
  if (vm.le == nullptr || vm.tables == nullptr || vm.tables->rgb2spec == nullptr)
    return vm_fail(vm, VmStatus::NULL_POINTER);
  op_emit_le(*vm.tables, vm.r + vm.a, vm.r[vm.a + 3], vm.ctx->lambda_t, vm.le);
  vm.pc += 1;
}
AR_NS_END
#define handle_EMIT_LE() ::ar::vm_op_emit_le(vm)
