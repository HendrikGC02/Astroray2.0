// SPDX-License-Identifier: Apache-2.0
// KernelGlobals: the one 8-byte-pointer argument of every stage. It holds the two
// state SoAs, the step table and queue arrays, the per-session uniforms and the
// scene, film and table pointers. Passed by const reference only (a by-value copy
// costs per-launch parameter traffic, 0002 D4); the struct lives in device memory,
// one copy per session. Feature switches are runtime fields, uniform per launch.
// Scene, film and table members are opaque types; the task that needs a member
// defines its struct and fills the pointer (upload: 102).
// Ref: docs/decisions/0002-kernel-architecture.md D4 (arguments), D6 (step table,
// queues), D8 (feature switches are KernelGlobals fields), D13;
// docs/decisions/0003-spectral-model-and-path-state.md d19-d20, d23.
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "portability.h"
#include "sched/step_table.h"
#include "state/stage.h"
#include "state/state.h"

AR_NS_BEGIN

// Constant for a whole pass. max_bounces uses the order of PathStateSoA::bounces:
// total, diffuse, glossy, transmission, volume, transparent.
struct SessionUniforms {
  uint32_t width;
  uint32_t height;
  uint32_t samples_done;       // n: samples already taken per pixel (camera_key)
  uint32_t samples_per_pass;   // k: samples per active pixel in this pass (camera_key)
  uint32_t pass_index;         // the light key's index
  uint32_t seed;
  uint32_t sampler_mode;       // SAMPLER_SOBOL or SAMPLER_INDEPENDENT (sample/sobol.h)
  uint32_t max_bounces[6];
  StateFeatures features;
};

// Opaque until the task that needs them defines them.
struct SceneData;
struct FilmData;
struct WavelengthTable;
struct Rgb2SpecTables;
struct DisplayTarget;

struct KernelGlobals {
  const PathStateSoA* path;
  const ShadowItemSoA* shadow;

  const StepTable* step_table;
  const uint32_t* main_queue;    // pool-local indices, partitioned by next_stage
  const uint32_t* shadow_queue;
  const uint32_t* active_pixels; // the pass's active-pixel list (camera_key)

  SessionUniforms session;

  const SceneData* scene;
  FilmData* film;
  const WavelengthTable* wavelengths;
  const Rgb2SpecTables* rgb2spec;

  // queue_build's per-tile histograms (sched/step_table.h, stages/queue_build.h): kStageCount + 1
  // counters per tile of both pools. Written only by the three queue_build kernels. Last member
  // so the offsets the CUDA device reads (step_table) stay put.
  uint32_t* queue_counts;

  // The material sort (stages/sort.h): the sorted copy of the SHADE_SURFACE segment and the
  // per-tile histograms and offset scratch. Written only by sort_count and sort_scatter.
  uint32_t* sort_queue;
  uint32_t* sort_counts;

  // film_present and film_heatmap: where they write (film/present.h). Last member so the offsets
  // the CUDA device reads stay put.
  DisplayTarget* display;
};

// ps_<field> and sh_<field>: the slot's element of that field's array. The array is
// null when the field's feature is off; reading it then is a bug that a debug build
// traps.
#define AR_PATH_FIELD(n, t, f)                                  \
  AR_FN t& ps_##n(const KernelGlobals& kg, uint32_t slot) {     \
    AR_ASSERT(kg.path->n != nullptr);                           \
    return kg.path->n[slot];                                    \
  }
#include "state/path_state.def"
#undef AR_PATH_FIELD

#define AR_SHADOW_FIELD(n, t, f)                                \
  AR_FN t& sh_##n(const KernelGlobals& kg, uint32_t slot) {     \
    AR_ASSERT(kg.shadow->n != nullptr);                         \
    return kg.shadow->n[slot];                                  \
  }
#define AR_SHADOW_FIELD_SHARED(n, t, f, nt)
#include "state/shadow_item.def"
#undef AR_SHADOW_FIELD
#undef AR_SHADOW_FIELD_SHARED

// The shared r_l / film_pos array (shadow_item.def): Spec elements with media, Real2
// elements without. NEE items use sh_r_l, connections sh_film_pos; no item uses both.
AR_FN Spec& sh_r_l(const KernelGlobals& kg, uint32_t slot) {
  AR_ASSERT(kg.session.features.media && kg.shadow->r_l_film_pos != nullptr);
  return kg.shadow->r_l_film_pos[slot];
}

AR_FN Real2& sh_film_pos(const KernelGlobals& kg, uint32_t slot) {
  AR_ASSERT(kg.shadow->r_l_film_pos != nullptr);
  if (kg.session.features.media) return *reinterpret_cast<Real2*>(&kg.shadow->r_l_film_pos[slot]);
  return reinterpret_cast<Real2*>(kg.shadow->r_l_film_pos)[slot];
}

AR_NS_END
