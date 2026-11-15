// SPDX-License-Identifier: Apache-2.0
// RGB_LIFT a, b, c: r[a..a+3] = the spectrum of the RGB colour r[b..b+2] at the path's
// wavelengths, lifted with semantic c (SpectrumSemantic: 0 REFLECTANCE, 1 UNBOUNDED,
// 2 ILLUMINANT). The semantic is the c byte because the op names no third register; IOR takes
// no RGB source (0005 item 10) and is a bad operand. Out-of-range components follow the lift's
// own clamp; the compiler reports clamps of constants, a run-time value has no report channel.
// Ref: docs/decisions/0005-material-model.md items 10, 11, 31; color/rgb2spec.h (Jakob and
// Hanika 2019, pbrt-v4 RGBAlbedoSpectrum, RGBUnboundedSpectrum, RGBIlluminantSpectrum).
#pragma once
#include "../../material/spectrum_ref.h"
#include "common.h"

AR_NS_BEGIN
// REFLECTANCE, UNBOUNDED or ILLUMINANT only; the caller has checked sem.
AR_FN Spec op_rgb_lift(const SceneTables& t, const float* rgb, SpectrumSemantic sem, const Lambda& lam) {
  uint32_t flags = 0;
  const Rgb c{{Real(rgb[0]), Real(rgb[1]), Real(rgb[2])}};
  if (sem == SpectrumSemantic::REFLECTANCE) return albedo(*t.rgb2spec, c, lam, &flags);
  if (sem == SpectrumSemantic::UNBOUNDED) return unbounded(*t.rgb2spec, c, lam, &flags);
  return illuminant(*t.rgb2spec, t.observer, c, lam, &flags);
}

AR_FN bool vm_lift_semantic_ok(uint32_t c) { return c <= uint32_t(SpectrumSemantic::ILLUMINANT); }

AR_FN void vm_op_rgb_lift(VmState& vm) {
  if (!vm_regs_ok(vm, vm.a, 4) || !vm_regs_ok(vm, vm.b, 3)) return;
  if (!vm_lift_semantic_ok(vm.c)) return vm_fail(vm, VmStatus::BAD_OPERAND);
  if (vm.tables == nullptr || vm.tables->rgb2spec == nullptr) return vm_fail(vm, VmStatus::NULL_POINTER);
  const Spec s = op_rgb_lift(*vm.tables, vm.r + vm.b, SpectrumSemantic(vm.c), vm.ctx->lambda_t);
  for (int i = 0; i < kLanes; ++i) vm.r[vm.a + uint32_t(i)] = float(s.v[i]);
  vm.pc += 1;
}
AR_NS_END
#define handle_RGB_LIFT() ::ar::vm_op_rgb_lift(vm)
