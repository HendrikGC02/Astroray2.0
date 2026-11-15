// SPDX-License-Identifier: Apache-2.0
// The alias-table light sampler over every light of the scene (lamps, emissive triangles, the
// world): sample(ctx, u) -> {light, pmf} and pmf(ctx, light), the LightSampler of 0004 D10. It
// ignores the context (power sampling); the light tree (M4) implements the same interface.
// Built on the host (host/light/light_builder.h). Only lights with positive importance have a
// bin, so a light of importance 0 (a zero-estimate triangle, D9) has pmf 0 and can never be
// drawn, whatever the rounding in the table construction.
//
// Ref: docs/decisions/0004-integrator.md D10 (interface, alias table over all lights), D9
// (pmf 0 for estimate 0); pbrt-v4 src/pbrt/util/sampling.cpp AliasTable::Sample,
// src/pbrt/lightsamplers.h PowerLightSampler::Sample / PMF (Apache-2.0).
#pragma once
#include <cstdint>

#include "light.h"

AR_NS_BEGIN

// One alias-table bin: keep this bin with probability q, else take bin `alias`.
struct AliasBin {
  float q;
  uint32_t alias;
};

struct LightSamplerView {
  const AliasBin* bins;      // bin_count
  const uint32_t* bin_light;  // the light index of each bin
  const float* pmf;           // light_count: selection probability of every light
  uint32_t bin_count, light_count;
};

inline constexpr uint32_t kNoLight = ~0u;

// pmf 0 (light kNoLight) when the scene has no light of positive importance.
struct SampledLight {
  uint32_t light;
  Real pmf;
};

// pbrt's AliasTable::Sample splits one uniform into the bin and the remainder that is tested
// against q; a float u leaves the remainder only 24 - log2(bins) bits, which biases q at large bin
// counts. Here the bin comes from 32 uniform bits (offset = floor(u_bin n / 2^32), uniform over
// the bins to n / 2^32) and the q test from an independent uniform u_q in [0, 1).
AR_FN SampledLight light_sampler_sample(const LightSamplerView& v, const LightSampleContext& /*ctx*/, uint32_t u_bin,
                                        Real u_q) {
  SampledLight s;
  s.light = kNoLight;
  s.pmf = Real(0);
  if (v.bin_count == 0u) return s;
  const uint32_t offset = uint32_t((uint64_t(u_bin) * uint64_t(v.bin_count)) >> 32);
  const Real up = ar::min(u_q, kLightOneMinusEpsilon);
  const AliasBin b = v.bins[offset];
  const uint32_t bin = up < Real(b.q) ? offset : b.alias;
  const uint32_t light = ar::load_ro(v.bin_light + bin);
  s.light = light;
  s.pmf = Real(ar::load_ro(v.pmf + light));
  return s;
}

AR_FN Real light_sampler_pmf(const LightSamplerView& v, const LightSampleContext& /*ctx*/, uint32_t light) {
  if (light >= v.light_count) return Real(0);
  return Real(ar::load_ro(v.pmf + light));
}

AR_NS_END
