// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Counter-based sampler of 0003 d23: exact values against an independent model, net
// property, f32/f64 bit identity, keys and the dimension map; the statistical cases
// (chi-squared, hero lane, independent-mode correlation) run once, in the f32 copy.
// Ref: 0003 Verification checks 14, 24, 25. Reference values: a pure-Python model of d23
// (Burley 2020 shuffled scrambled Sobol, pbrt MixBits, pcg4d) whose direction numbers come
// from scipy.stats.qmc.Sobol (Joe-Kuo new-joe-kuo-6.21201), not from src/kernel/sample/.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "common/stat.h"
#include "sample/dims.h"
#include "sample/hash.h"
#include "sample/keys.h"
#include "sample/sobol.h"

namespace {

namespace st = astroray::stat;

#if defined(AR_REAL_DOUBLE)
constexpr const char* kBuild = "f64";
#else
constexpr const char* kBuild = "f32";
#endif

// The 24 random bits of a draw, recovered from the Real so the conversion is checked too.
uint32_t u24(ar::Real u) { return static_cast<uint32_t>(static_cast<double>(u) * 16777216.0); }

uint64_t fnv_add(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h = (h ^ (v & 0xffu)) * 0x100000001b3ull;
    v >>= 8;
  }
  return h;
}

struct Args {
  uint32_t id, index, stream, d, seed;
};
// The 10^6 draws of the identity check; the Python model uses the same formulas.
Args draw_args(uint32_t i) {
  return Args{i * 0x9e3779b1u, (i % 3u == 0u) ? (i % 4096u) : (i * 0x85ebca6bu + 12345u), i & 3u,
              (i * 13u) % 200u, (i * 0xc2b2ae35u) ^ 0xdeadbeefu};
}

