// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0007-blender-integration.md d49, d50; 0009 B-34, B-37.
//
// The display-interval schedule below is taken from Cycles'
// intern/cycles/integrator/render_scheduler.cpp, RenderScheduler::
// guess_display_update_interval_in_seconds_for_num_samples_no_limit (interactive branch), read
// 2026-11-19:
//   SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//   SPDX-License-Identifier: Apache-2.0
// Licensed under the Apache License, Version 2.0; see https://www.apache.org/licenses/LICENSE-2.0.
// The passes per update follow that file's RenderScheduler::calculate_num_samples_per_update
// (max(int(interval / time per sample), 1)) and the static helper round_num_samples_to_power_of_2
// (1 stays 1, a power of two stays, otherwise the nearer of the powers of two above and below, a tie
// going up), both read from the raw file 2026-11-19.
#include "host/session/batching.h"

#include <algorithm>

namespace astroray::host {

uint32_t round_to_power_of_2(uint32_t n) {
  if (n <= 1) return 1;
  if ((n & (n - 1)) == 0) return n;
  uint32_t up = 1;
  while (up < n) up <<= 1;
  const uint32_t down = up - (up >> 1);
  return (up - n) <= (n - down) ? up : down;
}

double display_interval_s(double settled_s, uint64_t samples) {
  if (settled_s < 1.0) return 0.1;
  if (settled_s < 2.0) return 0.25;
  if (settled_s < 4.0) return 0.5;
  if (settled_s < 8.0 || samples < 32) return 1.0;
  return 2.0;
}

void Batching::note_commit(double now_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  any_ = true;
  last_s_ = std::max(last_s_, now_s);
}

bool Batching::note_view(const StoredView& view, double now_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  StoredView a = view, b = view_;
  a.v.render_scale = b.v.render_scale = 1.0f;
  const bool changed = !has_view_ || !a.same_as(b);
  view_ = view;
  has_view_ = true;
  if (changed) {
    any_ = true;
    last_s_ = std::max(last_s_, now_s);
  }
  return changed;
}

bool Batching::interacting(double now_s) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return any_ && now_s - last_s_ < kInteractionWindowS;
}

uint32_t Batching::passes_per_update(double now_s, double pass_s, uint64_t samples) const {
  double settled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (any_ && now_s - last_s_ < kInteractionWindowS) return 1;
    settled = any_ ? now_s - last_s_ - kInteractionWindowS : now_s;
  }
  if (!(pass_s > 0.0)) return 1;
  const double want = display_interval_s(settled, samples) / pass_s;
  const uint32_t n = want >= double(1u << 20) ? (1u << 20) : std::max(uint32_t(want), 1u);
  return round_to_power_of_2(n);
}

double Batching::publish_gap_s(double now_s) const {
  return interacting(now_s) ? kInteractionPublishGapS : kSettledPublishGapS;
}

}  // namespace astroray::host
