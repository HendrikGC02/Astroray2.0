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

#include "../kernel_globals.h"
#include "../scene/scene_view.h"
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

// The emissive-hit index: which light a BSDF ray's triangle hit is, so emission MIS can take that
// light's pmf (0004 D1, D9). One uint32 array (KernelGlobals::light_index, light_index_words
// words), built on the host from the light records themselves (host/light/light_builder.cpp
// build_light_table), so lights[entry] is the record of that very triangle:
//   word 0             N, the instance entries (one past the last instance with an emissive triangle)
//   words 1 .. N       per instance, the word offset of its slice, or kNoLight (no emissive triangle)
//   slice              C (one past its last emissive triangle), then C light indices, kNoLight for
//                      a triangle that is not a light
// Instances at or past N and triangles at or past C emit nothing: the lights' rebuild key covers
// only emissive instances, so a later unlit instance does not change the array.
//
// A hit outside the scene (instance at or past instance_count, or triangle at or past its mesh's
// triangle_count) is malformed: a debug build traps, a release build returns kLightIndexInvalid,
// which is neither a light nor kNoLight, so a caller can never mistake it for either.
inline constexpr uint32_t kLightIndexInvalid = ~0u - 1u;

// The array lookup alone, for a hit already known to be inside the scene.
AR_FN uint32_t light_index_lookup(const uint32_t* map, uint32_t words, uint32_t inst, uint32_t tri) {
  if (map == nullptr || words == 0u) return kNoLight;
  const uint32_t n = ar::load_ro(map);
  if (inst >= n) return kNoLight;
  AR_ASSERT(1u + inst < words);
  const uint32_t base = ar::load_ro(map + 1u + inst);
  if (base == kNoLight) return kNoLight;
  AR_ASSERT(base < words);
  if (tri >= ar::load_ro(map + base)) return kNoLight;
  AR_ASSERT(base + 1u + tri < words);
  return ar::load_ro(map + base + 1u + tri);
}

// The light index of triangle `triangle` of instance `instance`; kNoLight when it is not a light
// (lamp instances included: their light is DeviceInstanceRecord::light_index, 075).
AR_FN uint32_t light_of_hit(const KernelGlobals& kg, uint32_t instance, uint32_t triangle) {
  const SceneData& s = scene_of(kg);
  const bool inst_ok = instance < s.instance_count;
  AR_ASSERT(inst_ok);
  if (!inst_ok) return kLightIndexInvalid;
  const uint32_t mesh = load_ro(&s.instances[instance].mesh);
  const bool tri_ok = mesh >= s.mesh_count || triangle < load_ro(&s.meshes[mesh].triangle_count);
  AR_ASSERT(tri_ok);
  if (!tri_ok) return kLightIndexInvalid;
  return light_index_lookup(kg.light_index, kg.light_index_words, instance, triangle);
}

AR_NS_END