void bitwise_identity() {
  constexpr uint32_t kN = 1000000;
  // fnv1a-64 over u24 of every draw, from the Python model (ref_sampler.py main loop).
  constexpr uint64_t kFnvSobol = 0x4490de368401daefull;
  constexpr uint64_t kFnvIndependent = 0x7b0aca786001c887ull;
  std::vector<double> u(2 * size_t(kN));
  uint64_t hs = 0xcbf29ce484222325ull, hi = hs;
  for (uint32_t i = 0; i < kN; ++i) {
    const Args a = draw_args(i);
    const ar::Real s = ar::sample(a.id, a.index, a.stream, a.d, a.seed);
    const ar::Real n = ar::sample_independent(a.id, a.index, a.stream, a.d, a.seed);
    u[2 * size_t(i)] = static_cast<double>(s);
    u[2 * size_t(i) + 1] = static_cast<double>(n);
    hs = fnv_add(hs, u24(s));
    hi = fnv_add(hi, u24(n));
  }
  CHECK(hs == kFnvSobol);
  CHECK(hi == kFnvIndependent);

  // The other build's file, if it already ran in this process: compare every draw. The name holds
  // the process creation time as well as the id: ids are reused, so an id alone can pick up a file
  // an earlier process left behind.
  namespace fs = std::filesystem;
  FILETIME created{}, exited{}, kernel_time{}, user_time{};
  REQUIRE(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_time, &user_time));
  const uint64_t created_at = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  const std::string pid = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(created_at);
  const fs::path dir = fs::temp_directory_path();
  const fs::path mine = dir / ("astroray_sampler_" + pid + "_" + kBuild + ".bin");
  const fs::path other = dir / ("astroray_sampler_" + pid + (std::strcmp(kBuild, "f32") == 0 ? "_f64.bin" : "_f32.bin"));
  {
    std::ofstream f(mine, std::ios::binary);
    f.write(reinterpret_cast<const char*>(u.data()), static_cast<std::streamsize>(u.size() * sizeof(double)));
    REQUIRE(f.good());
  }
  if (fs::exists(other)) {
    std::vector<double> v(u.size());
    std::ifstream f(other, std::ios::binary);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(double)));
    REQUIRE(f.gcount() == static_cast<std::streamsize>(v.size() * sizeof(double)));
    size_t bad = 0;
    for (size_t i = 0; i < u.size(); ++i) bad += std::memcmp(&u[i], &v[i], sizeof(double)) != 0;
    std::printf("sampler bitwise f32/f64: %u draws compared (sobol and independent), %zu mismatches\n",
                kN, bad);
    CHECK(bad == 0);
    std::error_code ec;
    fs::remove(mine, ec);
    fs::remove(other, ec);
  } else {
    std::printf("sampler bitwise [%s]: %u draws, fnv1a64 sobol %016llx independent %016llx; "
                "other build not run yet in this process\n",
                kBuild, kN, static_cast<unsigned long long>(hs), static_cast<unsigned long long>(hi));
  }
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("sampler/hashes and direction numbers") {
    // mix_bits, pcg4d and the raw Sobol lanes: Python model values.
    CHECK(ar::mix_bits(0) == 0x0000000000000000ull);
    CHECK(ar::mix_bits(1) == 0xccde22c1faa4d20full);
    CHECK(ar::mix_bits(0x0123456789abcdefull) == 0x0eb3513c40b11a29ull);
    struct P { ar::UInt4 in, out; };
    const P pcg[] = {
        {{0, 0, 0, 0}, {0x0f02f829u, 0x2d568769u, 0x32b0c43bu, 0xd32548eau}},
        {{1, 2, 3, 4}, {0x3622cd16u, 0xf11471d8u, 0xe1109b3fu, 0x02b94c2fu}},
        {{0xdeadbeefu, 0x12345678u, 0x0badf00du, 0xffffffffu},
         {0xed1cebf8u, 0xca41b455u, 0x19a5aa98u, 0xeab4a9fau}}};
    for (const P& p : pcg) {
      const ar::UInt4 o = ar::pcg4d(p.in);
      CHECK(o.x == p.out.x);
      CHECK(o.y == p.out.y);
      CHECK(o.z == p.out.z);
      CHECK(o.w == p.out.w);
    }
    struct L { uint32_t index, v[4]; };
    const L lanes[] = {{1u, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}},
                       {2u, {0x40000000u, 0xc0000000u, 0xc0000000u, 0xc0000000u}},
                       {3u, {0xc0000000u, 0x40000000u, 0x40000000u, 0x40000000u}},
                       {0x12345678u, {0x1e6a2c48u, 0xf00aac48u, 0x6ab2e0a8u, 0x57c3fc38u}},
                       {0xaaaaaaaau, {0x55555555u, 0x00000003u, 0xc9009999u, 0x900b4977u}},
                       {0xffffffffu, {0xffffffffu, 0x00000001u, 0x4f00ffffu, 0x300cff8du}}};
    for (const L& l : lanes)
      for (uint32_t j = 0; j < 4; ++j) CHECK(ar::sobol_lane(j, l.index) == l.v[j]);
    CHECK(ar::sobol_lane(2, 0) == 0u);
  }

  AR_RT_CASE("sampler/pinned draws") {
    // u24 = (x >> 8) of sample() and sample_independent(): Python model.
    struct C { uint32_t id, index, stream, d, seed, sobol, indep; };
    const C cases[] = {
        {0u, 0u, 0u, 0u, 0u, 0u, 983800u},
        {0u, 0u, 0u, 0u, 12345u, 2768051u, 4012616u},
        {7u, 3u, 0u, 5u, 0u, 7179390u, 11897380u},
        {7u, 3u, 0u, 5u, 12345u, 3390874u, 11807104u},
        {123456u, 99u, 0u, 35u, 777u, 15822978u, 9343149u},
        {0x80000010u, 4u, 0u, 6u, 0xffffffffu, 7440535u, 1267430u},
        {5u, 1000000u, 1u, 0u, 1u, 1366737u, 1936380u},
        {5u, 1000000u, 2u, 3u, 1u, 1007054u, 7260468u},
        {65535u, 17u, 3u, 63u, 424242u, 13979509u, 7087171u},
        {1u, 0xffffffffu, 0u, 4u, 9u, 7623206u, 9586333u},
        {999u, 31u, 0u, 1u, 0u, 13971956u, 5701527u},
        {999u, 31u, 0u, 2u, 0u, 14708732u, 5056517u}};
    for (const C& c : cases) {
      CHECK(u24(ar::sample(c.id, c.index, c.stream, c.d, c.seed)) == c.sobol);
      CHECK(u24(ar::sample_independent(c.id, c.index, c.stream, c.d, c.seed)) == c.indep);
      CHECK(u24(ar::draw(ar::SAMPLER_SOBOL, c.id, c.index, c.stream, c.d, c.seed)) == c.sobol);
      CHECK(u24(ar::draw(ar::SAMPLER_INDEPENDENT, c.id, c.index, c.stream, c.d, c.seed)) == c.indep);
    }
  }

  AR_RT_CASE("sampler/net property dims 0 1") {
    // The first 2^m samples of dims (0, 1) of one pixel are a (0, m, 2)-net: every
    // elementary interval of volume 2^-m holds exactly one point (Owen scrambling and
    // the index shuffle preserve it). Checked for m = 1..10, all m + 1 interval shapes,
    // the first block and a later aligned block of indices.
    const uint32_t ids[] = {0u, 77u, 0x80000003u};
    const uint32_t seeds[] = {0u, 12345u};
    for (uint32_t id : ids)
      for (uint32_t seed : seeds)
        for (uint32_t m = 1; m <= 10; ++m)
          for (uint32_t block : {0u, 5u}) {
            const uint32_t n = 1u << m;
            std::vector<uint32_t> x0(n), x1(n);
            for (uint32_t i = 0; i < n; ++i) {
              x0[i] = ar::sample_bits(id, block * n + i, 0, 0, seed) >> 8;
              x1[i] = ar::sample_bits(id, block * n + i, 0, 1, seed) >> 8;
            }
            for (uint32_t a = 0; a <= m; ++a) {
              std::vector<uint8_t> hit(n, 0);
              for (uint32_t i = 0; i < n; ++i) {
                const uint32_t cx = x0[i] >> (24 - a);
                const uint32_t cy = x1[i] >> (24 - (m - a));
                ++hit[(cx << (m - a)) | cy];
              }
              bool ok = true;
              for (uint32_t c = 0; c < n; ++c) ok = ok && hit[c] == 1;
              INFO("id " << id << " seed " << seed << " m " << m << " block " << block << " a " << a);
              CHECK(ok);
            }
          }
  }

  AR_RT_CASE("sampler/f32 f64 bitwise") { bitwise_identity(); }

  AR_RT_CASE("sampler/keys and dimension map") {
    const uint32_t active[] = {10u, 20u, 30u, 40u};
    // sample_id 9, k 4: active[2], index n + 1.
    const ar::SamplerKey c = ar::camera_key(active, 9u, 100u, 4u);
    CHECK(c.id == 30u);
    CHECK(c.index == 101u);
    const ar::SamplerKey c0 = ar::camera_key(active, 3u, 0u, 1u);
    CHECK(c0.id == 40u);
    CHECK(c0.index == 0u);
    const ar::SamplerKey l = ar::light_key(5u, 3u);
    CHECK(l.id == 0x80000005u);
    CHECK(l.index == 3u);
    CHECK(ar::pixel_id(3u, 2u, 1920u) == 3843u);

    // Every constant of dims.h against the table of 0003 d23 (dims 0-7 per path kind,
    // the 12-dimension event block from 8 + 12e, stream ids): a swapped pair fails.
    struct Pin { const char* name; uint32_t got, want; };
#define PIN(n, v) Pin{#n, ar::n, v}
    const Pin pins[] = {
        PIN(DIM_CAM_FILTER_X, 0),      PIN(DIM_CAM_FILTER_Y, 1),    PIN(DIM_CAM_LENS_U, 2),
        PIN(DIM_CAM_LENS_V, 3),        PIN(DIM_CAM_TIME, 4),        PIN(DIM_CAM_WAVELENGTH, 5),
        PIN(DIM_CAM_HERO, 6),          PIN(DIM_CAM_UNUSED7, 7),     PIN(DIM_LIGHT_PICK, 0),
        PIN(DIM_LIGHT_POS_U, 1),       PIN(DIM_LIGHT_POS_V, 2),     PIN(DIM_LIGHT_TIME, 3),
        PIN(DIM_LIGHT_DIR_U, 4),       PIN(DIM_LIGHT_DIR_V, 5),     PIN(DIM_LIGHT_WAVELENGTH, 6),
        PIN(DIM_LIGHT_HERO, 7),        PIN(DIM_EVENT_FIRST, 8),     PIN(DIM_EVENT_COUNT, 12),
        PIN(EV_BSDF_U, 0),             PIN(EV_BSDF_V, 1),           PIN(EV_LOBE_CHOICE, 2),
        PIN(EV_ROULETTE, 3),           PIN(EV_LIGHT_U, 4),          PIN(EV_LIGHT_V, 5),
        PIN(EV_LIGHT_PICK, 6),         PIN(EV_DIST_CHOICE, 7),      PIN(EV_CONN_LENS_U, 4),
        PIN(EV_CONN_LENS_V, 5),        PIN(EV_LIGHT_PATH_UNUSED6, 6), PIN(EV_EXT_CHOICE, 8),
        PIN(EV_EXT_VALUE, 9),          PIN(EV_EQUIANGULAR, 10),     PIN(EV_MARCHER_OFFSET, 11),
        PIN(STREAM_FIXED, 0),          PIN(STREAM_PATH_MEDIUM, 1),  PIN(STREAM_SHADOW_MEDIUM, 2),
        PIN(STREAM_RANDOM_WALK, 3)};
#undef PIN
    CHECK(sizeof(pins) / sizeof(pins[0]) == 37u);
    for (const Pin& p : pins) {
      INFO(p.name);
      CHECK(p.got == p.want);
    }
    CHECK(ar::event_dim(0, 0) == 8u);
    CHECK(ar::event_dim(2, 11) == 43u);
    CHECK(ar::event_dim(3, ar::EV_EXT_CHOICE) == 52u);
    // Every event block starts on a 4-D block boundary and holds three blocks.
    for (uint32_t e = 0; e < 8; ++e) CHECK(ar::event_dim(e, 0) % 4u == 0u);
    CHECK(ar::event_dim(1, 0) - ar::event_dim(0, 0) == 12u);
    CHECK(ar::stream_dim(5u, 2u) == 22u);
  }
}

