// SPDX-License-Identifier: Apache-2.0
// generate_camera and film_build_active (0008 d3, 0003 d2, d19, d23) through the CPU kernels on a
// 7x5 render rectangle inside an 8x6 film, three passes of k = 2. Expectations are closed forms
// (slot = j * k + m, plane index y * 8 + x, tan(fov / 2) of the camera built here, the bin pdf
// read from the table) or inverse maps (camera_we gives the film pixel a ray crosses), never the
// stored result itself.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3 (slot assignment); 0003 d2 (film
// wavelengths from the key), d6 (hero lane), d19, d21, d23, check 26 (first half); 0002 D7 step 4;
// Akenine-Moller et al. 2021 (ray-cone spread).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "device/cpu/cpu_device.h"
#include "host/film/active_pixels.h"
#include "host/film/film_layout.h"
#include "host/film/filter_table.h"
#include "host/film/response_table.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"
#include "film/resolve.h"
#include "stages/film_build_active.h"
#include "stages/generate_camera.h"

using namespace astroray;
using namespace astroray::host;

namespace ar {
// Visits the arrays of the always-present path fields; the others stay null (no feature is on).
template <class F>
void for_each_always_field(PathStateSoA& soa, F&& fn) {
#define AR_PATH_FIELD(n, t, f) \
  if constexpr (StateFeature::f == StateFeature::ALWAYS) fn(sizeof(t), reinterpret_cast<void**>(&soa.n));
#include "state/path_state.def"
#undef AR_PATH_FIELD
}
}  // namespace ar

namespace {

constexpr uint32_t kFilmW = 8, kFilmH = 6, kRectW = 7, kRectH = 5, kPixels = kRectW * kRectH;
constexpr uint32_t kK = 2, kPasses = 3, kSlots = kPixels * kK;
constexpr double kTx = 0.5, kTy = 0.375;  // half extents of the film plane at z = -1

// A camera turned about two axes so that no axis is special: columns right, up, back.
ar::CameraView make_camera() {
  const double a = 0.6, b = -0.4;
  const double cz = std::cos(a), sz = std::sin(a), cx = std::cos(b), sx = std::sin(b);
  const double rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
  const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
  double r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = rz[i][0] * rx[0][j] + rz[i][1] * rx[1][j] + rz[i][2] * rx[2][j];
  ar::CameraView c{};
  c.type = ar::CameraType::PERSPECTIVE;
  c.origin = {1.5f, -2.0f, 0.75f};
  for (int j = 0; j < 3; ++j) c.axis[j] = {float(r[0][j]), float(r[1][j]), float(r[2][j])};
  c.film_origin = {float(-kTx), float(-kTy)};
  c.film_extent = {float(2 * kTx), float(2 * kTy)};
  c.film_area = float(4 * kTx * kTy);
  c.width = float(kRectW);
  c.height = float(kRectH);
  return c;
}

struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  WavelengthTable wavelengths = WavelengthTableBuilder().build();
  ResponseTable response;
  std::unique_ptr<Film> film;
  std::unique_ptr<ActivePixels> active;
  std::unique_ptr<ar::FilterTable> filter = std::make_unique<ar::FilterTable>(build_filter_table({}));
  ar::CameraView camera = make_camera();
  ar::PathStateSoA path{};
  std::vector<std::vector<unsigned char>> arrays;
  std::vector<uint32_t> queue;
  ar::StepTable table{};
  ar::KernelGlobals kg{};

