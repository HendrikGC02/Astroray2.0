// SPDX-License-Identifier: Apache-2.0
// DIV_F32 a, b, c: r[a] = r[b] / r[c]. The IEEE 754 single divide, with no special case for a zero
// denominator: the compiler emits it only where 0005 item 38 defines a quotient, and STORE_SLOT
// refuses a non-finite result rather than letting it into the record.
// Ref: docs/decisions/0005-material-model.md items 31, 38 (a survivor's closure_weight s a / a').
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN float op_div_f32(float x, float y) { return x / y; }

AR_FN void vm_op_div_f32(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 1) || !vm_regs_ok(vm, vm.c, 1)) return;
  vm.r[vm.a] = op_div_f32(vm.r[vm.b], vm.r[vm.c]);
  vm.pc += 1;
}
AR_NS_END
#define handle_DIV_F32() ::ar::vm_op_div_f32(vm)
