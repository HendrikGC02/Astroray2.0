// SPDX-License-Identifier: Apache-2.0
// Interaction and progressive batching of a viewport session: what counts as interaction, how many
// passes run between displayed updates, and how often frames may publish. Pure logic over times
// the caller supplies (seconds), so a test drives it with a synthetic clock; the session feeds it
// steady_clock. The interval schedule runs on path-trace time accumulated since the last epoch
// began, as Cycles' does (path_trace_time_ since reset), so it holds no clock origin of its own.
// Thread safe: the API threads note interaction, the render thread reads and counts passes.
// Ref: docs/decisions/0007-blender-integration.md d49 (interaction), d50 (batching), d30 (rates);
// 0009 B-34 (rates, schedule), B-37 (interaction window); Cycles
// intern/cycles/integrator/render_scheduler.cpp (Apache-2.0) for the schedule.
#pragma once
#include <cstdint>
#include <mutex>

#include "host/session/latch.h"

namespace astroray::host {

inline constexpr double kInteractionWindowS = 0.25;            // 0009 B-37
inline constexpr double kInteractionPublishGapS = 1.0 / 60.0;  // 0009 B-34: at most 60 frames/s
inline constexpr double kSettledPublishGapS = 1.0 / 10.0;      // 0009 B-34: at most 10 frames/s

// Cycles' viewport display interval (seconds) after `trace_s` seconds of path tracing since the
// last reset and `samples` samples per pixel done in the epoch.
double display_interval_s(double trace_s, uint64_t samples);
// Cycles' round_num_samples_to_power_of_2: the nearer power of two, a tie going up.
uint32_t round_to_power_of_2(uint32_t n);

class Batching {
 public:
  // A commit became active.
  void note_commit(double now_s);
  // A set_view call. Counts as interaction when the view differs from the previous one in anything
  // but the render scale (0007 d49: the controller's scale changes never count); the first view
  // always differs from none. Returns whether it counted.
  bool note_view(const StoredView& view, double now_s);
  // True from an interaction event until kInteractionWindowS after the last one; false before
  // any event.
  bool interacting(double now_s) const;
  // Render thread: a new epoch began (view or scene change): path-trace time and the pass average
  // restart, as Cycles' RenderScheduler::reset does.
  void begin_epoch();
  // Render thread: a pass of the epoch took `pass_s` seconds.
  void note_pass(double pass_s);
  // Passes to run before the next displayed update: 1 during interaction; afterwards
  // max(int(interval / average pass time), 1) rounded as Cycles does, 1 while no pass has
  // been timed.
  uint32_t passes_per_update(double now_s, uint64_t samples) const;
  // Smallest time between two published frames.
  double publish_gap_s(double now_s) const;
  // Path-trace seconds of the epoch so far.
  double trace_s() const;

 private:
  mutable std::mutex mutex_;
  bool any_ = false;
  double last_s_ = 0.0;
  bool has_view_ = false;
  StoredView view_;
  double trace_s_ = 0.0;
  uint64_t passes_ = 0;
};

}  // namespace astroray::host
