// SPDX-License-Identifier: Apache-2.0
// VOLUME_COEFFS: the volume entry's one sink. It fills the MediumSample from the medium's
// constant block: sigma_a, one sigma_s and one HG phase lobe per scattering term, emission,
// velocity 0. It reads no register, so over any box its coefficients are the block's own
// (interval_volume_coeffs). Only a volume entry runs it: with no medium sample or block (a
// surface run) it stops with NULL_POINTER.
//
// volume_term_ref is the host side, the one place a Blender volume node's mapping lives
// (0006 A.1, D21), called by the compiler and the tests. Cycles svm_node_closure_volume:
//   Volume Scatter    weight = colour * density      (the colour is the closure weight)
//   Volume Absorption weight = (1 - colour) * density
// with density clamped to >= 0. The colour is upsampled as a reflectance first (0006 D21), never
// the product. The complement uses the sigmoid's symmetry, 1 - S(x) = S(-x) (sigmoid_s), so the
// absorption of colour c is the reflectance ref of c with its three coefficients negated: still
// one RGB ref, and the complement stays exact at a grey 0 or 1 (c2 = -+inf swaps).
// Ref: docs/decisions/0006-volumes-and-astrophysics.md A.1, A.2, D21; cycles-ed69ef2c6bd3
// src/kernel/svm/closure.h svm_node_closure_volume (Apache-2.0, read, not copied); Jakob and
// Hanika 2019 through color/rgb2spec.h.
#pragma once
#include "../../color/rgb2spec.h"
#include "../../material/slot_resolve.h"
#include "../../medium/medium.h"
#include "../../medium/medium_block.h"
#include "common.h"

AR_NS_BEGIN

enum class VolumeTerm : uint8_t { ABSORPTION = 0, SCATTER = 1 };

// The ref of one term's coefficient in 1/m: scale = density, reflectance sigmoid of the colour
// (negated for absorption). *flags receives kRgbNegativeClamped / kRgbAboveOneClamped.
AR_FN SpectrumRef volume_term_ref(const Rgb2SpecTable& t, Rgb colour, float density, VolumeTerm kind, uint32_t* flags) {
  const Upsampled u = albedo_coeffs(t, colour, flags);
  const float sign = kind == VolumeTerm::ABSORPTION ? -1.0f : 1.0f;
  SpectrumRef r;
  r.kind = uint8_t(SpectrumKind::RGB);
  r.semantic = uint8_t(SpectrumSemantic::REFLECTANCE);
  r.aux = 0;
  r.index = 0;
  r.p[0] = sign * float(u.c.c[0]);
  r.p[1] = sign * float(u.c.c[1]);
  r.p[2] = sign * float(u.c.c[2]);
  r.p[3] = density > 0.0f ? density : 0.0f;  // fmaxf(density, 0) of Cycles; NaN gives 0
  return r;
}

// Fills *out from the block at the path's wavelengths. BAD_OPERAND for a lobe count past
// kMediumMaxLobes or a ref kind this build cannot resolve.
AR_FN VmStatus op_volume_coeffs(const MediumBlock& b, const ObserverView& obs, const Lambda& lam, MediumSample* out) {
  if (b.lobe_count > kMediumMaxLobes || !medium_ref_ok(b.sigma_a) || !medium_ref_ok(b.emission)) return VmStatus::BAD_OPERAND;
  *out = medium_sample_zero();
  out->sigma_a = resolve(b.sigma_a, lam, obs);
  for (uint32_t k = 0; k < b.lobe_count; ++k) {
    if (!medium_ref_ok(b.sigma_s[k])) return VmStatus::BAD_OPERAND;
    const Spec w = resolve(b.sigma_s[k], lam, obs);
    out->phase[k].kind = uint8_t(PhaseKind::HG);
    out->phase[k].p0 = b.g[k];
    out->phase[k].p1 = 0.0f;
    out->phase[k].weight = w;
    out->sigma_s = out->sigma_s + w;
  }
  out->emission = resolve(b.emission, lam, obs);
  return VmStatus::OK;
}

AR_FN void vm_op_volume_coeffs(VmState& vm) {
  if (vm.med == nullptr || vm.mblock == nullptr || vm.tables == nullptr) return vm_fail(vm, VmStatus::NULL_POINTER);
  const VmStatus s = op_volume_coeffs(*vm.mblock, vm.tables->observer, vm.ctx->lambda_t, vm.med);
  if (s != VmStatus::OK) return vm_fail(vm, s);
  vm.pc += 1;
}
AR_NS_END
#define handle_VOLUME_COEFFS() ::ar::vm_op_volume_coeffs(vm)
