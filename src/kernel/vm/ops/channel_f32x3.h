// SPDX-License-Identifier: Apache-2.0
// CHANNEL_F32X3 a, b, imm channel: r[a] = r[b + channel], channel 0-2 (else BAD_OPERAND). The one
// op that turns a colour into a per-point scalar, so an image channel can drive a scalar slot such
// as geometry_opacity.
// Ref: docs/decisions/0005-material-model.md items 31, 32 (the op and the opacity entry).
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN float op_channel_f32x3(const float* x, uint32_t channel) { return x[channel]; }

AR_FN void vm_op_channel_f32x3(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 1) || !vm_regs_ok(vm, vm.b, 3)) return;
  const uint32_t channel = vm_imm(vm, 0);
  if (channel > 2u) return vm_fail(vm, VmStatus::BAD_OPERAND);
  vm.r[vm.a] = op_channel_f32x3(vm.r + vm.b, channel);
  vm.pc += 2;
}
AR_NS_END
#define handle_CHANNEL_F32X3() ::ar::vm_op_channel_f32x3(vm)
