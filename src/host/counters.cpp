// SPDX-License-Identifier: Apache-2.0
#include "host/counters.h"

namespace astroray::host {

std::string format_counters(const PassCounters& c) {
  std::string s;
  auto add = [&s](const std::string& name, uint64_t v) { s += name + " " + std::to_string(v) + "\n"; };
  add("rays_closest", c.rays_closest);
  add("rays_shadow", c.rays_shadow);
  add("steps", c.steps);
  add("launches", c.launches);
  for (uint32_t i = 0; i < ar::kStageCount; ++i) add("items_stage_" + std::to_string(i), c.items[i]);
  add("active_path_steps", c.active_path_steps);
  add("peak_active_paths", c.peak_active_paths);
  add("refills", c.refills);
  add("camera_paths", c.camera_paths);
  add("light_paths", c.light_paths);
  add("sorts", c.sorts);
  add("state_bytes_moved", c.state_bytes_moved);
  add("shadow_item_bytes_moved", c.shadow_item_bytes_moved);
  add("lamp_crossings", c.lamp_crossings);
  add("shadow_rays_emitted", c.shadow_rays_emitted);
  add("permuted_launches", c.permuted_launches);
  return s;
}

}  // namespace astroray::host
