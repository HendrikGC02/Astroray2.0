// SPDX-License-Identifier: Apache-2.0
// CONST_F32 a, imm: r[a] = the immediate's bits as an f32.
// Ref: docs/decisions/0005-material-model.md item 31.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN float op_const_f32(uint32_t bits) { return bit_cast<float>(bits); }

AR_FN void vm_op_const_f32(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1)) return;
  vm.r[vm.a] = op_const_f32(vm_imm(vm, 0));
  vm.pc += 2;
}
AR_NS_END
#define handle_CONST_F32() ::ar::vm_op_const_f32(vm)
