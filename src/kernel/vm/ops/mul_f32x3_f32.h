// SPDX-License-Identifier: Apache-2.0
// MUL_F32X3_F32 a, b, c: r[a..a+2] = r[b..b+2] * r[c] (vector times scalar).
// Ref: docs/decisions/0005-material-model.md item 31.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void op_mul_f32x3_f32(const float* x, float s, float* out) {
  const float v0 = x[0] * s, v1 = x[1] * s, v2 = x[2] * s;
  out[0] = v0;
  out[1] = v1;
  out[2] = v2;
}

AR_FN void vm_op_mul_f32x3_f32(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3) || !vm_regs_ok(vm, vm.c, 1)) return;
  op_mul_f32x3_f32(vm.r + vm.b, vm.r[vm.c], vm.r + vm.a);
  vm.pc += 1;
}
AR_NS_END
#define handle_MUL_F32X3_F32() ::ar::vm_op_mul_f32x3_f32(vm)
