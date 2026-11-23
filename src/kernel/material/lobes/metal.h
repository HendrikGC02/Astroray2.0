// SPDX-License-Identifier: Apache-2.0
// Metal lobe of OpenPBR: F82-tint conductor reflection on GGX with height-correlated
// Smith masking and visible-normal sampling. Directions are in the local shading frame
// (z = normal); f excludes the cosine. Isotropic only; no energy compensation, no
// complex-IOR mode (metal_ior = 0, the F82 mode of 0005 item 12) and no thin film yet.
//
// Ref: OpenPBR Surface 1.1.2, third_party/_cache/refs/openpbr-781573a80cb2/index.html
// lines 545-563 (F82-tint, Kutz, Hasan, Edmondson 2021 extending Hoffman 2019; quoted at
// fresnel_f82 in fresnel.h) and line 560: F_metal(mu) = specular_weight * F_82(mu), where
// F_0 = base_weight * base_color (line 546) and specular_color is the tint at mu_bar.
// The reference implementation wires the same inputs,
// third_party/_cache/refs/openpbr-781573a80cb2/reference/open_pbr_surface.mtlx lines 407-418:
//   <multiply name="metal_reflectivity"> in1 = base_color, in2 = base_weight
//   <generalized_schlick_bsdf name="metal_bsdf"> weight = specular_weight,
//     color0 = metal_reflectivity, color82 = specular_color, roughness = main_roughness
// The spec text applies no clamp to F_82; fresnel_f82 saturates each lane to [0, 1] as
// openpbr-bsdf does (reasons there). Microfacet parts (D, G1, G2, VNDF, alpha =
// roughness^2, the reflection pdf) are ggx.h's, so there is one GGX (Walter 2007, Heitz
// 2014, Heitz 2018). Spectral semantics: every term per transport wavelength (0005 item 7).
// Cross-check: Adobe openpbr-bsdf (Apache-2.0).
//
// Filling MetalParams from OpenPBRParams (and mixing by base_metalness) is the lobe
// stack's job; this file is a pure function set that holds no state between calls.
//
// Contract (0005 item 8): wo and wi must lie above the shading plane (z > 0); at
// z <= 0 the lobe returns f = 0, pdf = 0 ("no sample"; the caller terminates).
// Reflection has no eta^2 term and the adjoint shading-normal correction belongs to the
// caller, so TransportMode does not change values.
//
// Delta convention (as ggx.h): below lobe_is_delta's threshold metal_sample returns the
// mirror direction with LOBE_REFLECTION | LOBE_DELTA, pdf = 1 and f = F(wo.z) / wo.z, so
// f cos / pdf = F; eval and pdf return 0 for a delta lobe (0004 D19).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "material/lobes/delta.h"
#include "material/lobes/fresnel.h"
#include "material/lobes/ggx.h"
#include "material/lobes/lobe.h"
#include "portability.h"

AR_NS_BEGIN

struct MetalParams {
  Spec base_color;      // F_0 = base_weight * base_color per lane
  Spec specular_color;  // F82 edge tint: the fraction of Schlick reached at mu_bar
  Real base_weight;
  Real specular_weight;  // multiplies F_82 (index.html line 560)
  Real roughness;        // specular_roughness in [0, 1]
};

// F_metal(cos) = specular_weight * F_82(base_weight * base_color, specular_color, cos) per lane.
AR_FN Spec metal_fresnel(const MetalParams& p, Real cos_i) {
  Spec f0;
  for (int i = 0; i < kLanes; ++i) f0.v[i] = p.base_weight * p.base_color.v[i];
  Spec f = fresnel_f82(f0, p.specular_color, cos_i);
  for (int i = 0; i < kLanes; ++i) f.v[i] *= p.specular_weight;
  return f;
}

AR_FN Real metal_pdf(const MetalParams& p, Real3 wo, Real3 wi, TransportMode mode) {
  (void)mode;
  const Real alpha = ggx_alpha_of(p.roughness);
  if (lobe_is_delta(alpha, alpha) || wo.z <= Real(0) || wi.z <= Real(0)) return Real(0);
  Real3 m{};
  if (!ggx_half_vector(wo, wi, m)) return Real(0);
  const Real pdf = ggx_reflect_pdf(alpha, wo, m);
  return pdf > Real(0) ? pdf : Real(0);  // also maps NaN to "no density"
}

AR_FN LobeEval metal_eval(const MetalParams& p, Real3 wo, Real3 wi, TransportMode mode) {
  const Real alpha = ggx_alpha_of(p.roughness);
  if (lobe_is_delta(alpha, alpha) || wo.z <= Real(0) || wi.z <= Real(0)) return ggx_zero_eval();
  Real3 m{};
  if (!ggx_half_vector(wo, wi, m)) return ggx_zero_eval();
  const Real scale = ggx_reflect_dg(alpha, wo, wi, m);
  const Spec fr = metal_fresnel(p, dot(wo, m));
  LobeEval e;
  for (int i = 0; i < kLanes; ++i) e.f.v[i] = fr.v[i] * scale;
  e.pdf = metal_pdf(p, wo, wi, mode);
  if (!(e.pdf > Real(0)) || !is_finite(e.f)) return ggx_zero_eval();
  return e;
}

// u in [0, 1).
AR_FN LobeSample metal_sample(const MetalParams& p, Real3 wo, Real2 u, TransportMode mode) {
  (void)mode;
  LobeSample s;
  s.wi = Real3{Real(0), Real(0), Real(1)};
  s.f = spec_fill(Real(0));
  s.pdf = Real(0);
  s.flags = LOBE_REFLECTION;
  if (wo.z <= Real(0)) return s;
  const Real alpha = ggx_alpha_of(p.roughness);

  if (lobe_is_delta(alpha, alpha)) {
    // Mirror direction; F at the unperturbed normal.
    s.wi = Real3{-wo.x, -wo.y, wo.z};
    const Spec fr = metal_fresnel(p, wo.z);
    for (int i = 0; i < kLanes; ++i) s.f.v[i] = fr.v[i] / wo.z;
    s.pdf = Real(1);
    s.flags = LOBE_REFLECTION | LOBE_DELTA;
    return s;
  }

  const Real3 m = ggx_sample_vndf(alpha, wo, u);
  const Real cos_o_m = dot(wo, m);
  const Real3 wi = m * (Real(2) * cos_o_m) - wo;
  // A reflected direction below the surface is "no sample" (0005 item 8).
  if (!(m.z > Real(0)) || !(cos_o_m > Real(0)) || !(wi.z > Real(0))) return s;
  const Real pdf = ggx_reflect_pdf(alpha, wo, m);
  const Real scale = ggx_reflect_dg(alpha, wo, wi, m);
  const Spec fr = metal_fresnel(p, cos_o_m);
  Spec f;
  for (int i = 0; i < kLanes; ++i) f.v[i] = fr.v[i] * scale;
  if (!(pdf > Real(0)) || !is_finite(f)) return s;
  s.wi = wi;
  s.pdf = pdf;
  s.f = f;
  return s;
}

AR_NS_END
