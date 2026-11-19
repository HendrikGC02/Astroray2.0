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
#include <functional>
#include <memory>
#include <vector>

#include "../../kernel/light/light_sampler.h"
#include "host/ledger.h"
#include "host/material/compiler.h"
#include "host/scene/instances.h"

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
  // The rest of one snapshot, built under the scene's one lock (068): the radiance scale s of 0003
  // d10, already folded into the strengths and estimate terms (the film divides by it), the light
  // index of the first lamp, and the lamps' instance records. A session keeps one table for an
  // epoch; 102 binds that same table.
  double radiance_scale = 1.0;
  uint32_t lamp_first = 0;
  // Index of the constant world among `lights`, ar::kNoLight when the scene has none.
  uint32_t world_light = ~0u;
  std::vector<LampInstanceIn> lamp_instances;
  // The emissive-hit index (layout: kernel/light/light_sampler.h light_of_hit), built from
  // `lights` in the same pass, so lights[entry] is that triangle's record. Never empty: a table
  // without triangle lights holds {0}.
  std::vector<uint32_t> light_index;
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

// Runs body(first, last) over chunks covering [0, count), possibly on several threads, and returns
// when all have run (Scene::lights passes the scene workers' WorkerPool::parallel_for).
using ChunkRunner = std::function<void(uint32_t count, const std::function<void(uint32_t first, uint32_t last)>& body)>;

// True when two lights are triangles with byte-identical estimate terms, so their display energy
// and emitter peak are the same numbers: the triangles of one material compute them once.
bool same_terms(const LightInput& a, const LightInput& b);

// The table over `lights` in order. A light whose importance is 0 gets pmf 0 and no bin; with no
// positive importance the table has no bins and sample() finds no light. `ledger` may be null.
// Throws std::runtime_error when two triangle records name the same instance and triangle.
// With `run` the per-light importances are computed in its chunks; every sum, check and the alias
// table stay serial in light order, so the table is bit-identical to the serial build (`run` empty).
std::shared_ptr<const LightTable> build_light_table(const std::vector<LightInput>& lights, const ar::SceneSphere& scene,
                                                    const ar::ObserverView& obs, std::shared_ptr<Ledger> ledger,
                                                    const ChunkRunner& run = {});

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
