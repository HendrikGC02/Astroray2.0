// SPDX-License-Identifier: Apache-2.0
// Per-pass work counters (u64), filled by the scheduler from the step tables it already reads.
// They are deterministic for a fixed scene, seed and device (0002 D12), which is what the
// gate's B-16 check compares against tests/budgets/counters.toml (filled by 076).
// lamp_crossings and shadow_rays_emitted are filled by later stages through counter slots in
// KernelGlobals; until a stage does, they stay 0.
// Ref: docs/decisions/0009-process-and-quality.md d18 (work counters, B-16);
// 0004 D29 (per-pass counters); 0002 D7, D13 (state sizes).
#pragma once
#include <cstdint>
#include <string>

#include "kernel/state/stage.h"

namespace astroray::host {

struct PassCounters {
  uint64_t rays_closest = 0;  // items of INTERSECT_CLOSEST
  uint64_t rays_shadow = 0;   // items of INTERSECT_SHADOW
  uint64_t steps = 0;
  uint64_t launches = 0;  // every device launch: queue build, sort, refill, stages
  uint64_t items[ar::kStageCount] = {};  // launched items per stage
  uint64_t active_path_steps = 0;        // sum over steps of the live main-pool paths
  uint64_t peak_active_paths = 0;
  uint64_t refills = 0;
  uint64_t camera_paths = 0;  // generated this pass
  uint64_t light_paths = 0;
  uint64_t sorts = 0;
  uint64_t state_bytes_moved = 0;        // main-pool stage items x bytes per path
  uint64_t shadow_item_bytes_moved = 0;  // shadow-pool stage items x bytes per shadow item
  uint64_t lamp_crossings = 0;
  uint64_t shadow_rays_emitted = 0;
  uint64_t permuted_launches = 0;  // AR_DEBUG_PERMUTE only
};

// One line per counter, "name value", in declaration order.
std::string format_counters(const PassCounters& c);

}  // namespace astroray::host
