// SPDX-License-Identifier: Apache-2.0
// The host light table: every light of the scene (lamps, emissive triangles, the world) as kernel
// LightRecords, each light's importance and radiometric power, and what the kernel's LightSampler
// reads (kernel/light/light_sampler.h): the light tree over the importance (host/light/light_tree.h),
// the radiometric alias table of D10's floor, and the importance alias table, the tree's oracle.
// Built in double. Host arrays count in the ledger's TABLES category; the scene tables upload them.
// Ref: docs/decisions/0004-integrator.md D9 (triangle power A pi estimate, pmf 0 for estimate 0),
// D10 (importance energy integral S(lambda) w(lambda) with w the normalised display response; the
// floor's radiometric power integral S(lambda) over the observer's support; excitation weighting is
// M4); pbrt-v4 src/pbrt/util/sampling.cpp
// AliasTable::AliasTable, src/pbrt/lightsamplers.cpp PowerLightSampler (Apache-2.0); Cycles
// intern/cycles/scene/shader.cpp Shader::estimate_emission (EMISSION_SAMPLING_AUTO, Apache-2.0).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "../../kernel/light/light_sampler.h"
#include "host/ledger.h"
#include "host/light/env_table.h"
#include "host/light/light_tree.h"
#include "host/material/compiler.h"
#include "host/scene/instances.h"

namespace astroray::host {

struct BakedEnvironment;
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
  // D10's default sampler: the tree over the importance (no leaf for importance 0) and the floor's
  // radiometric alias table (one bin per light of positive radiometric power, none for an
  // environment world, whose bake keeps only the weighted radiance), with p_rad per light.
  std::vector<ar::LightTreeNode> tree_nodes;
  std::vector<ar::LightTreeEmitter> tree_emitters;  // per light, empty without lights
  std::vector<double> radiometric;                  // per light: radiometric power
  std::vector<ar::AliasBin> rad_bins;
  std::vector<uint32_t> rad_bin_light;
  std::vector<float> rad_pmf;  // per light
  double tree_build_ms = 0.0;  // the tree's build time, for the scene's log (B-23)
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
  // The world program's environment bake (0004 D11), done before this table was built: its table
  // (both distributions; uploaded as the scene tables' ENV_TABLE) and the un-normalised grid's
  // maximum and solid-angle mean in the unscaled display units of display_energy. Null and 0 for a
  // scene without a program world, a black program world, or one whose grid was refused (its world
  // light is then absent). env_baked is true when this table's world program was baked or refused,
  // so a cached table built without a bake (Scene::lights() alone) is never taken for one that has it.
  std::shared_ptr<const EnvTable> env;
  double env_max = 0.0, env_mean = 0.0;
  bool env_baked = false;
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

// Integral of sum_t strength_t s_t(lambda) over the observer's support, unweighted (D10's floor),
// by the same 1 nm rectangle rule.
double radiometric_energy(const std::vector<EmissionTerm>& terms, const ar::ObserverView& obs);

// Runs body(first, last) over chunks covering [0, count), possibly on several threads, and returns
// when all have run (Scene::lights passes the scene workers' WorkerPool::parallel_for).
using ChunkRunner = std::function<void(uint32_t count, const std::function<void(uint32_t first, uint32_t last)>& body)>;

// True when two lights are triangles with byte-identical estimate terms, so their display energy
// and emitter peak are the same numbers: the triangles of one material compute them once.
bool same_terms(const LightInput& a, const LightInput& b);

// The table over `lights` in order. A light whose importance is 0 gets pmf 0, no bin and no tree
// leaf; a light whose radiometric power is 0 (and a program world) gets no radiometric bin. With no
// positive importance and power nothing can be drawn. `ledger` may be null. Throws
// std::runtime_error when two triangle records name the same instance and triangle, and when the
// tree cannot be built (host/light/light_tree.h build_light_tree: a leaf deeper than 32).
// With `run` the per-light importances are computed in its chunks; every sum, check and the alias
// table stay serial in light order, so the table is bit-identical to the serial build (`run` empty).
// `env` is the world program's bake (host/light/env_bake.h), built before this table: a program
// world (WORLD_CONST with kLightWorldProgram) whose bake has a table becomes WORLD_ENV, with the
// importance 4 pi mean x strength times its power factor (0004 D11); without one (no bake, or a
// black world) it keeps WORLD_CONST with importance 0, so nothing samples it.
std::shared_ptr<const LightTable> build_light_table(const std::vector<LightInput>& lights, const ar::SceneSphere& scene,
                                                    const ar::ObserverView& obs, std::shared_ptr<Ledger> ledger,
                                                    const ChunkRunner& run = {}, const BakedEnvironment* env = nullptr);

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
