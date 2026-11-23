// SPDX-License-Identifier: Apache-2.0
// CAUCHY_ABBE a, imm n_d, imm V_d, imm dispersion scale: r[a..a+3] = the IOR at the path's
// wavelengths, the same function resolve() calls for a CAUCHY_ABBE SpectrumRef.
// Ref: docs/decisions/0005-material-model.md items 9, 31; OpenPBR 1.1.2 dispersion section.
#pragma once
#include "../../material/slot_resolve.h"
#include "common.h"

AR_NS_BEGIN
AR_FN void vm_op_cauchy_abbe(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 4)) return;
  const Spec s = cauchy_abbe_ior(Real(bit_cast<float>(vm_imm(vm, 0))), Real(bit_cast<float>(vm_imm(vm, 1))),
                                 Real(bit_cast<float>(vm_imm(vm, 2))), vm.ctx->lambda_t);
  for (int i = 0; i < kLanes; ++i) vm.r[vm.a + uint32_t(i)] = float(s.v[i]);
  vm.pc += 4;
}
AR_NS_END
#define handle_CAUCHY_ABBE() ::ar::vm_op_cauchy_abbe(vm)
