// SPDX-License-Identifier: Apache-2.0
// Statistics and primary-hit AOV channels (0008 d6, d10, d12) through the CPU resolve of 047 on
// a film of a few pixels. Expected values are closed forms: with the flat tables below a slot
// of four lanes v / 256 converts to x = v in every display component (as in test_film_resolve).
// Ref: docs/decisions/0008-film-outputs-denoise.md d6, d10, d12; 0001 D2.11 (INVALID_SAMPLE).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

#include "device/cpu/cpu_device.h"
#include "film/resolve.h"
#include "host/film/film_layout.h"
#include "host/film/film_stats.h"
#include "host/film/response_table.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

struct StatsRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  ResponseTable response;
  WavelengthTable wavelengths;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};

  StatsRig(uint32_t w, uint32_t h, uint32_t k, bool aov = false, bool term = false) : pool(ctx, 1), dev(ledger, pool) {
    response.display_first = 0;
    response.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    response.y_w = 1.0;
    const double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) response.m_ws[i] = m[i];
    for (uint32_t c = 0; c < 3; ++c) {
      response.channels.push_back({2 * c, 2, ar::RESPONSE_ENERGY, 0});
      response.knot_nm.push_back(300.0f);
      response.knot_nm.push_back(900.0f);
      response.knot_value.push_back(1.0f);
      response.knot_value.push_back(1.0f);
    }
    wavelengths.edges = {360.0f, 616.0f};
    wavelengths.cdf = {0.0f, 1.0f};
    FilmRequest req;
    req.max_width = w;
    req.max_height = h;
    req.slot_capacity = w * h * k;
    req.statistics = true;
    req.primary_aov = aov;
    req.termination_stats = term;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    ar::FilmData& d = film->data();
    d.response = response_view(response);
    d.wavelengths = {wavelengths.edges.data(), wavelengths.cdf.data(), wavelengths.bins()};
    kg.film = &d;
    kg.session.width = w;
    kg.session.height = h;
    kg.session.seed = 7u;
    kg.session.sampler_mode = ar::SAMPLER_SOBOL;
    kg.session.samples_per_pass = k;
  }

  ar::FilmData& data() { return film->data(); }
  void add(uint32_t j, uint32_t m, float v) {
    ar::Spec s;
    for (int i = 0; i < 4; ++i) s.v[i] = v / 256.0f;
    const uint32_t slot = j * kg.session.samples_per_pass + m;
    ar::sample_add(kg, slot, s);
    ar::sample_set_flags(kg, slot, 0);
  }
  void resolve() {
    film->check_resolve_fits(kg.session.samples_per_pass);
    cpu_film_resolve(kg, 0, ar::film_resolve_count(kg));
    kg.session.samples_done += kg.session.samples_per_pass;
    kg.session.pass_index += 1;
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("film_stats/sample count equals spp") {
  StatsRig rig(3, 2, 4);
  for (int pass = 0; pass < 3; ++pass) {
    for (uint32_t j = 0; j < 6; ++j)
      for (uint32_t m = 0; m < 4; ++m) rig.add(j, m, 1.0f);
    rig.resolve();
  }
  for (uint32_t p = 0; p < 6; ++p) CHECK(rig.data().n[p] == 12u);
}

TEST_CASE("film_stats/invalid counted and reported") {
  StatsRig rig(1, 1, 5);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float vals[5] = {1.0f, nan, 2.0f, nan, nan};
  for (uint32_t m = 0; m < 5; ++m) rig.add(0, m, vals[m]);
  rig.resolve();
  CHECK(rig.data().invalid[0] == 3u);
  CHECK(rig.data().n[0] == 2u);

  Report report;
  InvalidSampleReporter reporter;
  reporter.report(rig.data(), report, 1);
  reporter.report(rig.data(), report, 1);  // nothing new: still one entry, count 3
  report.snapshot();
  ar_report_info info{};
  REQUIRE(report.info(&info) == AR_OK);
  REQUIRE(info.entry_count == 1u);
  ar_report_entry e{};
  REQUIRE(report.entry(0, &e) == AR_OK);
  CHECK(e.code == uint32_t(AR_REPORT_INVALID_SAMPLE));
  CHECK(e.count == 3u);

  reporter.on_film_reset(report);
  report.snapshot();
  REQUIRE(report.info(&info) == AR_OK);
  CHECK(info.entry_count == 0u);
}

TEST_CASE("film_stats/rel error is -1 below two samples") {
  StatsRig rig(2, 1, 4);
  // Pixel 0 gets 4 samples 1..4; pixel 1 gets one valid sample and three NaN.
  for (uint32_t m = 0; m < 4; ++m) rig.add(0, m, float(m + 1));
  rig.add(1, 0, 5.0f);
  for (uint32_t m = 1; m < 4; ++m) rig.add(1, m, std::numeric_limits<float>::quiet_NaN());
  rig.resolve();
  CHECK(film_rel_error(rig.data(), 1) == -1.0);
  // Values 1..4: mean 2.5, sample variance 5/3, variance of the mean 5/12.
  CHECK(film_rel_error(rig.data(), 0) == doctest::Approx(std::sqrt(5.0 / 12.0) / 2.5).epsilon(1e-6));
}

TEST_CASE("film_stats/bounce mean and max") {
  StatsRig rig(1, 1, 3);
  const uint32_t bounces[3] = {1, 4, 2};
  for (uint32_t m = 0; m < 3; ++m) {
    rig.add(0, m, 1.0f);
    ar::film_path_end_stats(rig.kg, m, bounces[m], ar::Term::ESCAPED);
  }
  rig.resolve();
  CHECK(film_bounce_mean(rig.data(), 0) == doctest::Approx(7.0 / 3.0));
  CHECK(rig.data().count[uint64_t(rig.data().bounce_max) * rig.data().plane_stride] == 4u);
}

TEST_CASE("film_stats/material id keeps sample 0") {
  StatsRig rig(1, 1, 4, true);
  const ar::FilmData& f = rig.data();
  const ar::Real3 p{1, 2, 3}, n{0, 0, 1};
  for (uint32_t s = 0; s < 4; ++s) {
    uint32_t flags = 0;
    ar::film_primary_aov(rig.kg, s, s, &flags, p, n, 0.25f, 0.5f, 10u + s);
    CHECK((flags & ar::AOV_WRITTEN) != 0u);
    // A pass-through after the first write must not write again.
    ar::film_primary_aov(rig.kg, s, s, &flags, p, n, 0.25f, 0.5f, 99u);
    rig.add(0, s, 1.0f);
  }
  rig.resolve();
  // The readback keeps sample 0 for the id and position and averages the normal over n = 4.
  const FilmLayout& l = rig.film->layout();
  CHECK(l.find("aov_material")->sample0);
  CHECK(l.find("aov_position")->sample0);
  CHECK(!l.find("aov_normal")->sample0);
  CHECK(film_add_channel_value(l, f, "aov_material", 0, 0) == 10.0);
  CHECK(film_add_channel_value(l, f, "aov_position", 2, 0) == 3.0);
  CHECK(f.add[uint64_t(f.aov_normal + 2) * f.plane_stride] == 4.0f);  // raw sum
  CHECK(film_add_channel_value(l, f, "aov_normal", 2, 0) == doctest::Approx(1.0));
  CHECK_THROWS_AS(film_add_channel_value(l, f, "bounce_max", 0, 0), std::invalid_argument);
}

TEST_CASE("film_stats/bounce mean divides by paths ended") {
  StatsRig rig(1, 1, 3);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const uint32_t bounces[3] = {1, 4, 2};
  const float vals[3] = {1.0f, nan, 1.0f};
  for (uint32_t m = 0; m < 3; ++m) {
    rig.add(0, m, vals[m]);
    ar::film_path_end_stats(rig.kg, m, bounces[m], ar::Term::ESCAPED);
  }
  rig.resolve();
  CHECK(rig.data().n[0] == 2u);
  CHECK(film_bounce_mean(rig.data(), 0) == doctest::Approx(7.0 / 3.0));  // three paths ended, one rejected
}

TEST_CASE("film_stats/termination histogram") {
  StatsRig rig(1, 1, 4, false, true);
  const ar::Term terms[4] = {ar::Term::ESCAPED, ar::Term::ESCAPED, ar::Term::MAX_BOUNCE, ar::Term::CANCELLED};
  for (uint32_t m = 0; m < 4; ++m) ar::film_path_end_stats(rig.kg, m, 1, terms[m]);
  const ar::FilmData& f = rig.data();
  const auto at = [&](ar::Term t) { return f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride]; };
  CHECK(at(ar::Term::ESCAPED) == 2u);
  CHECK(at(ar::Term::MAX_BOUNCE) == 1u);
  CHECK(at(ar::Term::CANCELLED) == 1u);
  CHECK(at(ar::Term::ACTIVE) == 0u);
}

