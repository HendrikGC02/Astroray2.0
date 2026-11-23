// SPDX-License-Identifier: Apache-2.0
// KernelGlobals: the one 8-byte-pointer argument of every stage. It holds the two
// state SoAs, the step table and queue arrays, the per-session uniforms and the
// scene, film and table pointers. Passed by const reference only (a by-value copy
// costs per-launch parameter traffic, 0002 D4); the struct lives in device memory,
// one copy per session. Feature switches are runtime fields, uniform per launch.
// Scene, film and table members are opaque types defined by their own headers;
// host/session/kg_builder.h fills every pointer, on both devices, and says which ones a
// feature set leaves unused.
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

// SessionUniforms::test_hooks bits. Set only by the host from a named test environment variable
// (101 reads ASTRORAY_TEST_PIXEL_CENTRE for 0007 V18); a hook changes sampling, never physics.
// PIXEL_CENTRE: generate_camera takes the filter offset as 0, so every camera ray goes through its
// pixel centre.
inline constexpr uint32_t TEST_HOOK_PIXEL_CENTRE = 1u << 0;
// NEE mode (ASTRORAY_TEST_NEE, RULINGS.md 77), two exclusive bits; neither is the default MIS of
// light and BSDF sampling. The three estimators must agree in the mean (Veach 1997, ch. 9):
// NEE_OFF: BSDF sampling only. No light sample, no shadow item; an emitter hit is weighted 1.
// NEE_LIGHT_ONLY: light sampling only. The NEE weight drops the BSDF pdf; an emitter reached after
// a non-delta bounce adds nothing. Emission at the first vertex or after a delta bounce counts in
// every mode, because no light sample can reach it. Unbiased only while every light has pmf > 0
// (0004 D9: a pmf-0 light is reached by BSDF rays alone), which the test scenes ensure.
inline constexpr uint32_t TEST_HOOK_NEE_OFF = 1u << 1;
inline constexpr uint32_t TEST_HOOK_NEE_LIGHT_ONLY = 1u << 2;

// Constant for a whole pass. max_bounces uses the order of PathStateSoA::bounces:
// total, diffuse, glossy, transmission, volume, transparent.
struct SessionUniforms {
  uint32_t width;
  uint32_t height;
  uint32_t samples_done;       // n: samples already taken per pixel (camera_key)
  uint32_t samples_per_pass;   // k: samples per active pixel in this pass (camera_key)
  uint32_t pass_index;         // the light key's index
  uint32_t seed;
  uint32_t test_hooks;         // TEST_HOOK_* bits; 0 in every production session
  uint32_t sampler_mode;       // SAMPLER_SOBOL or SAMPLER_INDEPENDENT (sample/sobol.h)
  uint32_t max_bounces[6];
  StateFeatures features;
  // Blender's min_light_bounces: Russian roulette starts at depth max(2, min_bounces), so a value
  // at or above max_bounces[0] disables it, as in Cycles (0004 D2).
  uint32_t min_bounces;
};

// Opaque until the task that needs them defines them.
struct SceneData;    // scene/scene_view.h
struct FilmData;     // film/film_write.h
struct SceneTables;  // vm/context.h: the VM's rgb2spec, observer, image and sRGB tables
struct DisplayTarget;
struct LightRecord;       // light/light_record.h
struct LightSamplerView;  // light/light_sampler.h
struct EDielTable;        // material/lobes/tables.h
struct CameraView;   // camera/camera.h
struct FilterTable;  // film/filter.h

// bake_environment's output: the n x n grid it fills, one float per cell, index iv n + iu
// (stages/bake_environment.h). Written by the host baker just before the launch (host/light/env_bake.h).
struct EnvBake {
  float* grid;
  uint32_t n;
};

struct KernelGlobals {
  const PathStateSoA* path;
  const ShadowItemSoA* shadow;

  const StepTable* step_table;
  const uint32_t* main_queue;    // pool-local indices, partitioned by next_stage
  const uint32_t* shadow_queue;
  // The pass's active-pixel list (camera_key). Written only by film_build_active at pass start, read
  // by every other stage until the pass drains (0008 d3); non-const for that one writer.
  uint32_t* active_pixels;

  SessionUniforms session;

  const SceneData* scene;
  FilmData* film;
  const SceneTables* tables;

  // queue_build's per-tile histograms (sched/step_table.h, stages/queue_build.h): kStageCount + 1
  // counters per tile of both pools. Written only by the three queue_build kernels. Last member
  // so the offsets the CUDA device reads (step_table) stay put.
  uint32_t* queue_counts;

  // The material sort (stages/sort.h): the sorted copy of the SHADE_SURFACE segment and the
  // per-tile histograms and offset scratch. Written only by sort_count, sort_scan and sort_scatter.
  uint32_t* sort_queue;
  uint32_t* sort_counts;

  // film_present and film_heatmap: where they write (film/present.h). Last member so the offsets
  // the CUDA device reads stay put.
  DisplayTarget* display;

  // generate_camera reads the camera and the pixel-filter table (host/scene/camera.h,
  // host/film/filter_table.h). Last members so the offsets the CUDA device reads stay put.
  const CameraView* camera;
  const FilterTable* filter;

  // The light table the session latched for its epoch (068): records by light index, the alias
  // sampler over them (light_sampler->light_count lights), and the GGX dielectric E table (064).
  const LightRecord* lights;
  const LightSamplerView* light_sampler;
  const EDielTable* e_diel;
  // Lengths of the scene arrays SceneData does not count: material-pool entries (material_slots),
  // program words and constant blocks.
  uint32_t material_slot_count;
  uint32_t material_program_words;
  uint32_t material_constant_count;
  // Index of the world light in `lights`, kNoLight (~0u) when the scene has none: a camera miss
  // then renders black. The initialiser keeps a value-initialised KernelGlobals at "no world".
  uint32_t world_light = 0xFFFFFFFFu;
  // The emissive-hit index of the same light table (light/light_sampler.h light_of_hit), and its
  // length in words. Last members so the offsets the CUDA device reads stay put.
  const uint32_t* light_index;
  uint32_t light_index_words;
  // Last member so the offsets the CUDA device reads stay put; the baker writes it in place.
  EnvBake env_bake = {};
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
