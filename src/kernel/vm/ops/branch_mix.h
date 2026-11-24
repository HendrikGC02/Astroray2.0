// SPDX-License-Identifier: Apache-2.0
// BRANCH_MIX ru, rf, off: one Mix Shader's selection. With f = clamp(r[rf], 0, 1), a uniform
// u = r[ru] below 1 - f keeps the first child: u becomes u / (1 - f) and the run falls through.
// Otherwise u becomes (u - (1 - f)) / f and the pc jumps `off` words (from the BRANCH_MIX word)
// to the second child. Given the choice, the rescaled u is again uniform on [0, 1), so a nested
// Mix and the lobe choice after the program reuse the same sampler dimension; as in pbrt-v4 the
// result is kept below 1, which the rounded division could otherwise reach.
// A NaN factor, a u outside [0, 1) or a non-positive offset stops the run: a NaN would otherwise
// pick a side silently, and control flow is forward only.
// Ref: docs/decisions/0005-material-model.md items 31, 38; pbrt-v4 src/pbrt/util/sampling.h
// SampleDiscrete (uRemapped = min((up - sum) / weights[offset], OneMinusEpsilon); Apache-2.0);
// Cycles src/kernel/svm/closure.h svm_node_mix_closure (saturated factor, first closure 1 - f,
// second f; Apache-2.0).
#pragma once
#include "common.h"

AR_NS_BEGIN
// The largest float below 1 (pbrt-v4 FloatOneMinusEpsilon).
inline constexpr float kBranchOneMinusEpsilon = 0x1.fffffep-1f;

// True when the second child is selected; *u is rescaled in place. f must not be NaN and *u must
// lie in [0, 1).
AR_FN bool op_branch_mix(float f, float* u) {
  const float fc = f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f;
  const float p_first = 1.0f - fc;
  const bool second = !(*u < p_first);
  const float r = second ? (*u - p_first) / fc : *u / p_first;
  *u = r < kBranchOneMinusEpsilon ? r : kBranchOneMinusEpsilon;
  return second;
}

AR_FN void vm_op_branch_mix(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1)) return;
  const int32_t off = int32_t(vm_imm(vm, 0));
  if (off <= 0) return vm_fail(vm, VmStatus::BAD_JUMP);
  const float f = vm.r[vm.b];
  float u = vm.r[vm.a];
  if (f != f || !(u >= 0.0f && u < 1.0f)) return vm_fail(vm, VmStatus::BAD_OPERAND);
  const bool second = op_branch_mix(f, &u);
  vm.r[vm.a] = u;
  vm.pc += second ? uint32_t(off) : 2u;
}
AR_NS_END
#define handle_BRANCH_MIX() ::ar::vm_op_branch_mix(vm)
