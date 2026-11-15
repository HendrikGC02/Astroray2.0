// SPDX-License-Identifier: Apache-2.0
// Slot resolution: a SpectrumRef at the path's wavelengths, so the BSDF never sees RGB and never
// branches on the source type. make_rgb_ref is the host side of the RGB kind: it applies the
// slot's convention once and stores the sigmoid coefficients and scale in the ref.
// Ref: docs/decisions/0005-material-model.md items 9-10 (kinds, semantic per slot, REFLECTANCE
// clamp); color/rgb2spec.h (Jakob and Hanika 2019 conventions, 0003 d16).
#pragma once
#include <cstdint>

#include "../color/observer.h"
#include "../color/rgb2spec.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"
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

// Only CONSTANT and RGB resolve in M1a; the compiler rejects every other kind (059), so reaching
// one here is a bug: it asserts in debug builds and gives zero.
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
  } else {
    AR_ASSERT(false);
    return spec_fill(Real(0));
  }
  if (sem == SpectrumSemantic::ILLUMINANT)
    for (int i = 0; i < kLanes; ++i) s.v[i] = s.v[i] * illuminant_at(obs, lam.nm[i]);
  return s;
}

AR_NS_END
