// SPDX-License-Identifier: Apache-2.0
// film layout, sample_add and film_resolve (0008 d1-d4, d6, V2) through the CPU kernel on a film
// of a few pixels. Expected values are closed forms, never read back from the code under test:
// with the flat tables below a slot whose four lanes hold v / 256 converts to exactly x = v
// (the wavelength pdf is 1/256, so film_weights is 64 per lane and 4 * (v / 256) * 64 = v),
// and v / 256 is exact in float, so S1 and S2 are sums of known doubles.
// Ref: docs/decisions/0008-film-outputs-denoise.md d1-d4, d6, V2; Chan, Golub and LeVeque 1983
// for the variance of the mean.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/film/film_layout.h"
#include "host/film/response_table.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"
#include "film/resolve.h"
#include "stages/film_build_active.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Flat: R = 1 everywhere and a single-bin wavelength table, so a slot of v / 256 gives x = v.
// Varied: ramps and a two-bin table, for tests that compare two schedules of the same samples.
struct Tables {
  ResponseTable response;
  WavelengthTable wavelengths;
  explicit Tables(bool flat) {
    ResponseTable& t = response;
    t.display_first = 0;
    t.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    t.y_w = flat ? 1.0 : 0.8;
    const double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double mv[9] = {1, 0.1, 0, 0, 1, 0.2, 0.1, 0, 1};
    for (int i = 0; i < 9; ++i) t.m_ws[i] = flat ? m[i] : mv[i];
    // Channels 0-2 display, 3 a band (photon units when varied).
    for (uint32_t c = 0; c < 4; ++c) {
      t.channels.push_back({2 * c, 2, (!flat && c == 3) ? ar::RESPONSE_PHOTON : ar::RESPONSE_ENERGY, 0});
      t.knot_nm.push_back(300.0f);
      t.knot_nm.push_back(900.0f);
      t.knot_value.push_back(1.0f);
      t.knot_value.push_back(flat ? 1.0f : 1.0f + 0.25f * float(c + 1));
    }
    if (flat) {
      wavelengths.edges = {360.0f, 616.0f};
      wavelengths.cdf = {0.0f, 1.0f};
    } else {
      wavelengths.edges = {360.0f, 500.0f, 700.0f};
      wavelengths.cdf = {0.0f, 0.25f, 1.0f};
    }
  }
};

struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  Tables tables;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};
  // The pass's active-pixel list, built the way the session does at pass start (0008 d3).
  std::vector<uint32_t> list;
  void build_list() {
    list.resize(film->data().plane_stride);
    kg.active_pixels = list.data();
    cpu_film_build_active(kg, 0, ar::film_build_active_count(film->data()));
  }

  Rig(const FilmRequest& req, bool flat = true) : pool(ctx, 1), dev(ledger, pool), tables(flat) {
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    ar::FilmData& d = film->data();
    d.response = response_view(tables.response);
    d.wavelengths = {tables.wavelengths.edges.data(), tables.wavelengths.cdf.data(), tables.wavelengths.bins()};
    kg.film = &d;
    kg.session.width = req.max_width;
    kg.session.height = req.max_height;
    kg.session.seed = 7u;
    kg.session.sampler_mode = ar::SAMPLER_SOBOL;
    build_list();
  }

  ar::FilmData& data() { return film->data(); }
  uint32_t pixels() const { return film->data().rect_width * film->data().rect_height; }
  void begin_pass(uint32_t k) { kg.session.samples_per_pass = k; }
  // Slot j * k + m of the pass (0008 d3).
  void add(uint32_t j, uint32_t m, const ar::Spec& L, uint32_t flags = 0) {
    const uint32_t slot = j * kg.session.samples_per_pass + m;
    ar::sample_add(kg, slot, L);
    ar::sample_set_flags(kg, slot, flags);
  }
  void resolve() {
    film->check_resolve_fits(kg.session.samples_per_pass);
    build_list();
    cpu_film_resolve(kg, 0, ar::film_resolve_count(kg));
    kg.session.samples_done += kg.session.samples_per_pass;
    kg.session.pass_index += 1;
  }
};

