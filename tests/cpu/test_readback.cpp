// SPDX-License-Identifier: Apache-2.0
// Session readback (0008 d13a, 0001 D2.11): read_pass on a synthetic 3x2 film agrees with the
// film helpers of 047/049 (moment_mean, moment_variance_of_mean, film_rel_error, ...), and a
// session's polled report holds the scene's entries beside its own, in 016's order.
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a, d10, d12; 0001 D2.11.
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/film/film_layout.h"
#include "host/film/film_stats.h"
#include "host/film/film_view.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// Exact in float, so the sum n * t and the division by n round trip.
constexpr float kTransparency[6] = {0.0f, 0.25f, 0.5f, 1.0f, 1.25f, -0.25f};

// Pixel p of the 3x2 film: n = 4 (pixel 5: n = 1); display component c has S1 = n (10c + p + 1)
// and a spread that gives variance of the mean 0.5 + p/8; bounces mean p + 1; invalid p.
struct ReadRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Film> film;

  ReadRig() : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = 3;
    req.max_height = 2;
    req.primary_aov = true;
    req.depth = true;
    req.transparency = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    ar::FilmData& d = film->data();
    const FilmLayout& l = film->layout();
    for (uint32_t p = 0; p < 6; ++p) {
      const uint32_t n = p == 5 ? 1u : 4u;
      d.n[p] = n;
      d.invalid[p] = p;
      for (uint32_t c = 0; c < 3; ++c) {
        const double s1 = double(n) * double(10 * c + p + 1);
        d.s1[uint64_t(c) * d.plane_stride + p] = s1;
        d.s2[uint64_t(c) * d.plane_stride + p] = s1 * s1 / double(n) + 12.0 * (0.5 + p / 8.0);
      }
      d.add[uint64_t(l.offset("bounces")) * d.plane_stride + p] = float(2 * (p + 1) * n);
      d.count[uint64_t(l.offset("bounce_paths")) * d.plane_stride + p] = n;
      for (uint32_t c = 0; c < 3; ++c)  // sample0: stored as is
        d.add[uint64_t(l.offset("aov_position") + int32_t(c)) * d.plane_stride + p] = float(100 * c + p);
      // Depth stores sample 0's z as is, 0 where the ray never hit (pixel 4). The transparency plane is a
      // sum over the n samples of the mean transparency kTransparency[p]; its values include ones outside
      // 0..1 (Russian roulette) for the clamp.
      d.add[uint64_t(l.offset("aov_depth")) * d.plane_stride + p] = p == 4 ? 0.0f : 2.5f + float(p);
      d.add[uint64_t(l.offset("transparency")) * d.plane_stride + p] = float(n) * kTransparency[p];
    }
  }
};

const PassDesc& pass_named(const std::vector<PassDesc>& passes, const char* name) {
  for (const PassDesc& p : passes)
    if (p.name == name) return p;
  FAIL("no pass " << name);
  return passes[0];
}

