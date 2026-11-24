// SPDX-License-Identifier: Apache-2.0
// MediumBlock: the constant block of one medium, the medium counterpart of ConstantBlock. Every
// coefficient is a SpectrumRef with its density already folded in (0006 A.1), so a medium
// evaluation costs a sigmoid per lane and no table lookup. Coefficients are in 1/m.
//
// Layout (designed here, 0006 A.1): sigma_a is one ref; scattering is up to two lobes, each a
// SpectrumRef sigma_s[k] that is also that lobe's phase weight, with its Henyey-Greenstein g
// beside it. A sum of two sigma_s spectra is not one ref, which is why the second lobe has its own
// ref instead of a mix fraction. emission is the coefficient j; it holds a CONSTANT 0 until a
// medium emits.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md A.1, A.2, D2, D7, D12;
// material/spectrum_ref.h (SpectrumRef, 0005 item 9).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../material/slot_resolve.h"
#include "../material/spectrum_ref.h"
#include "../portability.h"
#include "../vm/interval.h"

AR_NS_BEGIN

inline constexpr uint32_t kMediumMaxLobes = 2;

struct MediumBlock {
  SpectrumRef sigma_a;
  SpectrumRef sigma_s[kMediumMaxLobes];
  SpectrumRef emission;
  float g[kMediumMaxLobes];  // stored within +-kHgMaxAbsG; the compiler reports a clamp
  uint32_t lobe_count;       // scattering lobes in use, 0 to kMediumMaxLobes
  uint32_t entry;            // word offset of the volume entry in the medium program array; 0 = none
};

static_assert(sizeof(MediumBlock) == 112, "MediumBlock: 4 SpectrumRef, 2 f32, 2 u32");

// No absorption, scattering or emission, no entry: vacuum.
AR_FN MediumBlock medium_block_vacuum() {
  MediumBlock b = {};
  b.sigma_a = make_const_ref(SpectrumSemantic::UNBOUNDED, Real(0));
  for (uint32_t k = 0; k < kMediumMaxLobes; ++k) b.sigma_s[k] = make_const_ref(SpectrumSemantic::UNBOUNDED, Real(0));
  b.emission = make_const_ref(SpectrumSemantic::UNBOUNDED, Real(0));
  return b;
}

// The (kind, semantic) pairs a medium ref may hold: an unclamped CONSTANT and an RGB reflectance
// sigmoid, exactly what medium_ref_bound covers. resolve() would clamp a CONSTANT REFLECTANCE to 1
// and multiply an ILLUMINANT by the observer's illuminant, both outside that bound, so any other
// pair stops the run with BAD_OPERAND instead of being resolved.
AR_FN bool medium_ref_ok(const SpectrumRef& r) {
  return (r.kind == uint8_t(SpectrumKind::CONSTANT) && r.semantic == uint8_t(SpectrumSemantic::UNBOUNDED)) ||
         (r.kind == uint8_t(SpectrumKind::RGB) && r.semantic == uint8_t(SpectrumSemantic::REFLECTANCE));
}

// A bound on one coefficient ref over every wavelength; a pair medium_ref_ok refuses has none.
// A CONSTANT is itself. An RGB ref is scale * sigmoid with the sigmoid in [0, 1] mathematically;
// the float evaluation can leave that range by a few ulp near its ends, so the bound has 2^-22 of
// the scale of slack on each side. A refused pair, a negative scale or a NaN gives {NaN, NaN}: the
// bound is unsupported, never clamped.
AR_FN Interval medium_ref_bound(const SpectrumRef& r) {
  if (!medium_ref_ok(r)) return interval_nan();
  if (r.kind == uint8_t(SpectrumKind::CONSTANT)) return interval_exact(Interval{r.p[0], r.p[0]});
  if (r.p[3] >= 0.0f) {
    const float slack = r.p[3] * 2.384185791015625e-7f;
    return Interval{-slack, interval_step_up(r.p[3] + slack)};
  }
  return interval_nan();
}

// Extinction sigma_t = sigma_a + the lobes' sigma_s over every wavelength, from the refs alone:
// the interval the majorant builder takes from VOLUME_COEFFS (interval_volume_coeffs).
AR_FN Interval medium_block_sigma_t_bound(const MediumBlock& b) {
  Interval s = medium_ref_bound(b.sigma_a);
  for (uint32_t k = 0; k < b.lobe_count && k < kMediumMaxLobes; ++k) s = interval_add_scalar(s, medium_ref_bound(b.sigma_s[k]));
  return s;
}

AR_NS_END
