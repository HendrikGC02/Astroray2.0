// SPDX-License-Identifier: Apache-2.0
// MUL_F32 a, b, c: r[a] = r[b] * r[c].
// Ref: docs/decisions/0005-material-model.md item 31.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN float op_mul_f32(float x, float y) { return x * y; }

AR_FN void vm_op_mul_f32(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1) || !vm_regs_ok(vm, vm.c, 1)) return;
  vm.r[vm.a] = op_mul_f32(vm.r[vm.b], vm.r[vm.c]);
  vm.pc += 1;
}
AR_NS_END
#define handle_MUL_F32() ::ar::vm_op_mul_f32(vm)
