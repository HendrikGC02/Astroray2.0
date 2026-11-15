// SPDX-License-Identifier: Apache-2.0
// The host light table: every light of the scene (lamps, emissive triangles, the constant world)
// as kernel LightRecords, each light's importance energy, and the alias table the kernel's
// LightSampler reads (kernel/light/light_sampler.h). Built in double. Host arrays count in the
// ledger's TABLES category; the CUDA upload is 102's. Lamps enter from 068 on; until then only
// tests pass them.
// Ref: docs/decisions/0004-integrator.md D9 (triangle power A pi estimate, pmf 0 for estimate 0),
// D10 (importance energy integral S(lambda) w(lambda) with w the normalised display response; the
// radiometric floor and excitation weighting are M4); pbrt-v4 src/pbrt/util/sampling.cpp
// AliasTable::AliasTable, src/pbrt/lightsamplers.cpp PowerLightSampler (Apache-2.0); Cycles
// intern/cycles/scene/shader.cpp Shader::estimate_emission (EMISSION_SAMPLING_AUTO, Apache-2.0).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "../../kernel/light/light_sampler.h"
#include "host/ledger.h"
#include "host/material/compiler.h"

namespace astroray::host {

struct InstanceTable;
struct MaterialArrays;
class MeshStore;
struct MeshRecord;
struct WorldOutput;

// One light and its emission for the importance: the estimate terms (an emissive triangle), or
// empty for the record's own emission and strength (lamps, the world).
struct LightInput {
  ar::LightRecord record;
  std::vector<EmissionTerm> terms;
};

struct LightTable {
  std::vector<ar::LightRecord> lights;
  std::vector<double> importance;  // per light
  std::vector<ar::AliasBin> bins;  // one per light of positive importance
  std::vector<uint32_t> bin_light;
  std::vector<float> pmf;          // per light
  ar::SceneSphere scene = {};
  ar::LightSamplerView view() const;
  uint64_t bytes() const;

  LightTable() = default;
  LightTable(const LightTable&) = delete;
  LightTable& operator=(const LightTable&) = delete;
  ~LightTable();  // releases the ledger record
  std::shared_ptr<Ledger> ledger;
  uint64_t ledger_id = 0;
};

// Integral of sum_t strength_t s_t(lambda) w(lambda) over the observer's support, with w the
// CMF sum normalised to integrate to 1 (a 1 nm rectangle rule on the 471 table samples).
double display_energy(const std::vector<EmissionTerm>& terms, const ar::ObserverView& obs);

// The table over `lights` in order. A light whose importance is 0 gets pmf 0 and no bin; with no
// positive importance the table has no bins and sample() finds no light. `ledger` may be null.
std::shared_ptr<const LightTable> build_light_table(const std::vector<LightInput>& lights, const ar::SceneSphere& scene,
                                                    const ar::ObserverView& obs, std::shared_ptr<Ledger> ledger);

// Triangle sidedness from AR_EMISSION_SAMPLING_*: 0 = not a light, else kLightTwoSided,
// kLightBackFace or neither (FRONT), as LightRecord flags with kLightLit set. AUTO follows Cycles:
// two-sided when the largest RGB component of the estimate exceeds 0.5, else not a light. Throws
// std::invalid_argument for a value above FRONT_BACK or an estimate kind other than CONSTANT or RGB.
inline constexpr uint32_t kLightLit = 1u << 16;
uint32_t emission_sidedness(uint32_t emission_sampling, const std::vector<EmissionTerm>& estimate);

// What the scene's light table is built from: emissive triangles of every instance, the world,
// and the scene's bounding sphere. `key` identifies those inputs (the emissive instances' meshes,
// transforms and materials, the world, and the sphere when the world is lit), so an edit that
// leaves it unchanged needs no rebuild. With `collect` false only `key` is computed.
struct SceneLights {
  std::vector<LightInput> lights;
  std::vector<std::shared_ptr<const MeshRecord>> meshes;  // the lit instances' records, in `key`
  ar::SceneSphere scene = {};
  std::vector<uint8_t> key;
};
SceneLights gather_scene_lights(const InstanceTable* instances, const MeshStore& meshes, const MaterialArrays& materials,
                                const WorldOutput* world, bool collect);

}  // namespace astroray::host
