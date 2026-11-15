// SPDX-License-Identifier: Apache-2.0
// film_present, film_heatmap and the display ring on the CPU device. Expected values are closed
// forms or an independent reference: the half conversion is checked against a brute-force nearest
// even search over every finite half (never against float_to_half itself), the viridis ends against
// the published colours #440154 and #FDE725, the ring against its contract.
// Ref: docs/decisions/0007-blender-integration.md d29, d30; 0008 d12;
// Smith and van der Walt 2015 (viridis); IEEE 754-2008 binary16.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "stages/film_build_active.h"
#include "host/display/display_ring.h"
#include "host/film/film_layout.h"
#include "host/film/film_stats.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

struct PresentRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};
  ar::DisplayTarget target{};
  // The pass's active-pixel list, built the way the session does at pass start (0008 d3).
  std::vector<uint32_t> list;
  void build_list() {
    list.resize(f().plane_stride);
    kg.active_pixels = list.data();
    cpu_film_build_active(kg, 0, ar::film_build_active_count(f()));
  }

  PresentRig(uint32_t w, uint32_t h) : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = w;
    req.max_height = h;
    req.statistics = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    kg.film = &film->data();
    kg.display = &target;
    build_list();
  }
  ar::FilmData& f() { return film->data(); }
  // The display mean of pixel p becomes (r, g, b) with n samples: S1 = n * value.
  void set_mean(uint32_t pixel, uint32_t n, double r, double g, double b) {
    ar::FilmData& d = f();
    d.n[pixel] = n;
    const double v[3] = {r, g, b};
    for (uint32_t c = 0; c < 3; ++c) d.s1[uint64_t(d.moment[0].offset + c) * d.plane_stride + pixel] = v[c] * n;
  }
};

// Half to float, written for the test (exact for every half).
double half_value(uint32_t h) {
  const int e = int((h >> 10) & 0x1f);
  const double m = double(h & 0x3ff);
  const double mag = e == 0 ? std::ldexp(m, -24) : std::ldexp(1024.0 + m, e - 25);
  return (h & 0x8000u) ? -mag : mag;
}

// The nearest finite non-negative half to x (ties to the even fraction), by search.
uint32_t nearest_half(double x, const std::vector<double>& table) {
  const auto it = std::lower_bound(table.begin(), table.end(), x);
  if (it == table.begin()) return 0;
  if (it == table.end()) return uint32_t(table.size() - 1);
  const uint32_t hi = uint32_t(it - table.begin()), lo = hi - 1;
  const double dl = x - table[lo], dh = table[hi] - x;
  if (dl < dh) return lo;
  if (dh < dl) return hi;
  return (lo & 1u) == 0u ? lo : hi;
}