TEST_CASE("film_stats/rel error is the max over channels") {
  StatsRig rig(1, 1, 4);
  ar::FilmData& f = rig.data();
  f.n[0] = 4;
  const auto set = [&](uint32_t c, double s1, double s2) {
    f.s1[uint64_t(c) * f.plane_stride] = s1;
    f.s2[uint64_t(c) * f.plane_stride] = s2;
  };
  // Samples {1,1,1,3}: mean 1.5, variance of the mean (12 - 36 / 4) / 12 = 0.25, rel 0.5 / 1.5.
  // Samples {1,1,1,1}: zero. Samples {1,2,3,4}: sqrt(5/12) / 2.5, smaller than the first.
  set(0, 6.0, 12.0);
  set(1, 4.0, 4.0);
  set(2, 10.0, 30.0);
  CHECK(film_rel_error(f, 0) == doctest::Approx(0.5 / 1.5).epsilon(1e-9));
}

TEST_CASE("film_stats/rel error floors the mean at 1e-6") {
  StatsRig rig(1, 1, 4);
  ar::FilmData& f = rig.data();
  f.n[0] = 4;
  const auto set = [&](uint32_t c, double s1, double s2) {
    f.s1[uint64_t(c) * f.plane_stride] = s1;
    f.s2[uint64_t(c) * f.plane_stride] = s2;
  };
  // Samples {-1,1,-1,1}: mean 0, variance of the mean 4 / (4 * 3) = 1/3, divided by the floor.
  set(0, 4.0, 4.0);
  set(1, 0.0, 4.0);
  set(2, 4.0, 4.0);
  CHECK(film_rel_error(f, 0) == doctest::Approx(std::sqrt(1.0 / 3.0) / 1e-6).epsilon(1e-9));
}

TEST_CASE("film_stats/delta reporting keeps one cumulative entry") {
  StatsRig rig(1, 1, 5);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Report report;
  InvalidSampleReporter reporter;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t m = 0; m < 5; ++m) rig.add(0, m, m < (pass == 0 ? 3u : 2u) ? nan : 1.0f);
    rig.resolve();
    reporter.report(rig.data(), report, 1 + pass);
  }
  report.snapshot();
  ar_report_info info{};
  REQUIRE(report.info(&info) == AR_OK);
  REQUIRE(info.entry_count == 1u);
  ar_report_entry e{};
  REQUIRE(report.entry(0, &e) == AR_OK);
  CHECK(e.count == 5u);

  // A clear without on_film_reset is a pairing bug and must not pass silently.
  rig.film->clear();
  CHECK_THROWS_AS(reporter.report(rig.data(), report, 3), std::logic_error);
}

}  // TEST_SUITE