ar::Spec lanes(float v) {
  ar::Spec s;
  for (int i = 0; i < 4; ++i) s.v[i] = v / 256.0f;
  return s;
}

FilmRequest one_pixel(uint32_t slots) {
  FilmRequest r;
  r.max_width = r.max_height = 1;
  r.slot_capacity = slots;
  return r;
}

struct SplitMix {
  uint64_t x;
  double next() {  // uniform in [0, 1)
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return double((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);
  }
};

// Feeds `total` samples of value(i) to a 1x1 film in passes of k and returns S1, S2, n.
template <class F>
void stream(Rig& rig, uint64_t total, uint32_t k, F value) {
  rig.begin_pass(k);
  uint64_t i = 0;
  while (i < total) {
    for (uint32_t m = 0; m < k; ++m) rig.add(0, m, lanes(value(i++)));
    rig.resolve();
  }
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("film_resolve/layout allocates only requested channels") {
  FilmRequest r;
  r.max_width = 8, r.max_height = 4;
  const FilmLayout base = FilmLayout::build(r);
  CHECK(base.offset("display") == 0);
  CHECK(base.offset("splat") == -1);
  CHECK(base.offset("band") == -1);
  CHECK(base.moment_planes() == 3u);
  CHECK(base.splat_components() == 0u);
  CHECK(!base.adaptive());

  FilmRequest full = r;
  full.adaptive_sampling = true;
  full.light_pass = true;
  full.extra = {{"band", ChannelKind::Moment, 1, 3}, {"depth", ChannelKind::Add, 1, 0}, {"hist", ChannelKind::Count, 2, 0}};
  const FilmLayout l = FilmLayout::build(full);
  CHECK(l.offset("band") == 3);
  CHECK(l.offset("depth") == 0);
  CHECK(l.offset("hist") == 0);
  CHECK(l.offset("splat") == 0);
  CHECK(l.moment_planes() == 4u);
  CHECK(l.count_planes() == 4u);  // hist (2) plus the statistics bounce_max and bounce_paths
  // A requested channel costs memory; an unrequested one costs none.
  CHECK(l.bytes() > base.bytes());

  // Rows are bottom-up and the allocation is at the largest size: the sub-rectangle is a view.
  CHECK(base.bind(nullptr).plane_stride == 32u);
  CHECK(base.bind(nullptr).s1a == nullptr);
  CHECK(base.bind(nullptr).splat == nullptr);

  CHECK_THROWS_AS(FilmLayout::build({0, 4}), std::invalid_argument);
  FilmRequest dup = r;
  dup.extra = {{"display", ChannelKind::Add, 1, 0}};
  CHECK_THROWS_AS(FilmLayout::build(dup), std::invalid_argument);
  FilmRequest crypto = r;
  crypto.extra = {{"id", ChannelKind::Crypto, 2, 0}};
  CHECK_THROWS_AS(FilmLayout::build(crypto), std::invalid_argument);
  FilmRequest bad = r;
  bad.extra = {{"band", ChannelKind::Moment, 3, 3}};
  CHECK_THROWS_AS(FilmLayout::build(bad), std::invalid_argument);

  // The bytes are the film category of the ledger, and a smaller render size is only a view.
  Ledger ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice dev(ledger, pool);
  {
    Film film(dev, l, Owner{1, 1});
    CHECK(ledger.category_total(LedgerCategory::FILM) == l.bytes());
    film.set_render_size(4, 2);
    CHECK(ledger.category_total(LedgerCategory::FILM) == l.bytes());
    CHECK_THROWS_AS(film.set_render_size(9, 2), std::invalid_argument);
  }
  CHECK(ledger.total() == 0);
}

TEST_CASE("film_resolve/nan skipped and counted") {
  Rig rig(one_pixel(5));
  rig.begin_pass(5);
  ar::Spec one_lane_nan = lanes(1.0f), negative = lanes(1.0f);
  one_lane_nan.v[2] = kNaN;
  negative.v[0] = -0.5f;
  rig.add(0, 0, lanes(1.0f));
  rig.add(0, 1, one_lane_nan);
  rig.add(0, 2, lanes(1.0f));
  rig.add(0, 3, negative);
  rig.add(0, 4, lanes(kInf * 256.0f));
  rig.resolve();
  const ar::FilmData& d = rig.data();
  CHECK(d.n[0] == 2u);
  CHECK(d.invalid[0] == 3u);
  CHECK(d.s1[0] == 2.0);  // three components are all 1 per valid sample
  CHECK(d.s1[d.plane_stride] == 2.0);
  CHECK(d.s2[0] == 2.0);
  // The slots are clean for the next pass, bad samples included.
  for (int i = 0; i < 5 * 4; ++i) CHECK(d.slot_L[i] == 0.0f);
  for (int i = 0; i < 5; ++i) CHECK(d.slot_flags[i] == 0u);
}

TEST_CASE("film_resolve/splat folded once") {
  FilmRequest r;
  r.max_width = r.max_height = 4;
  r.light_pass = true;
  r.adaptive_sampling = true;
  Rig rig(r);
  rig.begin_pass(1);
  const ar::FilmData& d = rig.data();
  // Camera samples are valid zeros, so the moments hold only the light image.
  const uint32_t px[3] = {0, 5, 15};
  const float a[3] = {0.5f, 2.0f, 0.25f};
  const auto camera_zero = [&] {
    for (uint32_t j = 0; j < 16; ++j) rig.add(j, 0, lanes(0.0f));
  };
  // Pass 0 (even): two taps on pixel 0 add up inside the plane before the fold.
  camera_zero();
  ar::film_splat_add(rig.kg, px[0], ar::Real3{0.25f, 0.125f, 0.0625f});
  ar::film_splat_add(rig.kg, px[0], ar::Real3{0.25f, 0.125f, 0.0625f});
  ar::film_splat_add(rig.kg, px[1], ar::Real3{a[1], a[1], a[1]});
  ar::film_splat_add(rig.kg, px[2], ar::Real3{a[2], 0.0f, 0.0f});
  rig.resolve();
  CHECK(d.s1[px[0]] == 0.5);
  CHECK(d.s1[d.plane_stride + px[0]] == 0.25);
  CHECK(d.s1[2 * d.plane_stride + px[0]] == 0.125);
  CHECK(d.s2[px[0]] == 0.25);
  CHECK(d.s1a[px[0]] == 0.5);
  CHECK(d.s1[px[1]] == 2.0);
  CHECK(d.s2[px[1]] == 4.0);
  CHECK(d.s1[px[2]] == 0.25);
  CHECK(d.s1[1] == 0.0);
  for (uint32_t i = 0; i < 3 * 16; ++i) CHECK(d.splat[i] == 0.0f);  // cleared
  for (uint32_t j = 0; j < 16; ++j) CHECK(d.n[j] == 1u);           // a splat is not a camera sample

  // Pass 1 (odd) with no splats changes nothing: the plane was cleared, nothing folds twice.
  camera_zero();
  rig.resolve();
  CHECK(d.s1[px[0]] == 0.5);
  CHECK(d.s2[px[0]] == 0.25);

  // Pass 2 (even) again: the class-A sum follows the pass parity, S1 and S2 follow every pass.
  camera_zero();
  ar::film_splat_add(rig.kg, px[0], ar::Real3{0.5f, 0.0f, 0.0f});
  rig.resolve();
  CHECK(d.s1[px[0]] == 1.0);
  CHECK(d.s2[px[0]] == 0.5);
  CHECK(d.s1a[px[0]] == 1.0);
  // Pass 3 (odd): S1 grows, S1A does not.
  camera_zero();
  ar::film_splat_add(rig.kg, px[0], ar::Real3{0.5f, 0.0f, 0.0f});
  rig.resolve();
  CHECK(d.s1[px[0]] == 1.5);
  CHECK(d.s1a[px[0]] == 1.0);
}

TEST_CASE("film_resolve/tail pass equals single sample passes") {
  FilmRequest r;
  r.max_width = r.max_height = 2;
  r.slot_capacity = 16;
  r.adaptive_sampling = true;
  r.extra = {{"band", ChannelKind::Moment, 1, 3}};
  SplitMix rng{11};
  // Value of sample index s of pixel j: a fixed table, with one terminated-secondaries sample.
  ar::Spec table[4][4];
  uint32_t flags[4][4];
  for (int j = 0; j < 4; ++j)
    for (int s = 0; s < 4; ++s) {
      for (int i = 0; i < 4; ++i) table[j][s].v[i] = float(rng.next());
      flags[j][s] = (j == 1 && s == 2) ? ar::SECONDARIES_TERMINATED : 0u;
    }
  Rig tail(r, false), single(r, false);
  tail.begin_pass(4);
  for (uint32_t j = 0; j < 4; ++j)
    for (uint32_t m = 0; m < 4; ++m) tail.add(j, m, table[j][m], flags[j][m]);
  tail.resolve();
  for (uint32_t s = 0; s < 4; ++s) {
    single.begin_pass(1);
    for (uint32_t j = 0; j < 4; ++j) single.add(j, 0, table[j][s], flags[j][s]);
    single.resolve();
  }
  const FilmLayout& l = tail.film->layout();
  const uint32_t stride = tail.data().plane_stride;
  for (uint32_t pl = 0; pl < l.moment_planes(); ++pl)
    for (uint32_t p = 0; p < 4; ++p) {
      const uint64_t at = uint64_t(pl) * stride + p;
      CHECK(tail.data().s1[at] == single.data().s1[at]);
      CHECK(tail.data().s2[at] == single.data().s2[at]);
      CHECK(tail.data().s1a[at] == single.data().s1a[at]);
    }
  for (uint32_t p = 0; p < 4; ++p) {
    CHECK(tail.data().n[p] == 4u);
    CHECK(single.data().n[p] == 4u);
  }
  CHECK(tail.data().s1[0] > 0.0);
  // The terminated sample really weighs differently, so the table above is a real test.
  CHECK(tail.data().s1[1] != tail.data().s1[0]);
}

TEST_CASE("film_resolve/cpu single thread twice bit-identical") {
  FilmRequest r;
  r.max_width = 3, r.max_height = 2;
  r.slot_capacity = 12;
  r.adaptive_sampling = true;
  r.light_pass = true;
  std::vector<uint8_t> dumps[2];
  for (auto& dump : dumps) {
    Rig rig(r, false);
    SplitMix rng{5};
    rig.begin_pass(2);
    for (int pass = 0; pass < 6; ++pass) {
      for (uint32_t j = 0; j < 6; ++j)
        for (uint32_t m = 0; m < 2; ++m) {
          ar::Spec L;
          for (int i = 0; i < 4; ++i) L.v[i] = float(rng.next() * 3.0);
          rig.add(j, m, L);
        }
      ar::film_splat_add(rig.kg, uint32_t(pass), ar::Real3{float(rng.next()), float(rng.next()), float(rng.next())});
      rig.resolve();
    }
    dump.resize(rig.film->buffer().bytes);
    rig.dev.download(dump.data(), rig.film->buffer(), 0, dump.size());
  }
  REQUIRE(dumps[0].size() == dumps[1].size());
  CHECK(std::memcmp(dumps[0].data(), dumps[1].data(), dumps[0].size()) == 0);
}

TEST_CASE("film_resolve/radiance scale divides camera and splat") {
  // s = 4 (0003 d10): the host folded s into every emitter, so camera samples and light-tracing
  // splats both come back divided by it, in every moment plane.
  FilmRequest r;
  r.max_width = 2, r.max_height = 1;
  r.light_pass = true;
  r.adaptive_sampling = true;
  Rig rig(r);
  rig.data().inv_radiance_scale = 0.25;
  rig.begin_pass(1);
  rig.add(0, 0, lanes(2.0f));
  rig.add(1, 0, lanes(0.0f));
  ar::film_splat_add(rig.kg, 0, ar::Real3{1.0f, 1.0f, 1.0f});
  ar::film_splat_add(rig.kg, 1, ar::Real3{2.0f, 4.0f, 8.0f});
  rig.resolve();
  const ar::FilmData& d = rig.data();
  CHECK(d.s1[0] == 0.5 + 0.25);
  CHECK(d.s2[0] == 0.25 + 0.0625);
  CHECK(d.s1a[0] == 0.5 + 0.25);  // sample index 0 and pass 0 are both even
  CHECK(d.s1[1] == 0.5);
  CHECK(d.s1[d.plane_stride + 1] == 1.0);
  CHECK(d.s1[2 * d.plane_stride + 1] == 2.0);
  CHECK(d.s2[2 * d.plane_stride + 1] == 4.0);
}

TEST_CASE("film_resolve/bad splat dropped and counted") {
  FilmRequest r;
  r.max_width = 4, r.max_height = 1;
  r.light_pass = true;
  Rig rig(r);
  rig.begin_pass(1);
  for (uint32_t j = 0; j < 4; ++j) rig.add(j, 0, lanes(0.0f));
  ar::film_splat_add(rig.kg, 0, ar::Real3{1.0f, kNaN, 1.0f});
  ar::film_splat_add(rig.kg, 1, ar::Real3{1.0f, 1.0f, -0.5f});
  ar::film_splat_add(rig.kg, 2, ar::Real3{kInf, 1.0f, 1.0f});
  ar::film_splat_add(rig.kg, 3, ar::Real3{0.5f, 0.25f, 0.125f});
  rig.resolve();
  const ar::FilmData& d = rig.data();
  // A negative component is an out-of-gamut colour, not an invalid sample: folded normally.
  CHECK(d.invalid[1] == 0u);
  CHECK(d.s1[2 * d.plane_stride + 1] == -0.5);
  CHECK(d.s2[2 * d.plane_stride + 1] == 0.25);
  CHECK(d.s1[1] == 1.0);
  for (const uint32_t j : {0u, 2u}) {
    INFO("pixel " << j);
    CHECK(d.invalid[j] == 1u);
    for (uint32_t q = 0; q < 3; ++q) {
      CHECK(d.s1[q * d.plane_stride + j] == 0.0);
      CHECK(d.s2[q * d.plane_stride + j] == 0.0);
    }
  }
  CHECK(d.invalid[3] == 0u);
  CHECK(d.s1[3] == 0.5);
  CHECK(d.s1[d.plane_stride + 3] == 0.25);
  // The whole plane is clear, the dropped entries included.
  for (uint32_t i = 0; i < 3 * 4; ++i) CHECK(d.splat[i] == 0.0f);
}

TEST_CASE("film_resolve/rows are bottom first and the render rectangle is a view") {
  // 4x3 allocation, 3x2 rectangle: pixel (x, y) of the rectangle is plane index y * 4 + x, row 0
  // (the first plane row) is the bottom row of the image (0008 d1). Distinct value per pixel.
  FilmRequest r;
  r.max_width = 4, r.max_height = 3;
  Rig rig(r);
  rig.film->set_render_size(3, 2);
  rig.begin_pass(1);
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 3; ++x) rig.add(y * 3 + x, 0, lanes(1.0f + float(x) + 10.0f * float(y)));
  rig.resolve();
  const ar::FilmData& d = rig.data();
  for (uint32_t y = 0; y < 3; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      INFO("x " << x << " y " << y);
      const bool inside = x < 3 && y < 2;
      CHECK(d.s1[y * 4 + x] == (inside ? 1.0 + x + 10.0 * y : 0.0));
      CHECK(d.n[y * 4 + x] == (inside ? 1u : 0u));
    }
  // The pass would overflow the slots by one sample per pixel.
  CHECK_THROWS_AS(rig.film->check_resolve_fits(rig.film->data().slot_capacity / 6 + 1), std::length_error);
  CHECK_NOTHROW(rig.film->check_resolve_fits(2));
}

TEST_CASE("film_resolve/sub rectangle uses the allocated width in the sample key") {
  // The camera keys a pixel with pixel_id at the allocated size (sample/keys.h), so the resolve
  // must recompute the same wavelengths: expected values below use that id, with wavelengths
  // that differ per pixel, and a rectangle narrower than the allocation.
  FilmRequest r;
  r.max_width = 4, r.max_height = 3;
  r.slot_capacity = 6;
  r.extra = {{"band", ChannelKind::Moment, 1, 3}};
  Rig rig(r, false);
  rig.film->set_render_size(3, 2);
  rig.begin_pass(1);
  rig.kg.session.samples_done = 5;  // an arbitrary sample index
  ar::Spec L;
  for (int i = 0; i < 4; ++i) L.v[i] = 0.5f + 0.25f * float(i);
  for (uint32_t j = 0; j < 6; ++j) rig.add(j, 0, L);
  rig.resolve();
  const ar::FilmData& d = rig.data();
  const ar::WavelengthTableView wt = d.wavelengths;
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 3; ++x) {
      const ar::Real u = ar::draw(ar::SAMPLER_SOBOL, ar::pixel_id(x, y, 4), 5u, 0u, ar::DIM_CAM_WAVELENGTH, 7u);
      const ar::FilmWavelengths fw = ar::sample_film_wavelengths(wt, u);
      const double expected = ar::convert(d.response, L, fw.lf, ar::film_weights(0u, fw.pf), 3);
      INFO("x " << x << " y " << y);
      CHECK(std::fabs(d.s1[3 * d.plane_stride + y * 4 + x] - expected) <= 1e-12 * std::fabs(expected));
    }
}

}  // TEST_SUITE