  Rig() : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = kFilmW;
    req.max_height = kFilmH;
    req.slot_capacity = kSlots;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->set_render_size(kRectW, kRectH);
    film->data().wavelengths = {wavelengths.edges.data(), wavelengths.cdf.data(), wavelengths.bins()};
    // A response that varies with wavelength, so a wrong film wavelength changes the resolved value:
    // channels 0-2 are the display group, ramps over 300-900 nm.
    response.display_first = 0;
    response.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    response.y_w = 0.8;
    const double m[9] = {1, 0.1, 0, 0, 1, 0.2, 0.1, 0, 1};
    for (int i = 0; i < 9; ++i) response.m_ws[i] = m[i];
    for (uint32_t c = 0; c < 3; ++c) {
      response.channels.push_back({2 * c, 2, ar::RESPONSE_ENERGY, 0});
      response.knot_nm.push_back(300.0f);
      response.knot_nm.push_back(900.0f);
      response.knot_value.push_back(1.0f);
      response.knot_value.push_back(1.0f + 0.5f * float(c + 1));
    }
    film->data().response = response_view(response);
    film->data().film_exposure = 1.0;
    film->data().inv_radiance_scale = 1.0;
    active = std::make_unique<ActivePixels>(dev, kFilmW, kFilmH, Owner{1, 1});
    ar::for_each_always_field(path, [&](size_t bytes, void** slot) {
      arrays.emplace_back(bytes * kSlots, 0xAB);  // poison: a field generate_camera forgets stays 0xAB
      *slot = arrays.back().data();
    });
    // Free-slot entries are not in slot order, as after a partial refill: item i goes to a slot
    // that is a bijection of i.
    for (uint32_t i = 0; i < kSlots; ++i) queue.push_back((i * 3 + 5) % kSlots);
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.film = &film->data();
    kg.camera = &camera;
    kg.filter = filter.get();
    kg.step_table = &table;
    kg.active_pixels = active->list();
    kg.session.width = kFilmW;
    kg.session.height = kFilmH;
    kg.session.seed = 0x1234u;
    kg.session.sampler_mode = ar::SAMPLER_SOBOL;
  }

  // One pass: list built, then the refill in two launches (the second continues the sample
  // numbers where the first stopped, as the scheduler's overwritten step entry does).
  void run_pass(uint32_t n, uint32_t k) {
    kg.session.samples_done = n;
    kg.session.samples_per_pass = k;
    const uint32_t u = active->plan(film->data());
    cpu_film_build_active(kg, 0, u);
    const uint32_t items = u * k, split = items / 3;
    cpu_stage_generate_camera(kg, ar::StepEntry{0, split, 0}, 0, split);
    cpu_stage_generate_camera(kg, ar::StepEntry{split, items - split, split}, 0, items - split);
  }
};

