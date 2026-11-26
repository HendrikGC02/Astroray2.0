// SPDX-License-Identifier: Apache-2.0
// film_prepare_denoise on the CPU device: a hand-filled film, expected values are closed forms
// (the mean with negatives clamped, the guide planes' sum over n, albedo clamped to [0, 1]).
// Ref: docs/decisions/0008-film-outputs-denoise.md d15, d16, d17; 0002 D8.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "film/prepare_denoise.h"
#include "stages/film_build_active.h"
#include "host/film/film_layout.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

struct DenoiseRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};
  ar::DenoiseTarget target{};
  std::vector<uint32_t> list;
  std::vector<float> colour, albedo, normal;

  DenoiseRig(uint32_t w, uint32_t h, bool guides) : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = w;
    req.max_height = h;
    req.guides = guides;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    kg.film = &film->data();
    kg.denoise = &target;
    rebuild(w, h);
  }
  // The pass's active-pixel list and a target of the rectangle's size, filled with a sentinel so
  // an unwritten element shows.
  void rebuild(uint32_t w, uint32_t h) {
    list.resize(f().plane_stride);
    kg.active_pixels = list.data();
    cpu_film_build_active(kg, 0, ar::film_build_active_count(f()));
    const size_t floats = size_t(w) * h * 3;
    colour.assign(floats, -42.0f), albedo.assign(floats, -42.0f), normal.assign(floats, -42.0f);
    target = {colour.data(), albedo.data(), normal.data(), w, h};
  }
  ar::FilmData& f() { return film->data(); }
  void set_mean(uint32_t pixel, uint32_t n, double r, double g, double b) {
    ar::FilmData& d = f();
    d.n[pixel] = n;
    const double v[3] = {r, g, b};
    for (uint32_t c = 0; c < 3; ++c) d.s1[uint64_t(d.moment[0].offset + c) * d.plane_stride + pixel] = v[c] * n;
  }
  // The guide planes hold sums over the n samples: sum = n * mean.
  void set_guides(uint32_t pixel, const float albedo_sum[3], const float normal_sum[3]) {
    ar::FilmData& d = f();
    for (uint32_t c = 0; c < 3; ++c) {
      d.add[uint64_t(uint32_t(d.guide_albedo) + c) * d.plane_stride + pixel] = albedo_sum[c];
      d.add[uint64_t(uint32_t(d.guide_normal) + c) * d.plane_stride + pixel] = normal_sum[c];
    }
  }
  std::vector<unsigned char> film_bytes() const {
    const DeviceBuffer& b = film->buffer();
    return std::vector<unsigned char>(static_cast<const unsigned char*>(b.ptr),
                                      static_cast<const unsigned char*>(b.ptr) + b.bytes);
  }
  void run() { cpu_film_prepare_denoise(kg, 0, ar::film_prepare_denoise_count(kg)); }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("prepare_denoise/negatives clamp in the copy only") {
  DenoiseRig rig(3, 1, true);
  rig.set_mean(0, 4, 0.5, 2.0, 8.0);
  rig.set_mean(1, 2, -1.5, 3.0, -0.25);  // one pixel with negative components
  rig.set_mean(2, 0, 0.0, 0.0, 0.0);
  const std::vector<unsigned char> before = rig.film_bytes();
  rig.run();
  CHECK(rig.colour[0] == 0.5f);
  CHECK(rig.colour[1] == 2.0f);
  CHECK(rig.colour[2] == 8.0f);
  CHECK(rig.colour[3] == 0.0f);  // -1.5 clamped
  CHECK(rig.colour[4] == 3.0f);
  CHECK(rig.colour[5] == 0.0f);  // -0.25 clamped
  // The film keeps its negatives and nothing in it moved.
  const std::vector<unsigned char> after = rig.film_bytes();
  CHECK(before.size() == after.size());
  CHECK(std::memcmp(before.data(), after.data(), before.size()) == 0);
  const ar::FilmData& d = rig.f();
  CHECK(d.s1[uint64_t(d.moment[0].offset) * d.plane_stride + 1] == -3.0);
}

