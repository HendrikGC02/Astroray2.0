// SPDX-License-Identifier: Apache-2.0
// Pixel filters (0008 d5, d4, V3). Expected values are independent of the code
// under test: the densities below are written as closed forms in Blender's
// width w (derived by hand from Cycles' filter_func_* and width mapping), and
// the pinned literals come from float32 / float64 numpy models written
// separately from the C++ (their recipe is stated at each table).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "common/stat.h"
#include "film/filter.h"
#include "host/film/filter_table.h"

namespace st = astroray::stat;
namespace host = astroray::host;

namespace {

using host::FilterSettings;
using host::FilterType;

const double kPi = 3.14159265358979323846;

struct Density {
  double radius;                       // support half-width in pixels
  std::function<double(double)> f;     // un-normalised filter at offset v
};

// Cycles: Gaussian uses width' = 3w and exp(-2 (6 v / width')^2) = exp(-8 v^2 / w^2);
// Blackman-Harris uses width' = 2w and 0.35875 - 0.48829 cos(x) + ... with
// x = 2 pi (v / (2w) + 1/2), which is 0.35875 + 0.48829 cos(pi v / w) + ...
// Box is flat over w.
Density density_of(FilterType type, double w) {
  switch (type) {
    case FilterType::Box:
      return {w / 2, [](double) { return 1.0; }};
    case FilterType::Gaussian:
      return {1.5 * w, [w](double v) { return std::exp(-8.0 * v * v / (w * w)); }};
    case FilterType::BlackmanHarris:
      return {w, [w](double v) {
                return 0.35875 + 0.48829 * std::cos(kPi * v / w) + 0.14128 * std::cos(2 * kPi * v / w) +
                       0.01168 * std::cos(3 * kPi * v / w);
              }};
  }
  return {0, nullptr};
}

// Simpson's rule on [a, b] with 32 panels.
double integrate(const std::function<double(double)>& f, double a, double b) {
  const int n = 32;
  const double h = (b - a) / n;
  double s = f(a) + f(b);
  for (int i = 1; i < n; ++i) s += f(a + i * h) * (i % 2 ? 4.0 : 2.0);
  return s * h / 3.0;
}

// Cell edges at the k / cells quantiles of the density (cumulative trapezoid on 2^16
// panels, inverted by linear interpolation). Equal-mass cells because a 1024-entry
// inverse-CDF table (Cycles' size) spreads the last 1/1023 of the mass uniformly over
// its final step, which is up to a pixel wide in the Gaussian tail; cells wider than
// that step see only the density, as 0008 V3 means.
std::vector<double> quantile_edges(const Density& d, int cells) {
  const int m = 1 << 16;
  const double h = 2 * d.radius / m;
  std::vector<double> cum(m + 1, 0.0);
  for (int i = 0; i < m; ++i)
    cum[i + 1] = cum[i] + 0.5 * h * (d.f(-d.radius + i * h) + d.f(-d.radius + (i + 1) * h));
  std::vector<double> e(cells + 1);
  e[0] = -d.radius;
  e[cells] = d.radius;
  int i = 0;
  for (int k = 1; k < cells; ++k) {
    const double target = cum[m] * k / cells;
    while (cum[i + 1] < target) ++i;
    e[k] = -d.radius + (i + (target - cum[i]) / (cum[i + 1] - cum[i])) * h;
  }
  return e;
}

// 0008 V3: offsets of 2^20 samples against the filter density integrated per cell.
void chi2_case(const char* name, FilterType type) {
  const double w = 1.5;
  const Density d = density_of(type, w);
  const ar::FilterTable table = host::build_filter_table(FilterSettings{type, static_cast<float>(w)});
  CHECK(static_cast<double>(table.radius) == doctest::Approx(d.radius).epsilon(1e-6));
  const st::Run run = st::begin(name);
  for (int axis = 0; axis < 2; ++axis) {
    const auto gen = [&](int cells) {
      std::mt19937_64 rng(run.seed + 0x9E3779B97F4A7C15ull * (static_cast<uint64_t>(cells) * 2 + axis));
      std::uniform_real_distribution<double> uni(0.0, 1.0);
      const int n = 1 << 20;
      const std::vector<double> edges = quantile_edges(d, cells);
      st::Histogram h{std::vector<double>(cells, 0.0), std::vector<double>(cells, 0.0)};
      int outside = 0;
      for (int i = 0; i < n; ++i) {
        const ar::FilterOffset o = ar::sample_filter(table, static_cast<ar::Real>(uni(rng)),
                                                     static_cast<ar::Real>(uni(rng)));
        const double v = axis == 0 ? o.dx : o.dy;
        if (std::fabs(v) > d.radius * (1.0 + 1e-5)) ++outside;
        int c = static_cast<int>(std::upper_bound(edges.begin(), edges.end(), v) - edges.begin()) - 1;
        c = c < 0 ? 0 : (c >= cells ? cells - 1 : c);
        h.observed[c] += 1.0;
      }
      CHECK(outside == 0);
      double total = 0.0;
      for (int c = 0; c < cells; ++c) {
        h.expected[c] = integrate(d.f, edges[c], edges[c + 1]);
        total += h.expected[c];
      }
      for (double& e : h.expected) e *= n / total;
      return h;
    };
    const st::Chi2Result r = st::chi2_with_retry(run, gen, 64);
    INFO(r.message);
    CHECK(r.pass);
  }
}

struct TablePin {
  FilterType type;
  float width;
  float v[10];  // entries at kIdx
  double sum;   // sum of all 1024 entries
};
const int kIdx[10] = {0, 1, 2, 100, 511, 512, 900, 1021, 1022, 1023};

// Pinned from a float32 numpy model of Cycles' filter functions, width mapping and
// util_cdf_evaluate followed by the inversion of filter_table.cpp (u_j = j / 1023,
// half CDF mirrored); entries 0 and 1023 are the support edges 0.5 -/+ radius.
const TablePin kPins[] = {
    {FilterType::Box, 0.5f, {0.25f, 0.250488758f, 0.250977516f, 0.298875868f, 0.499755621f, 0.500244379f, 0.689882696f, 0.749022484f, 0.749511242f, 0.75f}, 512},
    {FilterType::Box, 1.5f, {-0.25f, -0.248533726f, -0.247067451f, -0.103372455f, 0.499266863f, 0.500733137f, 1.06964803f, 1.24706745f, 1.24853373f, 1.25f}, 511.999997},
    {FilterType::Box, 3.0f, {-1.0f, -0.997067451f, -0.994134903f, -0.706744909f, 0.498533726f, 0.501466274f, 1.63929617f, 1.9941349f, 1.99706745f, 2.0f}, 512},
    {FilterType::Gaussian, 0.5f, {-0.25f, 0.112993896f, 0.139421374f, 0.338148504f, 0.499846637f, 0.500153363f, 0.64677453f, 0.860578656f, 0.887006104f, 1.25f}, 512},
    {FilterType::Gaussian, 1.5f, {-1.75f, -0.661018372f, -0.581735849f, 0.0144455135f, 0.499539942f, 0.500460029f, 0.940323472f, 1.58173585f, 1.66101837f, 2.75f}, 512.000002},
    {FilterType::Gaussian, 3.0f, {-4.0f, -1.82203674f, -1.6634717f, -0.471108973f, 0.499079913f, 0.500920117f, 1.38064694f, 2.6634717f, 2.82203674f, 5.0f}, 512.000002},
    {FilterType::BlackmanHarris, 0.5f, {0.0f, 0.104578823f, 0.125395417f, 0.318074942f, 0.499824584f, 0.500175416f, 0.665510297f, 0.874604583f, 0.895421147f, 1.0f}, 512},
    {FilterType::BlackmanHarris, 1.5f, {-1.0f, -0.686263561f, -0.623813748f, -0.0457751751f, 0.49947378f, 0.500526249f, 0.99653095f, 1.62381375f, 1.68626356f, 2.0f}, 512.000001},
    {FilterType::BlackmanHarris, 3.0f, {-2.5f, -1.87252712f, -1.7476275f, -0.59155035f, 0.498947531f, 0.501052439f, 1.4930619f, 2.7476275f, 2.87252712f, 3.5f}, 512.000001},
};

// float64 reference splat weights for a splat at (10.3, 7.8), width 1.5:
// f(x - c_x) f(y - c_y) / I^2 with f the closed forms of density_of and the closed-form
// integrals I = 2 * 0.35875 * w (Blackman-Harris) and w sqrt(pi / 8) erf(1.5 sqrt 8)
// (Gaussian), centres c = i + 0.5 (python math module, float64).
struct WeightPin {
  FilterType type;
  int px, py;
  double weight;
};
const WeightPin kWeights[] = {
    {FilterType::BlackmanHarris, 10, 7, 0.618836665}, {FilterType::BlackmanHarris, 9, 7, 0.118649648},
    {FilterType::BlackmanHarris, 11, 8, 0.002540878}, {FilterType::BlackmanHarris, 9, 6, 0.000538122},
    {FilterType::Gaussian, 10, 7, 0.712880996},       {FilterType::Gaussian, 9, 8, 0.020363803},
};

// Integral of the un-normalised filter over its support, closed forms.
double integral_of(FilterType type, double w) {
  switch (type) {
    case FilterType::Box: return w;
    case FilterType::Gaussian: return w * std::sqrt(kPi / 8.0) * std::erf(1.5 * std::sqrt(8.0));
    case FilterType::BlackmanHarris: return 2 * 0.35875 * w;
  }
  return 0.0;
}

}  // namespace

