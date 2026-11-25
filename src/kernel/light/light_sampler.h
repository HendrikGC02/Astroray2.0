// SPDX-License-Identifier: Apache-2.0
// The kernel's light sampler over every light of the scene (lamps, emissive triangles, the world):
// sample(ctx, u) -> {light, pmf} and pmf(ctx, light), the LightSampler of 0004 D10. It draws from
// the mixture (1 - eps) p_tree + eps p_rad: the light tree over the display-weighted importance
// (light_tree.h) and a radiometric alias table, the floor that gives every light of positive
// radiometric power a positive pmf. The alias table over the importance is the tree's oracle: the
// gate hook TEST_HOOK_LIGHT_ALIAS (kernel_globals.h) selects it, and a view without the floor's
// tables (rad_pmf null: a hand-built view in a test) is the oracle too. All tables are built on the
// host (host/light/light_builder.h). Only lights with positive importance have an importance bin and
// a tree leaf, and only lights with positive radiometric power a radiometric bin, so an estimate-0
// triangle (D9) has pmf 0, also under the floor, whatever the rounding in the table construction.
// The environment world has no radiometric bin (its bake keeps only the weighted radiance), and
// the tree's distant node gives it a positive pmf at every point.
//
// Ref: docs/decisions/0004-integrator.md D10 (interface, floor, alias table over all lights as the
// oracle), D9 (pmf 0 for estimate 0), D13 (no new dimension: the alias test uniform picks the
// table); pbrt-v4 src/pbrt/util/sampling.cpp AliasTable::Sample, src/pbrt/lightsamplers.h
// PowerLightSampler::Sample / PMF (Apache-2.0); Hesterberg 1995 (defensive mixture).
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "../scene/scene_view.h"
#include "light.h"
#include "light_tree.h"

AR_NS_BEGIN

// One alias-table bin: keep this bin with probability q, else take bin `alias`.
struct AliasBin {
  float q;
  uint32_t alias;
};

struct LightSamplerView {
  // The importance alias table: the oracle.
  const AliasBin* bins;      // bin_count
  const uint32_t* bin_light;  // the light index of each bin
  const float* pmf;           // light_count: selection probability of every light
  uint32_t bin_count, light_count;
  // The default sampler: the tree and the radiometric floor. rad_pmf null selects the oracle.
  LightTreeView tree;
  const AliasBin* rad_bins;      // rad_bin_count
  const uint32_t* rad_bin_light;  // the light index of each radiometric bin
  const float* rad_pmf;           // light_count: p_rad of every light
  uint32_t rad_bin_count;
};

// D10's floor weight eps (PROVISIONAL).
inline constexpr Real kLightFloorEps = Real(0.05);

// pbrt's AliasTable::Sample splits one uniform into the bin and the remainder that is tested
// against q; a float u leaves the remainder only 24 - log2(bins) bits, which biases q at large bin
// counts. Here the bin comes from 32 uniform bits (offset = floor(u_bin n / 2^32), uniform over
// the bins to n / 2^32) and the q test from an independent uniform u_q in [0, 1). kNoLight for an
// empty table.
AR_FN uint32_t light_alias_pick(const AliasBin* bins, const uint32_t* bin_light, uint32_t count, uint32_t u_bin, Real u_q) {
  if (count == 0u) return kNoLight;
  const uint32_t offset = uint32_t((uint64_t(u_bin) * uint64_t(count)) >> 32);
  const Real up = ar::min(u_q, kLightOneMinusEpsilon);
  const AliasBin b = bins[offset];
  const uint32_t bin = up < Real(b.q) ? offset : b.alias;
  return ar::load_ro(bin_light + bin);
}

// The mixture's pmf from the tree's pmf of the light.
AR_FN Real light_floor_pmf(const LightSamplerView& v, Real p_tree, uint32_t light) {
  return (Real(1) - kLightFloorEps) * p_tree + kLightFloorEps * Real(ar::load_ro(v.rad_pmf + light));
}

// u_q < eps draws the radiometric bin from u_bin with u_q / eps as its test uniform; otherwise the
// tree descends with u_bin as its uniform (D13: the two numbers the call sites already hold). In
// both cases the tree's pmf of the drawn light comes from the same walk (light_tree_walk: a
// radiometric draw replays its bit trail), so pmf(ctx, sampled) is the pmf returned (D7). `oracle`
// draws from the importance alias table alone, as before the tree. The tree reads the context's
// point and shading normal (POINT contexts; segments are medium NEE's).
AR_FN SampledLight light_sampler_sample(const LightSamplerView& v, const LightSampleContext& ctx, uint32_t u_bin, Real u_q,
                                        bool oracle) {
  SampledLight s{kNoLight, Real(0)};
  if (oracle || v.rad_pmf == nullptr) {
    const uint32_t light = light_alias_pick(v.bins, v.bin_light, v.bin_count, u_bin, u_q);
    if (light == kNoLight) return s;
    s.light = light;
    s.pmf = Real(ar::load_ro(v.pmf + light));
    return s;
  }
  const bool radiometric = u_q < kLightFloorEps;
  uint32_t target = kNoLight, trail = 0u;
  bool walk = true;
  if (radiometric) {
    target = light_alias_pick(v.rad_bins, v.rad_bin_light, v.rad_bin_count, u_bin, u_q / kLightFloorEps);
    if (target == kNoLight) return s;
    const LightTreeEmitter e = target < v.tree.emitter_count ? v.tree.emitters[target] : LightTreeEmitter{0u, kLightTreeAbsent};
    walk = e.depth != kLightTreeAbsent && e.depth <= kLightTreeMaxDepth;
    trail = e.bit_trail;
  }
  // 2^-32 u_bin, below 1 also where float rounds it up.
  const Real u_tree = ar::min(Real(u_bin) * Real(2.3283064365386963e-10), kLightOneMinusEpsilon);
  const SampledLight t = walk ? light_tree_walk(v.tree, ctx.p, ctx.n_shade, u_tree, radiometric, trail) : s;
  const uint32_t light = radiometric ? target : t.light;
  if (light == kNoLight) return s;
  AR_ASSERT(light < v.light_count);
  s.light = light;
  s.pmf = light_floor_pmf(v, t.light == light ? t.pmf : Real(0), light);
  return s;
}

AR_FN Real light_sampler_pmf(const LightSamplerView& v, const LightSampleContext& ctx, uint32_t light, bool oracle) {
  if (light >= v.light_count) return Real(0);
  if (oracle || v.rad_pmf == nullptr) return Real(ar::load_ro(v.pmf + light));
  return light_floor_pmf(v, light_tree_pmf(v.tree, ctx.p, ctx.n_shade, light), light);
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
  if (instance_light_index(kg, instance) != kNoLightIndex) return kNoLight;  // a lamp instance
  const uint32_t mesh = load_ro(&s.instances[instance].mesh);
  const bool mesh_ok = mesh < s.mesh_count;
  AR_ASSERT(mesh_ok);
  if (!mesh_ok) return kLightIndexInvalid;
  const bool tri_ok = triangle < load_ro(&s.meshes[mesh].triangle_count);
  AR_ASSERT(tri_ok);
  if (!tri_ok) return kLightIndexInvalid;
  AR_ASSERT(kg.light_index != nullptr || kg.lights == nullptr);  // a light table binds its index
  return light_index_lookup(kg.light_index, kg.light_index_words, instance, triangle);
}

AR_NS_END