// The identity check repeats in the gate suite because `--tier=gate` is the acceptance
// command and must print its comparison count.
TEST_SUITE("gate") {
  AR_RT_CASE("sampler/f32 f64 bitwise") { bitwise_identity(); }
}

// The statistical cases are bit-identical across builds, so only the f32 copy runs them.
#if !defined(AR_REAL_DOUBLE)
namespace {

struct Pair { uint32_t a, b; };
// 16 pairs of dimensions in different 4-D blocks, spanning dims 0-7 and three event blocks.
constexpr Pair kPairs[16] = {{0, 4},  {1, 5},  {2, 6},  {3, 7},  {0, 8},  {2, 10}, {4, 12}, {6, 14},
                             {8, 12}, {9, 17}, {11, 19}, {3, 20}, {7, 24}, {10, 25}, {5, 30}, {1, 31}};

uint32_t cell(ar::Real u, uint32_t n) { return static_cast<uint32_t>(u * static_cast<ar::Real>(n)); }

}  // namespace

TEST_SUITE("gate") {
  TEST_CASE("sampler/chi2 cross-block pairs") {
    const st::Run run = st::begin("sampler/chi2 cross-block pairs");
    for (uint32_t seed : {0u, 12345u})
      for (const Pair& pr : kPairs) {
        const uint32_t id_base = static_cast<uint32_t>(run.seed);
        // 16 samples per pixel, as one pass block of the adaptive tail; 16 x 16 cells.
        const st::Chi2Result r = st::chi2_with_retry(
            run,
            [&](int pixels) {
              st::Histogram h;
              h.observed.assign(256, 0.0);
              h.expected.assign(256, double(pixels) * 16.0 / 256.0);
              for (int p = 0; p < pixels; ++p)
                for (uint32_t i = 0; i < 16; ++i) {
                  const uint32_t id = id_base + static_cast<uint32_t>(p);
                  const uint32_t ca = cell(ar::sample(id, i, 0, pr.a, seed), 16);
                  const uint32_t cb = cell(ar::sample(id, i, 0, pr.b, seed), 16);
                  h.observed[ca * 16 + cb] += 1.0;
                }
              return h;
            },
            8192);
        INFO("seed " << seed << " dims " << pr.a << "," << pr.b << ": " << r.message);
        CHECK(r.pass);
      }
  }

  TEST_CASE("sampler/hero lane independence") {
    // 0003 check 14: chi-squared independence of (h, floor(4u_wavelength)) over 2^16 paths.
    const st::Run run = st::begin("sampler/hero lane independence");
    for (uint32_t seed : {0u, 12345u}) {
      auto p_value = [&](uint32_t paths) {
        double t[4][4] = {};
        for (uint32_t p = 0; p < paths; ++p) {
          const uint32_t id = static_cast<uint32_t>(run.seed) + (p >> 4);
          const uint32_t w = cell(ar::sample(id, p & 15u, 0, ar::DIM_CAM_WAVELENGTH, seed), 4);
          const uint32_t h = cell(ar::sample(id, p & 15u, 0, ar::DIM_CAM_HERO, seed), 4);
          t[h][w] += 1.0;
        }
        double row[4] = {}, col[4] = {};
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j) {
            row[i] += t[i][j];
            col[j] += t[i][j];
          }
        double chi2 = 0.0;
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j) {
            const double e = row[i] * col[j] / double(paths);
            chi2 += (t[i][j] - e) * (t[i][j] - e) / e;
          }
        return st::chi2_pvalue(chi2, 9);  // (4 - 1)(4 - 1) degrees of freedom
      };
      run.family->note_check();
      double p = p_value(1u << 16);
      if (p < run.family->alpha()) p = p_value(1u << 17);  // 0009 d15: re-run at 2x
      INFO("seed " << seed << " p " << p << " alpha " << run.family->alpha() << "; " << st::reproduce(run));
      CHECK(p >= run.family->alpha());
    }
  }

  TEST_CASE("sampler/independent lag1 correlation") {
    // 0003 check 25: per-pixel lag-1 correlation of independent mode is within the gate's
    // bound of 0; Sobol mode is not, which shows the statistic can see a correlation.
    const st::Run run = st::begin("sampler/independent lag1 correlation");
    constexpr uint32_t kPixels = 256, kSamples = 4096;
    auto lag1 = [&](uint32_t mode, uint32_t seed) {
      std::vector<double> r(kPixels);
      std::vector<double> u(kSamples);
      for (uint32_t p = 0; p < kPixels; ++p) {
        double mean = 0.0;
        for (uint32_t i = 0; i < kSamples; ++i) {
          u[i] = static_cast<double>(ar::draw(mode, static_cast<uint32_t>(run.seed) + p, i, 0, 0, seed));
          mean += u[i];
        }
        mean /= kSamples;
        double num = 0.0, den = 0.0;
        for (uint32_t i = 0; i < kSamples; ++i) {
          den += (u[i] - mean) * (u[i] - mean);
          if (i + 1 < kSamples) num += (u[i] - mean) * (u[i + 1] - mean);
        }
        r[p] = num / den;
      }
      return r;
    };
    for (uint32_t seed : {0u, 12345u}) {
      const st::EnergyResult e = st::energy_test(run, lag1(ar::SAMPLER_INDEPENDENT, seed), 0.0);
      INFO("seed " << seed << ": " << e.message);
      CHECK(e.pass);
    }
    const st::EnergyResult s = st::energy_test(run, lag1(ar::SAMPLER_SOBOL, 12345u), 0.0);
    INFO("sobol mode lag-1 mean " << s.mean << " z " << s.z);
    CHECK_FALSE(s.pass);
  }
}
#endif
