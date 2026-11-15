// SPDX-License-Identifier: Apache-2.0
// Test-only stage and refill functions for the scheduler tests: every path terminates at its
// first hit. Header-only because tests/cpu/*.cpp is globbed by the test_ prefix only.
// Routing rule (typed here, re-derived independently in test_scheduler.cpp): work item r of
// either kind misses when r % 5 == 0, hits a lamp when r % 5 == 1 and a surface otherwise.
//   camera: miss SHADE_BACKGROUND, lamp SHADE_LIGHT, surface SHADE_SURFACE;
//   light:  miss TERMINATED,       lamp SHADE_LIGHT, surface SHADE_LIGHT_PATH (0002 D8).
// A surface or light-path hit emits one shadow item (k = 1) targeting its result index; the
// shadow item is occluded when its index % 3 == 0.
// Ref: docs/decisions/0002-kernel-architecture.md D5 (slot ownership), D7, D8 (light paths).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "kernel/kernel_globals.h"
#include "kernel/state/flags.h"
#include "kernel/state/stage.h"
#include "kernel_cpu/stage_cpu.h"

namespace stub {

// Terminal outcome of a path, written once by the stage that ends it.
enum Outcome : uint32_t { NONE = 0, BACKGROUND = 1, LAMP = 2, SURFACE = 3, LIGHT_PATH = 4, LIGHT_MISS = 5 };

// What the stages write into, reached through KernelGlobals::film. Index of a path: camera
// sample c -> c, light rank l -> camera_count + l. One writer per index.
struct Results {
  uint32_t camera_count = 0;
  std::atomic<bool>* on_closest = nullptr;  // set by every intersect_closest launch (cancel tests)
  std::vector<uint32_t> outcome;         // Outcome per index
  std::vector<uint32_t> shadow_delivered;  // index + 1 once its shadow item reached SHADE_SHADOW
  explicit Results(uint32_t cameras, uint32_t lights)
      : camera_count(cameras), outcome(cameras + lights, NONE), shadow_delivered(cameras + lights, 0) {}
};

inline Results& results(const ar::KernelGlobals& kg) { return *reinterpret_cast<Results*>(kg.film); }

inline uint32_t index_of(const ar::KernelGlobals& kg, uint32_t slot) {
  const bool light = (ar::ps_flags(kg, slot) & ar::PATH_KIND) != 0u;
  return ar::ps_sample_id(kg, slot) + (light ? results(kg).camera_count : 0u);
}

// The refill launches: item i of the launch takes slot main_queue[start + i] and work item
// shadow_base + i of the TERMINATED entry (sched/step_table.h).
inline void generate(const ar::KernelGlobals& kg, uint32_t first, uint32_t last, bool light) {
  const ar::StepEntry& e = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)];
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t slot = kg.main_queue[e.start + i];
    ar::ps_sample_id(kg, slot) = e.shadow_base + i;
    ar::ps_flags(kg, slot) = light ? ar::PATH_KIND : 0u;
    ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(ar::Stage::INTERSECT_CLOSEST);
  }
}
inline void generate_camera(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) { generate(kg, first, last, false); }
inline void generate_light(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) { generate(kg, first, last, true); }

inline void intersect_closest(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  if (results(kg).on_closest) results(kg).on_closest->store(true);
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t slot = kg.main_queue[step.start + i];
    const bool light = (ar::ps_flags(kg, slot) & ar::PATH_KIND) != 0u;
    const uint32_t r = ar::ps_sample_id(kg, slot) % 5u;
    ar::Stage next = light ? ar::Stage::SHADE_LIGHT_PATH : ar::Stage::SHADE_SURFACE;
    if (r == 0u) next = light ? ar::Stage::TERMINATED : ar::Stage::SHADE_BACKGROUND;
    if (r == 1u) next = ar::Stage::SHADE_LIGHT;
    if (next == ar::Stage::TERMINATED) results(kg).outcome[index_of(kg, slot)] = LIGHT_MISS;
    ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(next);
  }
}

inline void end_path(const ar::KernelGlobals& kg, uint32_t slot, Outcome o) {
  results(kg).outcome[index_of(kg, slot)] = o;
  ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(ar::Stage::TERMINATED);
}

inline void shade_background(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) end_path(kg, kg.main_queue[step.start + i], BACKGROUND);
}
inline void shade_light(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) end_path(kg, kg.main_queue[step.start + i], LAMP);
}

// A producer: item i owns shadow_queue[shadow_base + i] (k = 1).
inline void shade_with_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last,
                              Outcome o) {
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t slot = kg.main_queue[step.start + i];
    const uint32_t s = kg.shadow_queue[step.shadow_base + i];
    ar::sh_target(kg, s) = index_of(kg, slot);
    ar::sh_next_stage(kg, s) = static_cast<uint8_t>(ar::Stage::INTERSECT_SHADOW);
    end_path(kg, slot, o);
  }
}
inline void shade_surface(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  shade_with_shadow(kg, step, first, last, SURFACE);
}
inline void shade_light_path(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  shade_with_shadow(kg, step, first, last, LIGHT_PATH);
}

inline void intersect_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t s = kg.shadow_queue[step.start + i];
    ar::sh_next_stage(kg, s) = static_cast<uint8_t>(ar::sh_target(kg, s) % 3u == 0u ? ar::Stage::SHADOW_TERMINATED
                                                                                    : ar::Stage::SHADE_SHADOW);
  }
}
inline void shade_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t s = kg.shadow_queue[step.start + i];
    results(kg).shadow_delivered[ar::sh_target(kg, s)] += ar::sh_target(kg, s) + 1u;  // += so a duplicate or lost item shows
    ar::sh_next_stage(kg, s) = static_cast<uint8_t>(ar::Stage::SHADOW_TERMINATED);
  }
}

}  // namespace stub
