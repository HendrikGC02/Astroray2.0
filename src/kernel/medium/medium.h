// SPDX-License-Identifier: Apache-2.0
// The coefficient-level medium sample and the per-medium scene record.
// MediumSample is what a medium's VM volume program produces at one point, at the path's kLanes
// wavelengths in the medium's rest frame; MediumDesc holds no material parameters, only which
// program, grids and estimator a medium uses. A position is a Real3 in metres.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D1-D4 (field list, units, the 40 B layout,
// flag and estimator names), D9 (estimators), D12 (closed PhaseKind), D39 (double CPU build).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "portability.h"

AR_NS_BEGIN

// Closed list of phase functions (0006 D12). A kind is added together with its eval, sample, pdf,
// normalisation and chi-squared tests, never ahead of them.
enum class PhaseKind : uint8_t { HG = 0 };

// One phase component. p0, p1 are the kind's parameters (HG: p0 = g, p1 unused); weight is that
// component's sigma_s in 1/m, so two lobes mix in proportion to their scattering (0006 D2).
struct PhaseLobe {
  uint8_t kind;  // a PhaseKind
  float p0, p1;
  Spec weight;
};

// 0006 D2. sigma_a, sigma_s in 1/m (D4: scene units times metres-per-unit); emission is the
// emission coefficient j in W m^-3 sr^-1 nm^-1, radiance added per metre, not sigma_a * L_e;
// velocity in m/s relative to the static frame.
struct MediumSample {
  Spec sigma_a, sigma_s, emission;
  PhaseLobe phase[2];
  Real3 velocity;
};

// MediumDesc::model (0006 D3). NEBULAR arrives with its model.
enum class MediumModel : uint8_t { BLENDER_VOLUME = 0 };

// MediumDesc::estimator, derived at scene build (0006 D9): the deterministic emission-absorption
// marcher for pieces without scattering, null-scattering delta tracking otherwise.
enum class MediumEstimator : uint8_t { MARCH_EMISSION_ABSORPTION = 0, DELTA_TRACK = 1 };

// MediumDesc::flags bits (0006 D3).
inline constexpr uint16_t MEDIUM_HAS_EMISSION = 1u << 0;
inline constexpr uint16_t MEDIUM_HAS_SCATTERING = 1u << 1;
inline constexpr uint16_t MEDIUM_HAS_VELOCITY = 1u << 2;
inline constexpr uint16_t MEDIUM_HAS_FLUORESCENCE = 1u << 3;

// 0006 D3: one per medium in the scene table. Fixed-width fields only, so the layout is the same
// in the float and the double build. Grid handles are operands in the program's constant block.
struct MediumDesc {
  uint8_t model;      // a MediumModel
  uint8_t estimator;  // a MediumEstimator
  uint16_t flags;     // MEDIUM_HAS_* bits
  uint32_t program;
  uint32_t majorant_grid;
  uint32_t nebular_record;
  float bounds[6];
};

static_assert(sizeof(MediumDesc) == 40, "MediumDesc is 40 B (0006 D3)");

// No absorption, scattering, emission or motion; both lobes are HG with g = 0 and weight 0.
AR_FN MediumSample medium_sample_zero() {
  MediumSample s;
  s.sigma_a = spec_fill(Real(0));
  s.sigma_s = spec_fill(Real(0));
  s.emission = spec_fill(Real(0));
  for (int k = 0; k < 2; ++k) {
    s.phase[k].kind = uint8_t(PhaseKind::HG);
    s.phase[k].p0 = 0.0f;
    s.phase[k].p1 = 0.0f;
    s.phase[k].weight = spec_fill(Real(0));
  }
  s.velocity = Real3{Real(0), Real(0), Real(0)};
  return s;
}

// Extinction sigma_t = sigma_a + sigma_s, per lane, in 1/m.
AR_FN Spec medium_sigma_t(const MediumSample& s) { return s.sigma_a + s.sigma_s; }

// Beer-Lambert transmittance exp(-sigma_t d) per lane over a distance d in metres, for a medium
// that is constant along the segment (0006 V1).
AR_FN Spec medium_transmittance(const MediumSample& s, Real dist) {
  const Spec st = medium_sigma_t(s);
  Spec t;
  for (int i = 0; i < kLanes; ++i) t.v[i] = ar::exp(-st.v[i] * dist);
  return t;
}

AR_NS_END