std::vector<float> read(const FilmView& c, const PassDesc& p, uint32_t q) {
  std::vector<float> v(pass_float_count(p, q, c.width(), c.height()));
  read_pass(c, p, q, uint32_t(v.size()), v.data());
  return v;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("readback/read_pass equals the film helpers") {
  ReadRig rig;
  const auto passes = list_passes(rig.film->layout());
  std::vector<std::string> names;
  for (const PassDesc& p : passes) names.push_back(p.name);
  CHECK(names == std::vector<std::string>{"Combined", "Depth", "Position", "Normal", "UV", "IndexMA", "Alpha",
                                          "AR Sample Count", "AR Bounces", "AR Rel Error", "AR Invalid"});
  FilmView copy(rig.dev, *rig.film);
  const ar::FilmData& d = rig.film->data();

  const std::vector<float> mean = read(copy, pass_named(passes, "Combined"), AR_QUANTITY_MEAN);
  const std::vector<float> var = read(copy, pass_named(passes, "Combined"), AR_QUANTITY_VARIANCE);
  const std::vector<float> cnt = read(copy, pass_named(passes, "Combined"), AR_QUANTITY_COUNT);
  const std::vector<float> rel = read(copy, pass_named(passes, "AR Rel Error"), AR_QUANTITY_MEAN);
  const std::vector<float> bounces = read(copy, pass_named(passes, "AR Bounces"), AR_QUANTITY_MEAN);
  const std::vector<float> inval = read(copy, pass_named(passes, "AR Invalid"), AR_QUANTITY_COUNT);
  const std::vector<float> pos = read(copy, pass_named(passes, "Position"), AR_QUANTITY_MEAN);
  REQUIRE(mean.size() == 18);
  REQUIRE(cnt.size() == 6);
  for (uint32_t p = 0; p < 6; ++p) {
    const uint32_t n = d.n[p];
    for (uint32_t c = 0; c < 3; ++c) {
      const uint64_t at = uint64_t(c) * d.plane_stride + p;
      CHECK(mean[p * 3 + c] == doctest::Approx(float(moment_mean(d.s1[at], n))));
      CHECK(mean[p * 3 + c] == doctest::Approx(10.0 * c + p + 1));
      const double v = moment_variance_of_mean(d.s1[at], d.s2[at], n);
      if (std::isnan(v)) CHECK(std::isnan(var[p * 3 + c]));
      else CHECK(var[p * 3 + c] == doctest::Approx(float(v)));
      CHECK(pos[p * 3 + c] == float(100 * c + p));  // sample0 channel: not divided by n
    }
    CHECK(cnt[p] == float(n));
    CHECK(rel[p] == doctest::Approx(float(film_rel_error(d, p))));
    CHECK(bounces[p] == doctest::Approx(float(2 * (p + 1))));
    CHECK(inval[p] == float(p));
  }
  CHECK(rel[5] == -1.0f);  // n = 1: no data
}

TEST_CASE("readback/alpha is one minus transparency") {
  // Cycles film/read.h: alpha = saturate(1 - transparency), so the stored mean (a sum over n samples)
  // must be divided by n, subtracted from 1 and clamped; and a stored depth of 0 reads 1e10 while any
  // other depth is read as stored.
  ReadRig rig;
  const auto passes = list_passes(rig.film->layout());
  const PassDesc& alpha = pass_named(passes, "Alpha");
  CHECK(alpha.source == PassSource::Alpha);
  CHECK(alpha.components == 1);
  FilmView copy(rig.dev, *rig.film);
  const std::vector<float> a = read(copy, alpha, AR_QUANTITY_MEAN);
  const float expect[6] = {1.0f, 0.75f, 0.5f, 0.0f, 0.0f, 1.0f};  // 1.25 and -0.25 are clamped
  REQUIRE(a.size() == 6);
  for (uint32_t p = 0; p < 6; ++p) CHECK(a[p] == expect[p]);
  const std::vector<float> z = read(copy, pass_named(passes, "Depth"), AR_QUANTITY_MEAN);
  REQUIRE(z.size() == 6);
  for (uint32_t p = 0; p < 6; ++p) CHECK(z[p] == (p == 4 ? 1e10f : 2.5f + float(p)));

  // A pixel without a sample has no alpha: NaN, not a made-up 1.
  rig.film->data().n[2] = 0;
  FilmView none(rig.dev, *rig.film);
  CHECK(std::isnan(read(none, alpha, AR_QUANTITY_MEAN)[2]));
  CHECK_THROWS_AS(pass_float_count(alpha, AR_QUANTITY_VARIANCE, 3, 2), std::invalid_argument);
}

TEST_CASE("readback/read_pass maps the render rectangle") {
  // A 2x2 render rectangle inside the 3x2 allocation: pixel j of the rectangle is plane pixel
  // film_active_pixel(j) = (j % 2) + 3 (j / 2), so j = 0, 1, 2, 3 read planes 0, 1, 3, 4.
  ReadRig rig;
  rig.film->set_render_size(2, 2);
  const auto passes = list_passes(rig.film->layout());
  FilmView view(rig.dev, *rig.film);
  const std::vector<float> mean = read(view, pass_named(passes, "Combined"), AR_QUANTITY_MEAN);
  const std::vector<float> cnt = read(view, pass_named(passes, "Combined"), AR_QUANTITY_COUNT);
  const std::vector<float> inval = read(view, pass_named(passes, "AR Invalid"), AR_QUANTITY_MEAN);
  REQUIRE(mean.size() == 12);
  const uint32_t plane[4] = {0, 1, 3, 4};
  for (uint32_t j = 0; j < 4; ++j) {
    CHECK(ar::film_active_pixel(rig.film->data(), j) == plane[j]);
    for (uint32_t c = 0; c < 3; ++c) CHECK(mean[j * 3 + c] == doctest::Approx(10.0 * c + plane[j] + 1));
    CHECK(cnt[j] == 4.0f);
    CHECK(inval[j] == float(plane[j]));
  }
}

TEST_CASE("readback/read_pass refuses what it cannot give") {
  ReadRig rig;
  const auto passes = list_passes(rig.film->layout());
  FilmView copy(rig.dev, *rig.film);
  std::vector<float> buf(64);
  CHECK_THROWS_AS(read_pass(copy, pass_named(passes, "Combined"), AR_QUANTITY_MEAN, 17, buf.data()),
                  std::invalid_argument);
  CHECK_THROWS_AS(read_pass(copy, pass_named(passes, "Combined"), AR_QUANTITY_MEAN, 19, buf.data()),
                  std::invalid_argument);
  CHECK_THROWS_AS(pass_float_count(pass_named(passes, "AR Bounces"), AR_QUANTITY_VARIANCE, 3, 2), std::invalid_argument);
  CHECK_THROWS_AS(pass_float_count(pass_named(passes, "Position"), AR_QUANTITY_COUNT, 3, 2), std::invalid_argument);
  CHECK_THROWS_AS(pass_float_count(pass_named(passes, "Combined"), 9, 3, 2), std::invalid_argument);
}

TEST_CASE("readback/scene entries merged") {
  // The scene raises GENERATION_REJECTED and a SCENE entry; the session raises its own. The polled
  // table has all three, ordered level descending, and revision moves when either table does.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Ledger ledger;
  ThreadPool pool(ctx, 1);
  CpuDevice dev(ledger, pool);
  dev.register_production_stages();
  SessionOptions o;
  o.max_width = o.max_height = 4;
  Session s(scene, dev, o);
  const double value[2] = {0.0, 0.0};
  REQUIRE(scene.report().raise(AR_REPORT_GENERATION_REJECTED, 7, "bad mesh", 1, value, 3) == AR_OK);
  REQUIRE(scene.report().raise(AR_REPORT_NODE_UNSUPPORTED, 9, "node", 2, value, 3) == AR_OK);
  REQUIRE(s.report().raise(AR_REPORT_INVALID_SAMPLE, 0, "", 5, value, 1) == AR_OK);
  const ar_report_info a = s.report_snapshot();
  REQUIRE(a.entry_count == 3);
  CHECK(a.highest_level == AR_LEVEL_ERROR);
  ar_report_entry e[3];
  for (uint32_t i = 0; i < 3; ++i) s.report_entry(i, &e[i]);
  CHECK(e[0].code == AR_REPORT_GENERATION_REJECTED);
  CHECK(e[0].subject == 7);
  CHECK(e[1].level == AR_LEVEL_WARNING);
  CHECK(e[2].level == AR_LEVEL_WARNING);
  CHECK(e[1].code < e[2].code);
  CHECK_THROWS_AS(s.report_entry(3, &e[0]), std::invalid_argument);
  const ar_report_info same = s.report_snapshot();
  CHECK(same.revision == a.revision);
  REQUIRE(scene.report().raise(AR_REPORT_NODE_UNSUPPORTED, 9, "node", 1, value, 4) == AR_OK);
  CHECK(s.report_snapshot().revision != a.revision);
  s.close();
  s.join();
  // After CLOSED the scene's entries come from the copy taken at detach.
  CHECK(s.report_snapshot().entry_count == 3);
}

TEST_CASE("readback/counter words in format_counters order") {
  PassCounters c;
  c.rays_closest = 1;
  c.rays_shadow = 2;
  c.steps = 3;
  c.launches = 4;
  for (uint32_t i = 0; i < ar::kStageCount; ++i) c.items[i] = 100 + i;
  c.active_path_steps = 200;
  c.peak_active_paths = 201;
  c.refills = 202;
  c.camera_paths = 203;
  c.light_paths = 204;
  c.sorts = 205;
  c.state_bytes_moved = 206;
  c.shadow_item_bytes_moved = 207;
  c.lamp_crossings = 208;
  c.shadow_rays_emitted = 209;
  c.permuted_launches = 210;
  const std::vector<uint64_t> w = counter_words(c);
  REQUIRE(w.size() == 15 + ar::kStageCount);
  CHECK(w[0] == 1);
  CHECK(w[3] == 4);
  for (uint32_t i = 0; i < ar::kStageCount; ++i) CHECK(w[4 + i] == 100 + i);
  for (uint32_t k = 0; k < 11; ++k) CHECK(w[4 + ar::kStageCount + k] == 200 + k);
}

TEST_CASE("readback/band passes are refused until the unit is known") {
  FilmRequest req;
  req.max_width = req.max_height = 2;
  req.extra.push_back({"band_v", ChannelKind::Moment, 1, 0});
  CHECK_THROWS_AS(list_passes(FilmLayout::build(req)), std::invalid_argument);
}

namespace {
// Item i ends where it starts, so a pass with work launches stages and counts them.
void gen_terminate(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  const ar::StepEntry& e = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)];
  for (uint32_t i = first; i < last; ++i)
    ar::ps_next_stage(kg, kg.main_queue[e.start + i]) = static_cast<uint8_t>(ar::Stage::TERMINATED);
}
}  // namespace

TEST_CASE("readback/session counters are non-zero after real passes") {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Ledger ledger;
  ThreadPool pool(ctx, 2);
  CpuDevice dev(ledger, pool);
  dev.register_production_stages();
  dev.register_kernel(KernelId::GENERATE_CAMERA, &gen_terminate);
  SessionOptions o;
  o.max_width = o.max_height = 8;
  o.test_camera_samples_per_pass = 64;
  Session s(scene, dev, o);
  CHECK(counter_words(s.pass_counters())[3] == 0);  // launches: nothing ran yet
  s.start();
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (s.pass_counters().launches == 0 && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const PassCounters c = s.pass_counters();
  const std::vector<uint64_t> w = counter_words(c);
  CHECK(c.launches > 0);
  CHECK(c.steps > 0);
  CHECK(w[2] == c.steps);
  CHECK(w[3] == c.launches);
  s.close();
  s.join();
}

}  // TEST_SUITE