TEST_SUITE("gate") {
  TEST_CASE("filter/box chi2") { chi2_case("filter/box chi2", FilterType::Box); }
  TEST_CASE("filter/gaussian chi2") { chi2_case("filter/gaussian chi2", FilterType::Gaussian); }
  TEST_CASE("filter/blackman harris chi2") { chi2_case("filter/blackman harris chi2", FilterType::BlackmanHarris); }
}

TEST_SUITE("fast") {
  TEST_CASE("filter/table equals Cycles formulas") {
    for (const TablePin& p : kPins) {
      const ar::FilterTable t = host::build_filter_table(FilterSettings{p.type, p.width});
      for (int k = 0; k < 10; ++k) {
        INFO("type " << static_cast<int>(p.type) << " width " << p.width << " entry " << kIdx[k]);
        CHECK(std::fabs(t.inv_cdf[kIdx[k]] - p.v[k]) < 1e-6);
      }
      double sum = 0.0;
      for (int j = 0; j < ar::kFilterTableSize; ++j) {
        sum += t.inv_cdf[j];
        // The offset distribution is symmetric about the pixel centre.
        CHECK(std::fabs(t.inv_cdf[j] + t.inv_cdf[ar::kFilterTableSize - 1 - j] - 1.0) < 2e-6);
      }
      CHECK(std::fabs(sum - p.sum) < 1e-4);
    }
  }

  TEST_CASE("filter/default is blackman harris 1.5") {
    const ar::FilterTable d = host::build_filter_table(FilterSettings{});
    const ar::FilterTable b = host::build_filter_table(FilterSettings{FilterType::BlackmanHarris, 1.5f});
    CHECK(d.radius == 1.5f);
    for (int j = 0; j < ar::kFilterTableSize; ++j) CHECK(d.inv_cdf[j] == b.inv_cdf[j]);
    CHECK(std::fabs(d.inv_cdf[0] - (-1.0f)) < 1e-6);  // Cycles: width 2 * 1.5, edge at 0.5 - 1.5
    CHECK_THROWS(host::build_filter_table(FilterSettings{FilterType::Box, 0.0f}));
    CHECK_THROWS(host::build_filter_table(FilterSettings{FilterType::Box, -1.0f}));
    CHECK_THROWS(host::build_filter_table(FilterSettings{static_cast<FilterType>(7), 1.0f}));
  }

  TEST_CASE("filter/sample_filter reads the table") {
    const ar::FilterTable t = host::build_filter_table(FilterSettings{FilterType::Gaussian, 1.5f});
    // u = j / 1023 hits entry j; dx and dy use u0 and u1 independently.
    const ar::FilterOffset a = ar::sample_filter(t, ar::Real(100) / ar::Real(1023), ar::Real(900) / ar::Real(1023));
    CHECK(std::fabs(a.dx - (0.0144455135 - 0.5)) < 1e-5);
    CHECK(std::fabs(a.dy - (0.940323472 - 0.5)) < 1e-5);
    const ar::FilterOffset mid = ar::sample_filter(t, ar::Real(0.5), ar::Real(0.5));
    CHECK(std::fabs(mid.dx) < 1e-3);
    const ar::FilterOffset lo = ar::sample_filter(t, ar::Real(0), ar::Real(0));
    CHECK(std::fabs(lo.dx + 2.25) < 1e-6);
  }

  TEST_CASE("filter/density is the closed form at unit integral") {
    // 1-D integral over the support is 1, so f(0) = 1 / int f. Closed forms:
    // Box 1/w; Gaussian 1 / (w sqrt(pi/8) erf(1.5 sqrt 8)); Blackman-Harris 1 / (2 w 0.35875).
    const double w = 1.5;
    const double want[3] = {1.0 / w, 1.0 / (w * std::sqrt(kPi / 8.0) * std::erf(1.5 * std::sqrt(8.0))),
                            1.0 / (2 * w * 0.35875)};
    const FilterType types[3] = {FilterType::Box, FilterType::Gaussian, FilterType::BlackmanHarris};
    for (int i = 0; i < 3; ++i) {
      const ar::FilterTable t = host::build_filter_table(FilterSettings{types[i], static_cast<float>(w)});
      CHECK(ar::filter_density(t, ar::Real(0)) == doctest::Approx(want[i]).epsilon(1e-5));
      CHECK(ar::filter_density(t, ar::Real(t.radius) * ar::Real(1.001)) == ar::Real(0));
    }
  }

  TEST_CASE("filter/footprint weights are unit-integral filter") {
    // 0008 d4: the weight of every touched pixel is f(dx) f(dy) / I^2 at its centre.
    const FilterType types[3] = {FilterType::Box, FilterType::Gaussian, FilterType::BlackmanHarris};
    const float widths[3] = {1.0f, 1.5f, 3.0f};
    const double pos[6][2] = {{10.3, 7.8}, {0.0, 0.0}, {3.25, 99.5}, {5.9999, 2.0001}, {-1.7, 4.4}, {63.123, 0.5}};
    for (FilterType type : types) {
      for (float w : widths) {
        const ar::FilterTable t = host::build_filter_table(FilterSettings{type, w});
        const Density d = density_of(type, w);
        const double inv_i2 = 1.0 / (integral_of(type, w) * integral_of(type, w));
        for (const auto& pd : pos) {
          // The kernel sees the position rounded to Real; the reference uses the same value.
          const double p[2] = {static_cast<double>(ar::Real(pd[0])), static_cast<double>(ar::Real(pd[1]))};
          INFO("type " << static_cast<int>(type) << " width " << w << " at " << p[0] << "," << p[1]);
          const ar::SplatFootprint f = ar::splat_footprint(t, ar::Real(p[0]), ar::Real(p[1]));
          // The pixel list equals a brute-force scan of centres inside the support.
          auto scan = [&](double x, int first, int count) {
            int lo = 1 << 30, n = 0;
            for (int i = static_cast<int>(std::floor(x - d.radius)) - 3; i < static_cast<int>(std::floor(x + d.radius)) + 4; ++i) {
              const double dd = x - (i + 0.5);
              if (dd >= -d.radius && dd < d.radius) {
                lo = lo < i ? lo : i;
                ++n;
              }
            }
            CHECK(n == count);
            CHECK(lo == first);
          };
          scan(p[0], f.x0, f.nx);
          scan(p[1], f.y0, f.ny);
          double worst = 0.0;
          for (int j = 0; j < f.ny; ++j)
            for (int i = 0; i < f.nx; ++i) {
              const double want = d.f(p[0] - (f.x0 + i + 0.5)) * d.f(p[1] - (f.y0 + j + 0.5)) * inv_i2;
              worst = std::fmax(worst, std::fabs(ar::splat_tap_weight(t, f, i, j) - want));
            }
          CHECK(worst < 1e-6);
        }
      }
    }
  }

  TEST_CASE("filter/splat weights equal pinned float64 values") {
    for (const WeightPin& p : kWeights) {
      const ar::FilterTable t = host::build_filter_table(FilterSettings{p.type, 1.5f});
      const ar::SplatFootprint f = ar::splat_footprint(t, ar::Real(10.3), ar::Real(7.8));
      INFO("type " << static_cast<int>(p.type) << " pixel " << p.px << "," << p.py);
      REQUIRE(p.px >= f.x0);
      REQUIRE(p.px < f.x0 + f.nx);
      REQUIRE(p.py >= f.y0);
      REQUIRE(p.py < f.y0 + f.ny);
      CHECK(std::fabs(ar::splat_tap_weight(t, f, p.px - f.x0, p.py - f.y0) - p.weight) < 1e-6);
    }
  }

  TEST_CASE("filter/box at 1 px is one pixel") {
    const ar::FilterTable t = host::build_filter_table(FilterSettings{FilterType::Box, 1.0f});
    CHECK(t.radius == 0.5f);
    // Samples: the offset stays inside the pixel, which spans [-0.5, 0.5] around its centre.
    const int n = 4096;
    bool inside = true;
    for (int a = 0; a <= n; ++a) {
      for (int b = 0; b <= n; b += 257) {
        const ar::FilterOffset o = ar::sample_filter(t, ar::Real(a) / ar::Real(n), ar::Real(b) / ar::Real(n));
        inside = inside && o.dx >= ar::Real(-0.5) && o.dx <= ar::Real(0.5) && o.dy >= ar::Real(-0.5) &&
                 o.dy <= ar::Real(0.5);
      }
    }
    CHECK(inside);
    // The offset is uniform: the mean of a regular grid of u is the centre.
    double mean = 0.0;
    for (int a = 0; a < n; ++a) mean += ar::sample_filter(t, (ar::Real(a) + ar::Real(0.5)) / ar::Real(n), ar::Real(0)).dx;
    CHECK(std::fabs(mean / n) < 1e-4);
    // Splats: one pixel, weight 1, the pixel that contains the position (pixel i spans [i, i + 1)),
    // including positions on pixel edges.
    std::vector<double> xs = {3.0, 2.9999998, 3.0000002, 0.0, -0.0, -1.0, 7.5, 7.4999995, 100.0, 12.999999};
    for (int k = 0; k < 2000; ++k) xs.push_back(-20.0 + 0.0371 * k);
    for (size_t k = 0; k < xs.size(); ++k) {
      const double x = xs[k], y = xs[xs.size() - 1 - k];
      const ar::SplatFootprint f = ar::splat_footprint(t, ar::Real(x), ar::Real(y));
      INFO("splat at " << x << "," << y);
      REQUIRE(f.nx == 1);
      REQUIRE(f.ny == 1);
      CHECK(f.x0 == static_cast<int>(std::floor(static_cast<float>(x))));
      CHECK(f.y0 == static_cast<int>(std::floor(static_cast<float>(y))));
      CHECK(ar::splat_tap_weight(t, f, 0, 0) == ar::Real(1));
    }
  }
}
