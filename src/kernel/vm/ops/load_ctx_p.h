// SPDX-License-Identifier: Apache-2.0
// LOAD_CTX_P a: r[a..a+2] = the shading point P.
// Ref: docs/decisions/0005-material-model.md items 31, 33.
#pragma once
#include "common.h"

AR_NS_BEGIN
AR_FN void op_load_ctx_p(const ShadeContext& ctx, float* out) {
  out[0] = float(ctx.P.x);
  out[1] = float(ctx.P.y);
  out[2] = float(ctx.P.z);
}

AR_FN void vm_op_load_ctx_p(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 3)) return;
  op_load_ctx_p(*vm.ctx, vm.r + vm.a);
  vm.pc += 1;
}
AR_NS_END
#define handle_LOAD_CTX_P() ::ar::vm_op_load_ctx_p(vm)
