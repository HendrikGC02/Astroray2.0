// SPDX-License-Identifier: Apache-2.0
// BLACKBODY a, imm T, imm scale: r[a..a+3] = scale x Planck radiance at the path's wavelengths.
// The same function resolve() calls for a BLACKBODY SpectrumRef; no illuminant factor (analytic,
// 0005 item 39). The scale immediate already holds the host's 1 / Y(Planck(T)) when the program
// is meant to be display-normalised. The compiler stores the lone constant Blackbody emission in
// the block instead, so this op exists for per-path programs and the agreement tests.
// Ref: docs/decisions/0005-material-model.md items 9, 31, 36; 0003 d25.
#pragma once
#include "../../material/slot_resolve.h"
#include "common.h"

AR_NS_BEGIN
AR_FN void vm_op_blackbody(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 4)) return;
  const Spec s = blackbody_spec(Real(bit_cast<float>(vm_imm(vm, 0))), Real(bit_cast<float>(vm_imm(vm, 1))),
                                vm.ctx->lambda_t);
  for (int i = 0; i < kLanes; ++i) vm.r[vm.a + uint32_t(i)] = float(s.v[i]);
  vm.pc += 3;
}
AR_NS_END
#define handle_BLACKBODY() ::ar::vm_op_blackbody(vm)
