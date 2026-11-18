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
// (max(int(interval / average time per sample), 1); 1 while no time is recorded) and the static
// helper round_num_samples_to_power_of_2 (1 stays 1, a power of two stays, otherwise the nearer of
// the powers of two above and below, a tie going up), both read from the raw file 2026-11-19. The
// interval is keyed on path_trace_time_.get_wall(), the path-trace time accumulated since the last
// reset, which begin_epoch/note_pass keep here; the average is path_trace_time_.get_average().
#include "host/session/batching.h"

#include <algorithm>
#include <limits>

namespace astroray::host {

uint32_t round_to_power_of_2(uint32_t n) {
  if (n <= 1) return 1;
  if ((n & (n - 1)) == 0) return n;
  uint64_t up = 1;  // 64 bits: the next power of two above a 32-bit n cannot overflow
  while (up < n) up <<= 1;
  const uint64_t down = up - (up >> 1);
  return uint32_t((up - n) <= (n - down) ? up : down);
}

double display_interval_s(double trace_s, uint64_t samples) {
  if (trace_s < 1.0) return 0.1;
  if (trace_s < 2.0) return 0.25;
  if (trace_s < 4.0) return 0.5;
  if (trace_s < 8.0 || samples < 32) return 1.0;
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

void Batching::begin_epoch() {
  std::lock_guard<std::mutex> lock(mutex_);
  trace_s_ = 0.0;
  passes_ = 0;
}

void Batching::note_pass(double pass_s) {
  std::lock_guard<std::mutex> lock(mutex_);
  trace_s_ += pass_s;
  ++passes_;
}

double Batching::trace_s() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return trace_s_;
}

uint32_t Batching::passes_per_update(double now_s, uint64_t samples) const {
  double trace, average;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (any_ && now_s - last_s_ < kInteractionWindowS) return 1;
    // Cycles falls back to one sample while no time is recorded.
    if (passes_ == 0 || !(trace_s_ > 0.0)) return 1;
    trace = trace_s_;
    average = trace_s_ / double(passes_);
  }
  // Cycles converts with int(), so the count never exceeds an int.
  const double want =
      std::min(display_interval_s(trace, samples) / average, double(std::numeric_limits<int>::max()));
  return round_to_power_of_2(std::max(uint32_t(want), 1u));
}

double Batching::publish_gap_s(double now_s) const {
  return interacting(now_s) ? kInteractionPublishGapS : kSettledPublishGapS;
}

}  // namespace astroray::host
