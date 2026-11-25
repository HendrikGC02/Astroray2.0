// SPDX-License-Identifier: Apache-2.0
// Metal lobe of OpenPBR: F82-tint conductor reflection on GGX with height-correlated
// Smith masking and visible-normal sampling. Directions are in the local shading frame
// (z = normal); f excludes the cosine. Isotropic only; no thin film yet. eval and sample are
// the single-scattering lobe (tools/etables integrates them for E_spec); metal_compensation
// gives the multiple-scattering factor the lobe stack applies. An exact conductor Fresnel for
// (n, k) exists at the lobe level (MetalParams::conductor); the stack keeps the F82 mode until
// metal_ior reaches the record (0005 item 12).
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
  // Exact conductor mode (0005 item 12, metal_ior != 0): F is fresnel_conductor at the complex
  // index n + i k per lane, base_color, base_weight and specular_color are not read. n and k are
  // read only when conductor is set.
  bool conductor = false;
  Spec n, k;
};

// Unpolarised reflectance of a conductor of complex relative index eta = n + i k for light from
// the dielectric side at cos_i, the mean of |r_parl|^2 and |r_perp|^2. A template on the scalar
// type only, so the tests can run a double instance beside the float one (0002 D8).
// Ref: pbrt-v4 src/pbrt/util/scattering.h FrComplex (lines 81-92) and the complex sqrt, norm and
// division of src/pbrt/util/pstd.h (lines 1080-1083, 1207-1229), third_party/_cache/refs/
// pbrt-v4-092b23a47ad4 (Apache-2.0); Born and Wolf, Principles of Optics, 13.2 (0005 item 12).
template <typename T>
struct MetalComplex {
  T re, im;
};
template <typename T>
AR_FN MetalComplex<T> metal_cmul(MetalComplex<T> a, MetalComplex<T> b) {
  return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
template <typename T>
AR_FN MetalComplex<T> metal_cdiv(MetalComplex<T> a, MetalComplex<T> b) {
  const T scale = T(1) / (b.re * b.re + b.im * b.im);
  return {scale * (a.re * b.re + a.im * b.im), scale * (a.im * b.re - a.re * b.im)};
}
// Principal square root, as pstd::sqrt.
template <typename T>
AR_FN MetalComplex<T> metal_csqrt(MetalComplex<T> z) {
  const T n = ar::sqrt(z.re * z.re + z.im * z.im);
  if (n == T(0)) return {T(0), T(0)};
  // Halving is exact in binary floating point, so / T(2) is pstd's T(.5) * x.
  const T t1 = ar::sqrt((n + ar::abs(z.re)) / T(2));
  const T t2 = z.im / T(2) / t1;
  if (z.re >= T(0)) return {t1, t2};
  return {ar::abs(t2), z.im < T(0) ? -t1 : t1};
}
template <typename T>
AR_FN T fresnel_conductor_scalar(T cos_i, T n, T k) {
  cos_i = cos_i < T(0) ? T(0) : (cos_i > T(1) ? T(1) : cos_i);
  const MetalComplex<T> eta{n, k};
  const T sin2_i = T(1) - cos_i * cos_i;
  const MetalComplex<T> sin2_t = metal_cdiv(MetalComplex<T>{sin2_i, T(0)}, metal_cmul(eta, eta));
  const MetalComplex<T> cos_t = metal_csqrt(MetalComplex<T>{T(1) - sin2_t.re, -sin2_t.im});
  const MetalComplex<T> ec{eta.re * cos_i, eta.im * cos_i};
  const MetalComplex<T> r_parl = metal_cdiv(MetalComplex<T>{ec.re - cos_t.re, ec.im - cos_t.im},
                                            MetalComplex<T>{ec.re + cos_t.re, ec.im + cos_t.im});
  const MetalComplex<T> et = metal_cmul(eta, cos_t);
  const MetalComplex<T> r_perp = metal_cdiv(MetalComplex<T>{cos_i - et.re, -et.im}, MetalComplex<T>{cos_i + et.re, et.im});
  return (r_parl.re * r_parl.re + r_parl.im * r_parl.im + r_perp.re * r_perp.re + r_perp.im * r_perp.im) / T(2);
}
// Per transport wavelength (0005 item 7).
AR_FN Spec fresnel_conductor(Real cos_i, const Spec& n, const Spec& k) {
  Spec f;
  for (int i = 0; i < kLanes; ++i) f.v[i] = fresnel_conductor_scalar<Real>(cos_i, n.v[i], k.v[i]);
  return f;
}

// F_metal(cos) = specular_weight * F_82(base_weight * base_color, specular_color, cos) per lane, or
// specular_weight * F_conductor(cos, n, k) in the exact mode (the weight suppresses the lobe in
// both modes, index.html line 560).
AR_FN Spec metal_fresnel(const MetalParams& p, Real cos_i) {
  Spec f;
  if (p.conductor) {
    f = fresnel_conductor(cos_i, p.n, p.k);
  } else {
    Spec f0;
    for (int i = 0; i < kLanes; ++i) f0.v[i] = p.base_weight * p.base_color.v[i];
    f = fresnel_f82(f0, p.specular_color, cos_i);
  }
  for (int i = 0; i < kLanes; ++i) f.v[i] *= p.specular_weight;
  return f;
}

// Multiple-scattering compensation per lane, the factor that multiplies the single-scattering
// lobe: 1 + F0 (1 - E) / E, where E = E_spec(mu_o, r) is the lobe's directional albedo at F = 1
// (the table tools/etables builds from this file) and F0 = F_metal(1) is the lobe's own
// normal-incidence Fresnel per lane. At F = 1 it is 1 / E, so the compensated lobe reflects
// everything; it depends on wo only, so the lobe is not reciprocal, as the method accepts.
// Ref: Turquin 2019, "Practical multiple scattering compensation for microfacet models" (ILM
// white paper; cited in OpenPBR index.html line 428 and reference [Turquin2019] line 1626), the
// 1 / E scaling with the F0-weighted missing term of 0005 item 5; the same 1 + (1 - E) / E
// energy scale is Cycles src/kernel/closure/bsdf_microfacet.h lines 583-584
// (third_party/_cache/refs/cycles-ed69ef2c6bd3, Apache-2.0). e_single <= 0 cannot come from the
// table (every cell is positive); it returns the uncompensated lobe.
AR_FN Spec metal_compensation(const MetalParams& p, Real e_single) {
  const Spec f0 = metal_fresnel(p, Real(1));
  Spec c;
  const Real missing = e_single > Real(0) ? (Real(1) - e_single) / e_single : Real(0);
  for (int i = 0; i < kLanes; ++i) c.v[i] = Real(1) + f0.v[i] * missing;
  return c;
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