uint32_t pack(uint32_t r, uint32_t g, uint32_t b) { return r | (g << 8) | (b << 16) | 0xff000000u; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("present/half clamp") {
  CHECK(ar::float_to_half(0.0f) == 0x0000);
  CHECK(ar::float_to_half(1.0f) == 0x3c00);
  CHECK(ar::float_to_half(65504.0f) == 0x7bff);
  CHECK(ar::float_to_half(ar::film_clamp_half(70000.0f)) == 0x7bff);
  CHECK(ar::float_to_half(ar::film_clamp_half(-70000.0f)) == 0xfbff);
  CHECK(ar::float_to_half(ar::film_clamp_half(std::numeric_limits<float>::infinity())) == 0x7bff);
  // A NaN is never hidden as a finite value: it stays a half NaN, and Inf stays Inf.
  const uint16_t nan_half = ar::float_to_half(ar::film_clamp_half(std::numeric_limits<float>::quiet_NaN()));
  CHECK((nan_half & 0x7c00) == 0x7c00);
  CHECK((nan_half & 0x3ff) != 0);
  CHECK(ar::float_to_half(std::numeric_limits<float>::infinity()) == 0x7c00);
  // The finite clamp holds through the whole kernel: S1 / n = 70000 presents as 65504.
  PresentRig rig(1, 1);
  rig.set_mean(0, 4, 70000.0, -70000.0, 1.0);
  std::vector<uint16_t> out(4);
  rig.target = {out.data(), 0, 0.0f};
  cpu_film_present(rig.kg, 0, 1);
  CHECK(out[0] == 0x7bff);
  CHECK(out[1] == 0xfbff);
  CHECK(out[2] == 0x3c00);
  CHECK(out[3] == 0x3c00);
}

TEST_CASE("present/half rounding to nearest even") {
  // Ties and subnormal edges.
  CHECK(ar::float_to_half(1.0f + std::ldexp(1.0f, -11)) == 0x3c00);        // tie, even below
  CHECK(ar::float_to_half(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3c02);  // tie, even above
  CHECK(ar::float_to_half(std::ldexp(1.0f, -25)) == 0x0000);                // tie between 0 and 1
  CHECK(ar::float_to_half(std::ldexp(1.0f, -24)) == 0x0001);
  CHECK(ar::float_to_half(1.5f * std::ldexp(1.0f, -24)) == 0x0002);         // tie between 1 and 2
  CHECK(ar::float_to_half(std::ldexp(1.0f, -14)) == 0x0400);                // smallest normal
  CHECK(ar::float_to_half(-0.0f) == 0x8000);
  // Against the search reference: every half round-trips, and random floats agree.
  std::vector<double> table(0x7c00);
  for (uint32_t h = 0; h < 0x7c00; ++h) table[h] = half_value(h);
  for (uint32_t h = 0; h < 0x7c00; ++h) {
    REQUIRE(ar::float_to_half(float(table[h])) == h);
    REQUIRE(ar::float_to_half(-float(table[h])) == (h | 0x8000u));
  }
  uint64_t x = 12345;
  uint32_t bad = 0;
  for (int i = 0; i < 200000; ++i) {
    x = x * 6364136223846793005ull + 1442695040888963407ull;
    const double u = double(x >> 11) * (1.0 / 9007199254740992.0);
    const float v = float(std::exp(std::log(1e-8) + u * (std::log(65504.0) - std::log(1e-8))));
    bad += ar::float_to_half(v) != nearest_half(double(v), table);
  }
  CHECK(bad == 0u);
}

TEST_CASE("present/film_present rows and no data") {
  // 4 x 3 allocation, 3 x 2 sub-rectangle: element j is pixel (j % 3, j / 3), row 0 the bottom.
  PresentRig rig(4, 3);
  rig.film->set_render_size(3, 2);
  rig.build_list();
  const ar::FilmData& d = rig.f();
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 3; ++x) {
      const uint32_t j = y * 3 + x, pixel = y * d.width + x;
      if (j != 4) rig.set_mean(pixel, 2 + j, 0.5 * double(j + 1), 1.0, 0.25);
    }
  std::vector<uint16_t> out(3 * 2 * 4, 0xffffu);
  rig.target = {out.data(), 0, 0.0f};
  CHECK(ar::film_present_count(rig.kg) == 6u);
  cpu_film_present(rig.kg, 0, 6);
  for (uint32_t j = 0; j < 6; ++j) {
    if (j == 4) {  // n = 0: black with alpha 1, never NaN
      CHECK(out[4 * j] == 0x0000);
      CHECK(out[4 * j + 1] == 0x0000);
      CHECK(out[4 * j + 2] == 0x0000);
    } else {
      CHECK(half_value(out[4 * j]) == 0.5 * double(j + 1));  // exact in half
      CHECK(out[4 * j + 1] == 0x3c00);
      CHECK(out[4 * j + 2] == 0x3400);  // 0.25
    }
    CHECK(out[4 * j + 3] == 0x3c00);
  }
}

TEST_CASE("present/heatmap viridis ends") {
  // Published viridis ends: #440154 at 0 and #FDE725 at 1 (Smith and van der Walt 2015).
  PresentRig rig(4, 1);
  ar::FilmData& d = rig.f();
  const uint32_t paths = uint32_t(d.bounce_paths) * d.plane_stride, sum = uint32_t(d.bounce_sum) * d.plane_stride;
  // Bounce mean over [0, 4]: 0 -> entry 0, 4 -> entry 255, 9 clamps to the top, no paths -> grey.
  d.count[paths + 0] = 3;
  d.add[sum + 0] = 0.0f;
  d.count[paths + 1] = 2;
  d.add[sum + 1] = 8.0f;
  d.count[paths + 2] = 1;
  d.add[sum + 2] = 9.0f;
  std::vector<uint32_t> out(4);
  rig.target = {out.data(), ar::HEATMAP_BOUNCE_COUNT, 4.0f};
  REQUIRE(ar::film_heatmap_supported(d, rig.target.mode));
  cpu_film_heatmap(rig.kg, 0, 4);
  CHECK(out[0] == pack(0x44, 0x01, 0x54));
  CHECK(out[1] == pack(0xfd, 0xe7, 0x25));
  CHECK(out[2] == pack(0xfd, 0xe7, 0x25));
  CHECK(out[3] == ar::kHeatmapNoData);

  // Sample count over [0, 8]: n = 0 is no data, 8 the top, 4 the middle entry (128).
  d.n[0] = 0;
  d.n[1] = 8;
  d.n[2] = 4;
  d.n[3] = 100;
  rig.target.mode = ar::HEATMAP_SAMPLE_COUNT;
  rig.target.range_max = 8.0f;
  cpu_film_heatmap(rig.kg, 0, 4);
  const auto entry = [](int i) {
    return pack(uint32_t(ar::viridis_component(i, 0) * 255.0f + 0.5f), uint32_t(ar::viridis_component(i, 1) * 255.0f + 0.5f),
                uint32_t(ar::viridis_component(i, 2) * 255.0f + 0.5f));
  };
  CHECK(out[0] == ar::kHeatmapNoData);
  CHECK(out[1] == pack(0xfd, 0xe7, 0x25));
  CHECK(out[2] == entry(128));
  CHECK(out[3] == pack(0xfd, 0xe7, 0x25));
  // Entry 128 is a genuinely different colour from both ends.
  CHECK(out[2] != out[1]);
}

TEST_CASE("present/heatmap bounce mean divides by paths ended") {
  // Two valid samples, four paths ended (two samples were invalid and dropped from n): the mean
  // is add / paths = 2, not add / n = 4.
  PresentRig rig(1, 1);
  ar::FilmData& d = rig.f();
  d.n[0] = 2;
  d.count[uint64_t(d.bounce_paths) * d.plane_stride] = 4;
  d.add[uint64_t(d.bounce_sum) * d.plane_stride] = 8.0f;
  CHECK(film_bounce_mean(d, 0) == 2.0);
  std::vector<uint32_t> out(1);
  rig.target = {out.data(), ar::HEATMAP_BOUNCE_COUNT, 4.0f};
  cpu_film_heatmap(rig.kg, 0, 1);
  const int mid = 128;  // t = 2 / 4 = 0.5
  CHECK(out[0] == pack(uint32_t(ar::viridis_component(mid, 0) * 255.0f + 0.5f),
                       uint32_t(ar::viridis_component(mid, 1) * 255.0f + 0.5f),
                       uint32_t(ar::viridis_component(mid, 2) * 255.0f + 0.5f)));
}

TEST_CASE("present/heatmap needs its planes") {
  PresentRig rig(1, 1);
  ar::FilmData d = rig.f();
  d.bounce_sum = d.bounce_paths = -1;
  CHECK(ar::film_heatmap_supported(d, ar::HEATMAP_SAMPLE_COUNT));
  CHECK_FALSE(ar::film_heatmap_supported(d, ar::HEATMAP_BOUNCE_COUNT));
  CHECK_FALSE(ar::film_heatmap_supported(d, 7u));
}

TEST_CASE("present/ring newest frame") {
  DisplayRing ring(64);
  const auto frame = [](uint8_t v) { return std::vector<uint8_t>(16, v); };  // 2 x 2 RGBA8
  const auto a = frame(1), b = frame(2);
  const uint64_t id1 = ring.publish_host(a.data(), 5, 2, 2, DisplayFormat::RGBA8_SRGB);
  const uint64_t id2 = ring.publish_host(b.data(), 6, 2, 2, DisplayFormat::RGBA8_SRGB);
  REQUIRE(id1 == 1);
  REQUIRE(id2 == 2);
  auto got = ring.acquire(0);
  REQUIRE(got.has_value());
  CHECK(got->frame_id == 2);  // the newest, not the oldest
  CHECK(got->generation == 6);
  CHECK(got->width == 2);
  CHECK(got->row_bytes == 8);
  CHECK(static_cast<const uint8_t*>(got->data)[0] == 2);
  CHECK_FALSE(ring.acquire(2).has_value());  // nothing newer than the last drawn
  const auto older = ring.acquire(1);        // newer than 1 is frame 2 again
  REQUIRE(older.has_value());
  CHECK(older->frame_id == 2);
  ring.release(2);
  ring.release(2);
  CHECK_THROWS_AS(ring.release(2), std::invalid_argument);  // not acquired any more
}

TEST_CASE("present/ring released slots reused") {
  DisplayRing ring(64);
  std::vector<uint8_t> px(16);
  uint64_t last_drawn = 0, last_generation = 0;
  for (uint32_t i = 0; i < 20; ++i) {
    std::fill(px.begin(), px.end(), uint8_t(i));
    REQUIRE(ring.publish_host(px.data(), 1 + i / 4, 2, 2, DisplayFormat::RGBA8_SRGB) == i + 1);
    const auto f = ring.acquire(last_drawn);
    REQUIRE(f.has_value());
    CHECK(f->frame_id == i + 1);
    CHECK(f->generation >= last_generation);  // generations never go backwards
    CHECK(static_cast<const uint8_t*>(f->data)[7] == uint8_t(i));
    last_drawn = f->frame_id;
    last_generation = f->generation;
    ring.release(f->frame_id);
  }
  CHECK(ring.dropped() == 0u);
  // The producer's generation must not go backwards either.
  CHECK_THROWS_AS(ring.publish_host(px.data(), 1, 2, 2, DisplayFormat::RGBA8_SRGB), std::logic_error);
}

TEST_CASE("present/ring read slot never overwritten") {
  DisplayRing ring(64);
  std::vector<uint8_t> px(16);
  const auto publish = [&](uint8_t v) {
    std::fill(px.begin(), px.end(), v);
    return ring.publish_host(px.data(), 1, 2, 2, DisplayFormat::RGBA8_SRGB);
  };
  REQUIRE(publish(10) == 1);
  const auto held = ring.acquire(0);  // frame 1 stays held
  REQUIRE(held.has_value());
  for (uint8_t v = 11; v < 30; ++v) REQUIRE(publish(v) != 0);  // two free slots cycle
  CHECK(static_cast<const uint8_t*>(held->data)[0] == 10);
  CHECK(ring.dropped() == 0u);
  // Hold the newest frames too: once every slot is being read, the next frame is dropped and counted.
  const auto h2 = ring.acquire(held->frame_id);
  REQUIRE(h2.has_value());
  const auto h3 = ring.acquire(0);  // the newest again, a second reader of the same slot
  REQUIRE(h3.has_value());
  CHECK(publish(40) != 0);  // the one slot nobody reads: the older unread frame
  CHECK(publish(41) != 0);  // supersedes the unread 40
  const auto h5 = ring.acquire(h3->frame_id);
  REQUIRE(h5.has_value());
  CHECK(publish(42) == 0u);  // frame 1, the h2/h3 slot and the newest are all being read
  CHECK(ring.dropped() == 1u);
  CHECK(static_cast<const uint8_t*>(held->data)[0] == 10);
  CHECK(static_cast<const uint8_t*>(h2->data)[0] == 29);
  CHECK(static_cast<const uint8_t*>(h5->data)[0] == 41);
  ring.release(held->frame_id);
  CHECK(publish(43) != 0);  // a released slot is reused
  CHECK_THROWS_AS(ring.publish_host(px.data(), 9, 100, 100, DisplayFormat::RGBA16F), std::invalid_argument);
}

TEST_CASE("present/ring allocator and failed copy") {
  const SlotAllocator only_allocate{[](uint64_t b) -> void* { return std::malloc(b); }, nullptr};
  CHECK_THROWS_AS(DisplayRing(64, only_allocate), std::invalid_argument);
  const SlotAllocator only_release{nullptr, [](void* p) { std::free(p); }};
  CHECK_THROWS_AS(DisplayRing(64, only_release), std::invalid_argument);

  // A copy that throws leaves the ring as it was: no frame, same ids and generation.
  PresentRig rig(1, 1);
  DisplayRing ring(64);
  std::vector<uint8_t> px(16, 7);
  REQUIRE(ring.publish_host(px.data(), 5, 2, 2, DisplayFormat::RGBA8_SRGB) == 1);
  DeviceBuffer small = rig.dev.allocate(LedgerCategory::FILM, 8, Owner{1, 1});
  CHECK_THROWS(ring.publish(rig.dev, small, 9, 2, 2, DisplayFormat::RGBA8_SRGB));  // 16 bytes from an 8-byte buffer
  rig.dev.free(small);
  CHECK(ring.acquire(1) == std::nullopt);
  CHECK(ring.publish_host(px.data(), 5, 2, 2, DisplayFormat::RGBA8_SRGB) == 2);  // id 2, generation 9 never taken
}

TEST_CASE("present/ring publishes a device buffer") {
  PresentRig rig(2, 2);
  DeviceBuffer buf = rig.dev.allocate(LedgerCategory::FILM, 32, Owner{1, 1});
  std::vector<uint16_t> halves(16);
  for (uint32_t i = 0; i < 16; ++i) halves[i] = uint16_t(0x3c00 + i);
  rig.dev.upload(buf, 0, halves.data(), 32);
  DisplayRing ring(64);
  REQUIRE(ring.publish(rig.dev, buf, 3, 2, 2, DisplayFormat::RGBA16F) == 1);
  const auto f = ring.acquire(0);
  REQUIRE(f.has_value());
  CHECK(f->row_bytes == 16);
  CHECK(std::memcmp(f->data, halves.data(), 32) == 0);
  ring.release(f->frame_id);
  rig.dev.free(buf);
}

}  // TEST_SUITE

