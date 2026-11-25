// SPDX-License-Identifier: Apache-2.0
// The environment world (0004 D11): a world whose radiance is a program (an HDRI, a gradient, any
// non-constant Background) and whose importance table the host baked (host/light/env_bake.h).
// NEE draws directions from the MIS-compensated table (allow_incomplete: BSDF sampling already
// covers the dim directions), and the pdf a BSDF ray that escaped is weighted with is that same
// compensated pdf, so both strategies of the balance heuristic read one density. The density over
// solid angle is the table's density over the equal-area square divided by 4 pi
// (environment_table.h). Radiance at a sampled direction is never read from the table: sample_li
// returns deferred_le and the NEE stage runs the world program toward the direction, through the
// same evaluation an escaped ray gets (stages/shade_background.h world_program_radiance). Light
// paths (sample_le) are M4. Include light.h, not this file.
//
// Ref: third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/lights.h ImageInfiniteLight::SampleLi
// (compensatedDistribution when allowIncompletePDF, pdf mapPDF / (4 pi), no sample when mapPDF is
// 0) and src/pbrt/lights.cpp ImageInfiniteLight::PDF_Li and ImageInfiniteLight::Phi
// (4 pi^2 R^2 times the mean radiance) (Apache-2.0); Karlik, Sik, Vevoda, Skrivan, Krivanek 2019,
// "MIS compensation: optimizing sampling techniques in multiple importance sampling", ACM TOG
// 38(6); docs/decisions/0004-integrator.md D7, D10, D11.
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not world_env.h"
#endif

#include "environment_table.h"

AR_NS_BEGIN

// The two distributions of one baked world, as KernelGlobals holds them: [0] plain (light paths),
// [1] MIS-compensated (NEE and the escaped-ray weight).
inline constexpr int kEnvPlain = 0, kEnvCompensated = 1;

AR_FN LightLiSample world_env_sample_li(const LightSampleContext& ctx, Real2 u, const EnvTableView* env) {
  AR_ASSERT(env != nullptr);
  const EnvDirSample d = env_sample(env[kEnvPlain], env[kEnvCompensated], u, true);
  if (!(d.pdf > Real(0))) return light_li_none();
  LightLiSample s = light_li_none();
  s.wi = d.wi;
  s.infinite = true;
  s.p_light = ctx.p;  // no point: the light is at infinity
  s.pdf = d.pdf;
  s.deferred_le = true;
  return s;
}

AR_FN Real world_env_pdf_li(Real3 wi, const EnvTableView* env) {
  AR_ASSERT(env != nullptr);
  return env_pdf(env[kEnvPlain], env[kEnvCompensated], wi, true);
}

// pbrt ImageInfiniteLight::Phi = 4 pi^2 R^2 mean(L) = pi R^2 times the integral of L over the
// sphere; `energy` is that integral weighted by w(lambda) (4 pi times the bake's solid-angle mean,
// times the record's strength), so the factor is the area of the disk the scene sphere shows.
AR_FN Real world_env_power(const SceneSphere& scene, Real energy) { return kLightPi * scene.radius * scene.radius * energy; }

AR_NS_END