TEST_CASE("prepare_denoise/albedo clamps and normal is the mean") {
  DenoiseRig rig(2, 1, true);
  rig.set_mean(0, 4, 1.0, 1.0, 1.0);
  rig.set_mean(1, 2, 1.0, 1.0, 1.0);
  // Pixel 0: albedo sums 6, 2, -4 over 4 samples (1.5 clamps to 1, 0.5, -1 clamps to 0); the
  // normal sum (2, -1, 1) over 4 samples is (0.5, -0.25, 0.25), length < 1, kept as is.
  const float a0[3] = {6.0f, 2.0f, -4.0f}, n0[3] = {2.0f, -1.0f, 1.0f};
  // Pixel 1: normal sum (1.5, 1.5, 1.5) over 2 samples is (0.75, 0.75, 0.75), length 1.30, not
  // renormalised.
  const float a1[3] = {0.5f, 1.0f, 0.0f}, n1[3] = {1.5f, 1.5f, 1.5f};
  rig.set_guides(0, a0, n0);
  rig.set_guides(1, a1, n1);
  rig.run();
  CHECK(rig.albedo[0] == 1.0f);
  CHECK(rig.albedo[1] == 0.5f);
  CHECK(rig.albedo[2] == 0.0f);
  CHECK(rig.normal[0] == 0.5f);
  CHECK(rig.normal[1] == -0.25f);
  CHECK(rig.normal[2] == 0.25f);
  CHECK(rig.albedo[3] == 0.25f);
  CHECK(rig.albedo[4] == 0.5f);
  CHECK(rig.albedo[5] == 0.0f);
  for (uint32_t c = 3; c < 6; ++c) CHECK(rig.normal[c] == 0.75f);
}

TEST_CASE("prepare_denoise/no samples writes zero") {
  DenoiseRig rig(1, 1, true);
  rig.set_mean(0, 0, 0.0, 0.0, 0.0);
  // Guide sums and a moment that a sample-less pixel must ignore.
  const float a[3] = {1.0f, 1.0f, 1.0f}, nn[3] = {1.0f, 0.0f, 0.0f};
  rig.set_guides(0, a, nn);
  rig.f().s1[uint64_t(rig.f().moment[0].offset) * rig.f().plane_stride] = 5.0;
  rig.run();
  for (uint32_t c = 0; c < 3; ++c) {
    CHECK(rig.colour[c] == 0.0f);
    CHECK(rig.albedo[c] == 0.0f);
    CHECK(rig.normal[c] == 0.0f);
  }
}

TEST_CASE("prepare_denoise/non-finite never leaves") {
  DenoiseRig rig(1, 1, true);
  rig.set_mean(0, 1, std::numeric_limits<double>::quiet_NaN(), 1e300, 1.0);  // 1e300 overflows float
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float a[3] = {nan, inf, 0.5f}, nn[3] = {inf, nan, -inf};
  rig.set_guides(0, a, nn);
  rig.run();
  CHECK(rig.colour[0] == 0.0f);
  CHECK(rig.colour[1] == 0.0f);
  CHECK(rig.colour[2] == 1.0f);
  CHECK(rig.albedo[0] == 0.0f);
  CHECK(rig.albedo[1] == 0.0f);
  CHECK(rig.albedo[2] == 0.5f);
  for (uint32_t c = 0; c < 3; ++c) CHECK(rig.normal[c] == 0.0f);
}

TEST_CASE("prepare_denoise/rows bottom-up and the present mean") {
  // 4 x 3 allocation, 3 x 2 sub-rectangle: element j is pixel (j % 3, j / 3), row 0 the bottom.
  DenoiseRig rig(4, 3, true);
  rig.film->set_render_size(3, 2);
  rig.rebuild(3, 2);
  const ar::FilmData& d = rig.f();
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 3; ++x) {
      const uint32_t j = y * 3 + x, pixel = y * d.width + x;
      rig.set_mean(pixel, 2 + j, 0.5 * double(j + 1), 1.0, 0.25);
      const float a[3] = {0.0f, 0.0f, 0.0f}, nn[3] = {float(j) * float(2 + j), 0.0f, 0.0f};
      rig.set_guides(pixel, a, nn);
    }
  rig.run();
  CHECK(ar::film_prepare_denoise_count(rig.kg) == 6u);
  for (uint32_t j = 0; j < 6; ++j) {
    // The display mean film_present reads: S1 / n, exposure already in the moments.
    float want[3];
    ar::film_display_mean(d, rig.list[j], want);
    CHECK(rig.colour[3 * j] == 0.5f * float(j + 1));
    CHECK(rig.colour[3 * j] == want[0]);
    CHECK(rig.colour[3 * j + 1] == want[1]);
    CHECK(rig.colour[3 * j + 2] == 0.25f);
    CHECK(rig.normal[3 * j] == float(j));
  }
}

TEST_CASE("prepare_denoise/guides off writes zero guides") {
  DenoiseRig rig(2, 1, false);
  CHECK(rig.f().guide_albedo == -1);
  CHECK(rig.f().guide_normal == -1);
  rig.set_mean(0, 2, 1.0, 2.0, 3.0);
  rig.set_mean(1, 1, 4.0, 5.0, 6.0);
  rig.run();
  CHECK(rig.colour[0] == 1.0f);
  CHECK(rig.colour[5] == 6.0f);
  for (uint32_t i = 0; i < 6; ++i) {
    CHECK(rig.albedo[i] == 0.0f);
    CHECK(rig.normal[i] == 0.0f);
  }
}

}  // TEST_SUITE