TEST_SUITE("gate") {

TEST_CASE("present/reads the active list") {
  // Element j of the output is the list's j-th pixel, not pixel j: the list is a permutation of a
  // 4 x 3 film, pixel list[j] holds the display mean j + 1 over j + 1 samples, and the
  // expected output is read off the list, for the beauty and the sample-count kernels.
  PresentRig rig(4, 3);
  const uint32_t pixels = 12;
  for (uint32_t j = 0; j < pixels; ++j) rig.list[j] = (j * 5u + 3u) % pixels;  // 5 is coprime with 12
  for (uint32_t j = 0; j < pixels; ++j) rig.set_mean(rig.list[j], j + 1, double(j + 1), 1.0, 1.0);
  std::vector<uint16_t> out(pixels * 4, 0xffffu);
  rig.target = {out.data(), 0, 0.0f};
  cpu_film_present(rig.kg, 0, pixels);
  uint32_t wrong = 0;
  for (uint32_t j = 0; j < pixels; ++j) wrong += half_value(out[4 * j]) != double(j + 1);
  CHECK(wrong == 0);

  std::vector<uint32_t> heat(pixels, 0u);
  rig.target = {heat.data(), ar::HEATMAP_SAMPLE_COUNT, 16.0f};
  cpu_film_heatmap(rig.kg, 0, pixels);
  wrong = 0;
  for (uint32_t j = 0; j < pixels; ++j) wrong += heat[j] != ar::viridis_rgba8(float(j + 1) / 16.0f);
  CHECK(wrong == 0);
}

}  // TEST_SUITE
