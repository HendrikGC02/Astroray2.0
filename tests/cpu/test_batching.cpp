// SPDX-License-Identifier: Apache-2.0
// Interaction and progressive batching (0007 d49, d50; 0009 B-34, B-37) on a synthetic clock.
// Ref: docs/decisions/0007-blender-integration.md d49, d50; 0009 B-34, B-37; Cycles
// intern/cycles/integrator/render_scheduler.cpp for the schedule values.
#include <doctest.h>

#include "host/session/batching.h"

using namespace astroray::host;

namespace {
StoredView view_of(float scale, uint32_t w = 64, uint32_t h = 48) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = scale;
  return StoredView::copy_of(v);
}
}  // namespace

TEST_SUITE("fast") {

TEST_CASE("batching/one pass during interaction") {
  Batching b;
  b.note_commit(10.0);
  // However fast a pass is, every displayed update is one pass while interacting.
  for (double t : {10.0, 10.05, 10.1, 10.249}) {
    CHECK(b.interacting(t));
    CHECK(b.passes_per_update(t, 0.0005, 0) == 1);
    CHECK(b.passes_per_update(t, 0.0005, 500) == 1);
    CHECK(b.publish_gap_s(t) == doctest::Approx(1.0 / 60.0));
  }
}

TEST_CASE("batching/passes grow after interaction") {
  Batching b;
  b.note_commit(10.0);
  const double pass = 0.004;  // 4 ms
  const double end = 10.0 + kInteractionWindowS;
  // Cycles' schedule: interval 0.1, 0.25, 0.5, 1.0 s, then 2.0 s once 32 samples are done.
  struct Row { double settled; uint64_t samples; double interval; uint32_t passes; };
  // interval / 4 ms = 25, 62, 125, 250, 500 samples; Cycles rounds to the nearer power of two.
  const Row rows[] = {{0.0, 4, 0.1, 32},   {0.99, 4, 0.1, 32}, {1.0, 4, 0.25, 64}, {2.0, 4, 0.5, 128},
                      {4.0, 4, 1.0, 256},  {7.9, 31, 1.0, 256}, {8.0, 31, 1.0, 256}, {8.0, 32, 2.0, 512}};
  uint32_t last = 0;
  for (const Row& r : rows) {
    CHECK(display_interval_s(r.settled, r.samples) == doctest::Approx(r.interval));
    CHECK(b.passes_per_update(end + r.settled, pass, r.samples) == r.passes);
    CHECK(r.passes >= last);
    last = r.passes;
  }
  // round_num_samples_to_power_of_2: 1 stays, powers stay, else the nearer one, a tie (6, 12) going up.
  CHECK(round_to_power_of_2(0) == 1);
  CHECK(round_to_power_of_2(1) == 1);
  CHECK(round_to_power_of_2(8) == 8);
  CHECK(round_to_power_of_2(5) == 4);
  CHECK(round_to_power_of_2(6) == 8);
  CHECK(round_to_power_of_2(7) == 8);
  CHECK(round_to_power_of_2(11) == 8);
  CHECK(round_to_power_of_2(12) == 16);
  // A pass slower than the interval still makes progress: at least one pass per update.
  CHECK(b.passes_per_update(end + 5.0, 3.0, 100) == 1);
  CHECK(b.publish_gap_s(end) == doctest::Approx(0.1));
  // 0009 B-34: after interaction no more than 10 frames/s, whatever the pass time.
  CHECK(display_interval_s(0.0, 0) >= 0.1);
}

TEST_CASE("batching/interaction ends 250 ms after the last event") {
  Batching b;
  CHECK_FALSE(b.interacting(5.0));  // nothing happened yet
  b.note_commit(10.0);
  CHECK(b.interacting(10.2499));
  CHECK_FALSE(b.interacting(10.25));
  b.note_view(view_of(1.0), 10.2);  // a later event extends the window
  CHECK(b.interacting(10.4499));
  CHECK_FALSE(b.interacting(10.45));
  b.note_commit(10.3);              // an older timestamp never shortens it
  CHECK(b.interacting(10.4499));
}

TEST_CASE("batching/scale changes are ignored") {
  Batching b;
  CHECK(b.note_view(view_of(1.0), 1.0));  // the first view differs from none
  CHECK_FALSE(b.interacting(2.0));
  CHECK_FALSE(b.note_view(view_of(0.5), 2.0));  // the controller moved the scale only
  CHECK_FALSE(b.interacting(2.0));
  CHECK_FALSE(b.note_view(view_of(1.0), 2.1));
  CHECK_FALSE(b.interacting(2.1));
  CHECK(b.note_view(view_of(1.0, 65, 48), 3.0));  // a region change is a hash change
  CHECK(b.interacting(3.1));
  CHECK_FALSE(b.note_view(view_of(1.0, 65, 48), 3.2));  // the same view again: no new event
  CHECK_FALSE(b.interacting(3.26));
}

}  // TEST_SUITE
