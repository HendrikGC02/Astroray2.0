// SPDX-License-Identifier: Apache-2.0
// CONST_F32X3 a, imm x3: r[a..a+2] = the immediates' bits as f32.
// Ref: docs/decisions/0005-material-model.md item 31.
#pragma once
#include "common.h"
#include "const_f32.h"

AR_NS_BEGIN
AR_FN void vm_op_const_f32x3(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3)) return;
  for (uint32_t i = 0; i < 3; ++i) vm.r[vm.a + i] = op_const_f32(vm_imm(vm, i));
  vm.pc += 4;
}
AR_NS_END
#define handle_CONST_F32X3() ::ar::vm_op_const_f32x3(vm)
