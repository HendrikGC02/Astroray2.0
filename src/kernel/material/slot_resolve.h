// SPDX-License-Identifier: Apache-2.0
// Slot resolution: a SpectrumRef at the path's wavelengths, so the BSDF never sees RGB and never
// branches on the source type. make_rgb_ref is the host side of the RGB kind: it applies the
// slot's convention once and stores the sigmoid coefficients and scale in the ref.
// Ref: docs/decisions/0005-material-model.md items 9-10 (kinds, semantic per slot, REFLECTANCE
// clamp); color/rgb2spec.h (Jakob and Hanika 2019 conventions, 0003 d16); OpenPBR 1.1.2 dispersion
// section (Abbe number, Cauchy law); spectral/planck.h (0003 d25).
#pragma once
#include <cstdint>

#include "../color/observer.h"
#include "../color/rgb2spec.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"
#include "../spectral/planck.h"
#include "spectrum_ref.h"

AR_NS_BEGIN

AR_FN SpectrumRef make_const_ref(SpectrumSemantic sem, Real value) {
  SpectrumRef r;
  r.kind = uint8_t(SpectrumKind::CONSTANT);
  r.semantic = uint8_t(sem);
  r.aux = 0;
  r.index = 0;
  r.p[0] = float(value);
  r.p[1] = r.p[2] = r.p[3] = 0.0f;
  return r;
}

// RGB source for a slot. False for the IOR semantic, which accepts no RGB (0005 item 10: refused);
// *flags receives kRgbNegativeClamped / kRgbAboveOneClamped for the compiler to report.
AR_FN bool make_rgb_ref(const Rgb2SpecTable& t, Rgb rgb, SpectrumSemantic sem, SpectrumRef* out, uint32_t* flags) {
  if (sem == SpectrumSemantic::IOR) return false;
  const Upsampled u = (sem == SpectrumSemantic::REFLECTANCE) ? albedo_coeffs(t, rgb, flags) : unbounded_coeffs(t, rgb, flags);
  out->kind = uint8_t(SpectrumKind::RGB);
  out->semantic = uint8_t(sem);
  out->aux = 0;
  out->index = 0;
  out->p[0] = float(u.c.c[0]);
  out->p[1] = float(u.c.c[1]);
  out->p[2] = float(u.c.c[2]);
  out->p[3] = float(u.scale);
  return true;
}

// Blackbody emission: scale x Planck radiance per lane. The host stores scale = the node's scale
// times 1 / Y(Planck(T)), so the result's display Y is the node's scale (0003 d11).
// Ref: 0003 d25 (planck_radiance); 0005 item 9.
AR_FN Spec blackbody_spec(Real T, Real scale, const Lambda& lam) {
  Spec s;
  for (int i = 0; i < kLanes; ++i) s.v[i] = scale * planck_radiance(lam.nm[i], T);
  return s;
}

// Fraunhofer lines of the Abbe number (nm). Ref: OpenPBR 1.1.2 dispersion section (index.html,
// "Dispersion"): lambda_C = 656.3, lambda_d = 587.6, lambda_F = 486.1.
inline constexpr double kLambdaC = double(656.3), kLambdaD = double(587.6), kLambdaF = double(486.1);
inline constexpr double kCauchyLambdaMin = double(360.0), kCauchyLambdaMax = double(830.0);  // 0003 d8

// Cauchy n(l) = A + B / l^2 with B = s (n_d - 1) / (V_d (l_F^-2 - l_C^-2)) and A = n_d - B / l_d^2,
// written as n_d + B (l^-2 - l_d^-2) so the d line returns n_d exactly. The OpenPBR Abbe number of
// the medium is V_d / s for dispersion scale s, so s = 0 is flat n_d and s = 1 reproduces V_d.
// Templated like planck_radiance_t so the host test can run it in double. l is clamped to the
// observer range (0003 d8). Ref: OpenPBR 1.1.2 dispersion section, Eqs. for B and A.
template <class R>
AR_FN R cauchy_abbe_ior_t(R nd, R Vd, R dispersion_scale, R lambda_nm) {
  const R l = lambda_nm < R(kCauchyLambdaMin) ? R(kCauchyLambdaMin)
                                              : (lambda_nm > R(kCauchyLambdaMax) ? R(kCauchyLambdaMax) : lambda_nm);
  const R inv_f = R(1) / (R(kLambdaF) * R(kLambdaF)), inv_c = R(1) / (R(kLambdaC) * R(kLambdaC));
  const R inv_d = R(1) / (R(kLambdaD) * R(kLambdaD));
  const R B = dispersion_scale * (nd - R(1)) / (Vd * (inv_f - inv_c));
  return nd + B * (R(1) / (l * l) - inv_d);
}

AR_FN Spec cauchy_abbe_ior(Real nd, Real Vd, Real dispersion_scale, const Lambda& lam) {
  Spec s;
  for (int i = 0; i < kLanes; ++i) s.v[i] = cauchy_abbe_ior_t<Real>(nd, Vd, dispersion_scale, lam.nm[i]);
  return s;
}

// CONSTANT, RGB, BLACKBODY (ILLUMINANT slots only) and CAUCHY_ABBE (IOR slots only) resolve; the
// compiler rejects every other kind and every other (kind, semantic) pair (0005 item 9), so
// reaching one here is a bug: it asserts in debug builds and gives zero. A BLACKBODY ref is
// analytic (0005 item 39), so it skips the illuminant multiply an ILLUMINANT slot applies.
AR_FN Spec resolve(const SpectrumRef& r, const Lambda& lam, const ObserverView& obs) {
  const SpectrumSemantic sem = SpectrumSemantic(r.semantic);
  Spec s;
  if (r.kind == uint8_t(SpectrumKind::CONSTANT)) {
    Real v = Real(r.p[0]);
    if (sem == SpectrumSemantic::REFLECTANCE) v = !(v >= Real(0)) ? Real(0) : (v > Real(1) ? Real(1) : v);
    s = spec_fill(v);
  } else if (r.kind == uint8_t(SpectrumKind::RGB)) {
    Upsampled u;
    u.c.c[0] = Real(r.p[0]);
    u.c.c[1] = Real(r.p[1]);
    u.c.c[2] = Real(r.p[2]);
    u.scale = Real(r.p[3]);
    s = upsampled_eval(u, lam);
  } else if (r.kind == uint8_t(SpectrumKind::BLACKBODY) && sem == SpectrumSemantic::ILLUMINANT) {
    return blackbody_spec(Real(r.p[0]), Real(r.p[1]), lam);
  } else if (r.kind == uint8_t(SpectrumKind::CAUCHY_ABBE) && sem == SpectrumSemantic::IOR) {
    return cauchy_abbe_ior(Real(r.p[0]), Real(r.p[1]), Real(r.p[2]), lam);
  } else {
    AR_ASSERT(false);
    return spec_fill(Real(0));
  }
  if (sem == SpectrumSemantic::ILLUMINANT)
    for (int i = 0; i < kLanes; ++i) s.v[i] = s.v[i] * illuminant_at(obs, lam.nm[i]);
  return s;
}

AR_NS_END
