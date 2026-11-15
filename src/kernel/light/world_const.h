// SPDX-License-Identifier: Apache-2.0
// The constant world: radiance Le = strength * s(lambda) from every direction, the compiled world
// program's CONSTANT_EMISSION value (or the constant magenta world). An infinite light: sample_li
// reports `infinite`, never a distance. Include light.h, not this file.
//
// Ref: docs/decisions/0004-integrator.md D6 (world row), D7 (infinite lights); pbrt-v4
// src/pbrt/lights.cpp UniformInfiniteLight::SampleLi (uniform sphere, pdf 1 / (4 pi)), PDF_Li,
// SampleLe (direction from the first pair, a disk of the scene radius facing it from the second,
// pdf_pos 1 / (pi R^2), pdf_dir 1 / (4 pi)) and Phi (4 pi^2 R^2 L) (Apache-2.0).
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not world_const.h"
#endif

AR_NS_BEGIN

AR_FN Real world_pdf() { return Real(1) / (Real(4) * kLightPi); }

AR_FN LightLiSample world_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                    const ObserverView& obs) {
  LightLiSample s = light_li_none();
  s.wi = sample_uniform_sphere(u);
  s.infinite = true;
  s.p_light = ctx.p;  // no point: the light is at infinity
  s.pdf = world_pdf();
  s.Le = light_emission(r, lam, obs);
  if (!is_finite(s.Le)) return light_li_none();
  return s;
}

AR_FN Real world_pdf_li(const LightRecord& /*r*/, const LightSampleContext& /*ctx*/, Real3 /*wi*/, const LightHit& /*hit*/) {
  return world_pdf();
}

// u_dir picks the direction w the light travels along, u_pos the point of the disk of radius R
// facing it; the ray starts R behind the disk so it crosses the whole scene sphere.
AR_FN LightLeSample world_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                    const ObserverView& obs, const SceneSphere& scene) {
  if (!(scene.radius > Real(0))) return light_le_none();
  const Real3 w = sample_uniform_sphere(u_dir);
  const Real2 cd = sample_disk_concentric(u_pos);
  const Real3 p_disk = scene.center + to_world(frame_from_normal(-w), Real3{cd.x, cd.y, Real(0)}) * scene.radius;
  LightLeSample s;
  s.ray.o = p_disk - w * scene.radius;
  s.ray.d = w;
  s.Le = light_emission(r, lam, obs);
  s.pdf_pos = Real(1) / (kLightPi * scene.radius * scene.radius);
  s.pdf_dir = world_pdf();
  if (!is_finite(s.Le) || !(s.pdf_pos - s.pdf_pos == Real(0))) return light_le_none();
  return s;
}

// pbrt-v4 UniformInfiniteLight::Phi: 4 pi^2 R^2 times the emission (strength or weighted energy).
AR_FN Real world_power(const SceneSphere& scene, Real energy) {
  return Real(4) * kLightPi * kLightPi * scene.radius * scene.radius * energy;
}

AR_NS_END