// The film position a camera ray crosses: the inverse of the camera map, independent of how the
// ray was made. Not clipped to the film: a filter offset may cross the film edge.
bool ray_pixel(const Rig& r, uint32_t slot, ar::Real2* film) {
  const ar::Real3 o = r.path.ray_o[slot], d = r.path.ray_d[slot];
  CHECK(o.x == r.camera.origin.x);
  CHECK(o.y == r.camera.origin.y);
  CHECK(o.z == r.camera.origin.z);
  const ar::Real3 v = ar::camera_from_world_dir(r.camera, d);
  if (!(v.z < 0.0f)) return false;
  film->x = ((v.x / -v.z - r.camera.film_origin.x) / r.camera.film_extent.x) * r.camera.width;
  film->y = ((v.y / -v.z - r.camera.film_origin.y) / r.camera.film_extent.y) * r.camera.height;
  return true;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("generate_camera/slot formula and pixel coverage") {
  Rig r;
  r.kg.session.test_hooks = ar::TEST_HOOK_PIXEL_CENTRE;  // the ray is then exactly through the centre
  const ar::WavelengthTableView& t = r.film->data().wavelengths;
  // Lane-0 film wavelength of every (pixel, sample index) of the epoch, to read a slot's sample
  // index back from what generate_camera stored. A collision would make the lookup ambiguous.
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> by_lambda;
  for (uint32_t y = 0; y < kRectH; ++y)
    for (uint32_t x = 0; x < kRectW; ++x)
      for (uint32_t idx = 0; idx < kPasses * kK; ++idx) {
        const uint32_t pixel = y * kFilmW + x;
        const ar::Real u = ar::draw(ar::SAMPLER_SOBOL, pixel, idx, 0u, ar::DIM_CAM_WAVELENGTH, r.kg.session.seed);
        float l0 = ar::sample_film_wavelengths(t, u).lf.nm[0];
        uint32_t bits;
        std::memcpy(&bits, &l0, sizeof bits);
        by_lambda[bits] = {pixel, idx};
      }
  REQUIRE(by_lambda.size() == size_t(kPixels) * kPasses * kK);

  std::set<std::pair<uint32_t, uint32_t>> seen;  // observed (plane index, sample index)
  for (uint32_t pass = 0; pass < kPasses; ++pass) {
    r.run_pass(pass * kK, kK);
    for (uint32_t sample = 0; sample < kSlots; ++sample) {
      const uint32_t slot = r.queue[sample];
      CHECK(r.path.sample_id[slot] == sample);
      // slot = j * k + m: the pixel the ray crosses is the j-th of the render rectangle in row order.
      const uint32_t j = sample / kK, m = sample % kK;
      ar::Real2 film{};
      REQUIRE(ray_pixel(r, slot, &film));
      CHECK(film.x == doctest::Approx(j % kRectW + 0.5).epsilon(1e-4));
      CHECK(film.y == doctest::Approx(j / kRectW + 0.5).epsilon(1e-4));
      const uint32_t pixel = uint32_t(film.y) * kFilmW + uint32_t(film.x);
      float l0 = r.path.lambda_t[slot].v[0];
      uint32_t bits;
      std::memcpy(&bits, &l0, sizeof bits);
      const auto it = by_lambda.find(bits);
      REQUIRE(it != by_lambda.end());
      CHECK(it->second.first == pixel);
      CHECK(it->second.second == pass * kK + m);  // n + m
      seen.insert(it->second);
    }
  }
  CHECK(seen.size() == size_t(kPixels) * kPasses * kK);  // every (pixel, index) exactly once
}

TEST_CASE("generate_camera/active pixel list") {
  Rig r;
  r.run_pass(0, 1);
  CHECK(r.active->capacity() == kFilmW * kFilmH);
  CHECK(r.active->count() == kPixels);
  const uint32_t* list = r.active->list();
  for (uint32_t j = 0; j < kPixels; ++j) CHECK(list[j] == (j / kRectW) * kFilmW + j % kRectW);
  CHECK(r.ledger.category_total(LedgerCategory::FILM) >= uint64_t(kFilmW) * kFilmH * sizeof(uint32_t));
  // A render rectangle beyond the list's capacity is refused, never truncated.
  r.film->data().rect_width = 2 * kFilmW;
  CHECK_THROWS_AS(r.active->plan(r.film->data()), std::length_error);
}

TEST_CASE("generate_camera/resolve recomputes same wavelengths") {
  // 0003 check 26, first half: the film_resolve kernel itself, run on slots whose values are known,
  // must convert at the wavelengths generate_camera started the paths with. The expected sums use
  // the stored lambda_t and the table's own bin pdfs, so a wrong dimension, or the pass-local m
  // instead of n + m, in either kernel changes S1.
  Rig r;
  const ar::FilmData& f = r.film->data();
  const uint32_t n = 4;
  r.run_pass(n, kK);  // n = 4: a resolve that used m would draw other wavelengths
  const ar::WavelengthTableView& t = f.wavelengths;
  std::vector<double> expect(size_t(3) * kFilmW * kFilmH, 0.0);
  for (uint32_t sample = 0; sample < kSlots; ++sample) {
    ar::Spec L;
    for (int i = 0; i < ar::kLanes; ++i) {
      L.v[i] = 0.25f + 0.0625f * float(i) + 0.015625f * float(sample % 13);
      f.slot_L[4u * sample + uint32_t(i)] = L.v[i];
    }
    f.slot_flags[sample] = 0u;
    const uint32_t slot = r.queue[sample];
    ar::Lambda lf;
    ar::Spec pf;
    for (int i = 0; i < ar::kLanes; ++i) {
      lf.nm[i] = r.path.lambda_t[slot].v[i];  // lambda_t starts as lambda_f
      uint32_t b = 0;
      while (b + 1 < t.bins && t.edges[b + 1] <= lf.nm[i]) ++b;
      pf.v[i] = (t.cdf[b + 1] - t.cdf[b]) / (t.edges[b + 1] - t.edges[b]);
    }
    const ar::Spec w = ar::film_weights(0u, pf);
    const ar::DisplayRgb rgb = ar::display_rgb(f.response, L, lf, w, f.film_exposure);
    const uint32_t j = sample / kK;
    const uint32_t pixel = (j / kRectW) * kFilmW + j % kRectW;
    for (int c = 0; c < 3; ++c) expect[size_t(c) * f.plane_stride + pixel] += rgb.v[c];
  }
  r.film->check_resolve_fits(kK);
  cpu_film_resolve(r.kg, 0, ar::film_resolve_count(r.kg));
  uint32_t checked = 0;
  for (uint32_t j = 0; j < kPixels; ++j) {
    const uint32_t pixel = (j / kRectW) * kFilmW + j % kRectW;
    CHECK(f.n[pixel] == kK);
    for (int c = 0; c < 3; ++c) {
      const double want = expect[size_t(c) * f.plane_stride + pixel];
      CHECK(f.s1[size_t(f.moment[0].offset + c) * f.plane_stride + pixel] == doctest::Approx(want).epsilon(1e-4));
      ++checked;
    }
  }
  CHECK(checked == kPixels * 3u);
}

TEST_CASE("generate_camera/initial state") {
  Rig r;
  r.run_pass(4, kK);  // n = 4: the sample index is not the sample id
  const double spread = std::atan(2.0 * kTy / kRectH);  // 2 tan(fov_y / 2) / H
  std::set<uint32_t> heroes;
  for (uint32_t sample = 0; sample < kSlots; ++sample) {
    const uint32_t slot = r.queue[sample];
    const uint32_t j = sample / kK, m = sample % kK;
    const uint32_t pixel = (j / kRectW) * kFilmW + j % kRectW;
    CHECK(r.path.next_stage[slot] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    CHECK(r.path.term[slot] == uint8_t(ar::Term::ACTIVE));
    CHECK(r.path.ray_type[slot] == AR_VISIBILITY_CAMERA);
    CHECK(r.path.event_index[slot] == 0);
    for (int i = 0; i < 6; ++i) CHECK(r.path.bounces[slot].v[i] == 0);
    for (int i = 0; i < ar::kLanes; ++i) {
      CHECK(r.path.beta[slot].v[i] == 1.0f);
      CHECK(r.path.r_u[slot].v[i] == 1.0f);
      CHECK(r.path.r_l[slot].v[i] == 1.0f);
    }
    CHECK(r.path.eta_scale[slot] == 1.0f);
    CHECK(r.path.prev_ng[slot] == 0u);
    CHECK(r.path.prev_ns[slot] == 0u);
    CHECK(r.path.prev_dist[slot] == 0.0f);
    CHECK(r.path.cone[slot].x == 0.0f);
    CHECK(r.path.cone[slot].y == doctest::Approx(spread).epsilon(1e-6));
    // Flags: a camera path whose only set bits are the hero lane floor(4 u) of dimension 6.
    const ar::Real u = ar::draw(ar::SAMPLER_SOBOL, pixel, 4 + m, 0u, ar::DIM_CAM_HERO, r.kg.session.seed);
    const uint32_t hero = uint32_t(double(u) * 4.0);
    CHECK(r.path.flags[slot] == (hero << ar::HERO_LANE_SHIFT));
    heroes.insert(hero);
    // The film position is the pixel centre plus the importance-sampled filter offset of dims 0 and 1
    // (0008 d5; production path, no PIXEL_CENTRE hook).
    ar::Real2 film{};
    REQUIRE(ray_pixel(r, slot, &film));
    const ar::Real ux = ar::draw(ar::SAMPLER_SOBOL, pixel, 4 + m, 0u, ar::DIM_CAM_FILTER_X, r.kg.session.seed);
    const ar::Real uy = ar::draw(ar::SAMPLER_SOBOL, pixel, 4 + m, 0u, ar::DIM_CAM_FILTER_Y, r.kg.session.seed);
    const ar::FilterOffset off = ar::sample_filter(*r.filter, ux, uy);
    CHECK(film.x == doctest::Approx(j % kRectW + 0.5 + off.dx).epsilon(1e-4));
    CHECK(film.y == doctest::Approx(j / kRectW + 0.5 + off.dy).epsilon(1e-4));
    CHECK(std::fabs(off.dx) > 0.0f);  // the offset is not silently zero
  }
  CHECK(heroes.size() > 1);  // 70 draws reach more than one lane
  CHECK(AR_VISIBILITY_CAMERA == ar::kRayTypeCamera);
}

}  // TEST_SUITE
