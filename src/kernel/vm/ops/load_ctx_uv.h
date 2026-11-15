// SPDX-License-Identifier: Apache-2.0
// LOAD_CTX_UV a, b: r[a..a+2] = (u, v, 0) of UV set b. The set index is the b byte because the
// op has no register input; the z component is written so a reused register file never leaks
// the previous program's value.
// Ref: docs/decisions/0005-material-model.md items 31, 33 (UV sets by index).
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void op_load_ctx_uv(const ShadeContext& ctx, uint32_t set, float* out) {
  out[0] = float(ctx.uv[set].x);
  out[1] = float(ctx.uv[set].y);
  out[2] = 0.0f;
}

AR_FN void vm_op_load_ctx_uv(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3)) return;
  if (vm.b >= kMaxUvSets) return vm_fail(vm, VmStatus::BAD_OPERAND);
  op_load_ctx_uv(*vm.ctx, vm.b, vm.r + vm.a);
  vm.pc += 1;
}
AR_NS_END
#define handle_LOAD_CTX_UV() ::ar::vm_op_load_ctx_uv(vm)
