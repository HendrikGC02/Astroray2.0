// SPDX-License-Identifier: Apache-2.0
// MUL_F32X3 a, b, c: r[a..a+2] = r[b..b+2] * r[c..c+2], componentwise. The result goes through
// locals so a destination that overlaps a source is read before it is written.
// Ref: docs/decisions/0005-material-model.md item 31.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void op_mul_f32x3(const float* x, const float* y, float* out) {
  const float v0 = x[0] * y[0], v1 = x[1] * y[1], v2 = x[2] * y[2];
  out[0] = v0;
  out[1] = v1;
  out[2] = v2;
}

AR_FN void vm_op_mul_f32x3(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3) || !vm_regs_ok(vm, vm.b, 3) || !vm_regs_ok(vm, vm.c, 3)) return;
  op_mul_f32x3(vm.r + vm.b, vm.r + vm.c, vm.r + vm.a);
  vm.pc += 1;
}
AR_NS_END
#define handle_MUL_F32X3() ::ar::vm_op_mul_f32x3(vm)