TEST_SUITE("gate") {

TEST_CASE("film_resolve/3e7 ones exact" * doctest::test_suite("full")) {
  // float32 stalls at 2^24 = 16,777,216; the fp64 moments do not.
  Rig rig(one_pixel(3000));
  stream(rig, 30000000ull, 3000, [](uint64_t) { return 1.0f; });
  const ar::FilmData& d = rig.data();
  CHECK(d.n[0] == 30000000u);
  CHECK(d.invalid[0] == 0u);
  CHECK(d.s1[0] == 3.0e7);
  CHECK(d.s2[0] == 3.0e7);
  CHECK(d.s1[d.plane_stride] == 3.0e7);
  CHECK(d.s1[2 * d.plane_stride] == 3.0e7);
}

TEST_CASE("film_resolve/2^24 samples of 0.1" * doctest::test_suite("full")) {
  Rig rig(one_pixel(4096));
  const uint64_t total = 1ull << 24;
  stream(rig, total, 4096, [](uint64_t) { return 0.1f; });
  const ar::FilmData& d = rig.data();
  const double expected = double(total) * 0.1;
  CHECK(d.n[0] == uint32_t(total));
  CHECK(std::fabs(d.s1[0] - expected) / expected < 1e-6);
}

namespace {

// Variance of the mean from the film against the closed form sigma^2 / n, inside the 99.9%
// interval of the sample variance, whose relative standard deviation is sqrt((kurtosis - 1) / n),
// and against a two-pass double sum of the very same float samples (the rounding of v to float
// is then no part of the comparison).
template <class Draw>
void check_variance(double mean, double variance, double kurtosis, Draw draw) {
  const uint32_t n = 1u << 17;
  Rig rig(one_pixel(4096));
  std::vector<float> xs(n);
  SplitMix rng{20260101};
  for (float& x : xs) x = float(draw(rng.next()));
  stream(rig, n, 4096, [&](uint64_t i) { return xs[i]; });
  const ar::FilmData& d = rig.data();
  REQUIRE(d.n[0] == n);

  double sum = 0.0;
  for (float x : xs) sum += double(x);
  const double m = sum / n;
  double ss = 0.0;
  for (float x : xs) ss += (double(x) - m) * (double(x) - m);
  const double reference = ss / (double(n) - 1.0) / n;
  const double estimate = moment_variance_of_mean(d.s1[0], d.s2[0], d.n[0]);
  CHECK(std::fabs(estimate - reference) / reference < 1e-6);

  const double truth = variance / n;
  const double rel_sd = std::sqrt((kurtosis - 1.0) / n);
  CHECK(std::fabs(estimate / truth - 1.0) < 3.29 * rel_sd);
  CHECK(std::fabs(moment_mean(d.s1[0], d.n[0]) - mean) < 3.29 * std::sqrt(truth));
}

}  // namespace

TEST_CASE("film_resolve/variance of uniform samples") {
  check_variance(0.5, 1.0 / 12.0, 1.8, [](double u) { return u; });
}

TEST_CASE("film_resolve/variance of exponential samples") {
  check_variance(1.0, 1.0, 9.0, [](double u) { return -std::log1p(-u); });
}

}  // TEST_SUITE
