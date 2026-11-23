// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// The environment importance table (0004 D11, Verification row D7/D11, ruling 88): equal-area
// mapping, piecewise-constant sampling and pdf, the host builder.
// Ref: Clarberg 2008 (the mapping, here re-evaluated in double with std::atan2 instead of pbrt's
// polynomial); pbrt-v4 util/math.cpp EqualAreaSquareToSphere / EqualAreaSphereToSquare,
// util/sampling.h PiecewiseConstant2D, lights.cpp ImageInfiniteLight (compensated function);
// Van Oosterom and Strackee 1983 (solid angle of a spherical triangle, for the cell areas).
//
// Mapping error. pbrt states no figure for its atan polynomial; measured here (polynomial_error)
// as 4.07e-6 in phi / (pi / 2), which moves a square coordinate by at most half that. The bound
// for the inverse map against Clarberg's exact formula, and for the square -> sphere -> square
// round trip, is that error plus 1e-6; measured 2.09e-6 (float) and 2.03e-6 (double) on 1e5
// points. The sphere -> square -> sphere trip is bounded by 8 times it plus 1e-6: the map stretches
// square distances by at most 4 sqrt(2) (radially at the equator, dz/dr = 2 and dr/ds = 2 sqrt(2);
// the tangential stretch is 4 pi over the radial one, at most pi), and a per-coordinate error d is
// at most sqrt(2) d long; measured 9.4e-6 (float). Not covered by those 1e5 points: in float, near
// the poles z = 1 - r^2 rounds away most of r^2, so where r (in [-1,1] units) is below about
// 1.5e-3, a fraction near 2e-6 of the square, the round trip exceeds that bound; a scan toward the
// poles measured 8.6e-5 at worst. Ruling 88's e below includes every draw's own round-trip error,
// so such a draw is never judged against a too-small e.
//
// Ruling 88 bound. e = the largest per-coordinate round-trip error, over the 1e5 uniform points
// and over the test's own draws. An n x n grid has 2(n - 1) interior edge lines, each the centre
// of a band of width 2e, so a uniform draw lies within e of an edge with probability at most
// 4e(n - 1). The draws here follow the table, so the band is weighted by cell probability: a cell
// with k_x interior vertical and k_y interior horizontal edges has the fraction
// f = f_x + f_y - f_x f_y of its area in the bands, f_x = k_x e n, and the expected near fraction is
// B = sum_cells P(cell) f (at most 4en; 4e(n - 1) for the uniform table). The printed fraction must
// stay under B + 3 sqrt(B (1 - B) / draws).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/stat.h"
#include "host/light/env_table.h"
#include "light/environment_table.h"

namespace st = astroray::stat;
using astroray::host::build_env_table;
using astroray::host::EnvDistribution;
using astroray::host::EnvTable;
using ar::Real;
using ar::Real2;
using ar::Real3;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct D2 {
  double x, y;
};
struct D3 {
  double x, y, z;
};
D3 d3(Real3 v) { return {double(v.x), double(v.y), double(v.z)}; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 unit3(D3 a) {
  const double l = std::sqrt(dot3(a, a));
  return {a.x / l, a.y / l, a.z / l};
}

ar::EnvTableView view_of(const EnvDistribution& d) {
  return ar::EnvTableView{d.func.data(), d.cdf_u.data(), d.marginal_func.data(), d.marginal_cdf.data(), d.func_integral, d.n};
}

// Clarberg's square-to-sphere formulas in double, independent of the kernel's evaluation order.
D3 ref_square_to_sphere(double px, double py) {
  const double u = 2 * px - 1, v = 2 * py - 1, up = std::fabs(u), vp = std::fabs(v);
  const double sd = 1 - (up + vp), r = 1 - std::fabs(sd);
  const double phi = (r == 0 ? 1 : (vp - up) / r + 1) * kPi / 4;
  const double z = std::copysign(1 - r * r, sd), s = r * std::sqrt(std::max(0.0, 2 - r * r));
  return {std::copysign(std::cos(phi), u) * s, std::copysign(std::sin(phi), v) * s, z};
}

// Clarberg's sphere-to-square with the exact angle atan2(min, max) 2 / pi.
D2 ref_sphere_to_square(D3 d) {
  const double x = std::fabs(d.x), y = std::fabs(d.y), r = std::sqrt(std::max(0.0, 1 - std::fabs(d.z)));
  double phi = std::atan2(std::min(x, y), std::max(x, y)) * 2 / kPi;
  if (x < y) phi = 1 - phi;
  double v = phi * r, u = r - v;
  if (d.z < 0) {
    const double w = u;
    u = 1 - v;
    v = 1 - w;
  }
  return {0.5 * (std::copysign(u, d.x) + 1), 0.5 * (std::copysign(v, d.y) + 1)};
}

// pbrt's polynomial, evaluated in double: its largest deviation from atan(b) 2 / pi on [0,1].
double polynomial_error() {
  const double c[7] = {0.406758566246788489601959989e-5, 0.636226545274016134946890922156, 0.61572017898280213493197203466e-2,
                       -0.247333733281268944196501420480, 0.881770664775316294736387951347e-1,
                       0.419038818029165735901852432784e-1, -0.251390972343483509333252996350e-1};
  double worst = 0.0;
  for (int k = 0; k <= 1000000; ++k) {
    const double b = k / 1e6;
    double p = c[6];
    for (int j = 5; j >= 0; --j) p = p * b + c[j];
    worst = std::max(worst, std::fabs(p - std::atan(b) * 2 / kPi));
  }
  return worst;
}

// Max per-coordinate error of the kernel's square -> sphere -> square round trip over 1e5 uniform
// points (fixed seed): e of ruling 88.
double square_round_trip_error() {
  std::mt19937_64 rng(153);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  double e = 0.0;
  for (int k = 0; k < 100000; ++k) {
    const Real2 p{Real(uni(rng)), Real(uni(rng))};
    const Real2 q = ar::equal_area_sphere_to_square(ar::equal_area_square_to_sphere(p));
    e = std::max(e, std::max(std::fabs(double(q.x) - double(p.x)), std::fabs(double(q.y) - double(p.y))));
  }
  return e;
}

// Solid angle of the spherical triangle a, b, c (unit vectors).
double triangle_solid_angle(D3 a, D3 b, D3 c) {
  return 2.0 * std::atan2(std::fabs(dot3(a, cross3(b, c))), 1.0 + dot3(a, b) + dot3(b, c) + dot3(c, a));
}

// Cell solid angles of an n x n grid under the kernel mapping, by corner quadrature. The mapping is
// smooth inside each of its eight triangles and creased along u = 0, v = 0 (cell edges for even n)
// and |u| + |v| = 1, which runs along cell diagonals: each sub-cell is cut along the diagonal of
// its quadrant's crease so no spherical triangle straddles a crease. With k x k sub-cells per cell,
// each two spherical triangles of mapped corners, the chords of the curved cell edges leave an
// error in even powers of 1 / k (measured: 9.9e-2, 2.8e-2, 7.3e-3, 1.9e-3 relative worst cell for
// k = 1, 2, 4, 8 at n = 64); Romberg's extrapolation over k = 2, 4, 8 removes the 1/k^2 and 1/k^4
// terms. The directions come from one vertex grid of 8n + 1 points per side, shared by every k.
std::vector<double> cell_solid_angles(int n) {
  const int m = 8 * n;
  std::vector<D3> v(size_t(m + 1) * (m + 1));
  for (int j = 0; j <= m; ++j)
    for (int i = 0; i <= m; ++i)
      v[size_t(j) * (m + 1) + i] = unit3(d3(ar::equal_area_square_to_sphere(Real2{Real(double(i) / m), Real(double(j) / m)})));
  const auto at = [&](int i, int j) { return v[size_t(j) * (m + 1) + i]; };
  const auto level = [&](int ci, int cj, int k) {
    const int s = 8 / k;
    double sum = 0.0;
    for (int a = 0; a < k; ++a)
      for (int b = 0; b < k; ++b) {
        const int i0 = ci * 8 + a * s, j0 = cj * 8 + b * s, i1 = i0 + s, j1 = j0 + s;
        const D3 p00 = at(i0, j0), p10 = at(i1, j0), p01 = at(i0, j1), p11 = at(i1, j1);
        const bool same = (i0 + i1 - m) * (j0 + j1 - m) > 0;  // quadrants (+,+) and (-,-)
        sum += same ? triangle_solid_angle(p00, p10, p01) + triangle_solid_angle(p10, p11, p01)
                    : triangle_solid_angle(p00, p10, p11) + triangle_solid_angle(p00, p11, p01);
      }
    return sum;
  };
  std::vector<double> out(size_t(n) * n);
  for (int cj = 0; cj < n; ++cj)
    for (int ci = 0; ci < n; ++ci) {
      const double o2 = level(ci, cj, 2), o4 = level(ci, cj, 4), o8 = level(ci, cj, 8);
      const double r4 = (4.0 * o4 - o2) / 3.0, r8 = (4.0 * o8 - o4) / 3.0;
      out[size_t(cj) * n + ci] = (16.0 * r8 - r4) / 15.0;
    }
  return out;
}

// n = 64 test grid: a smooth positive field, one bright cell, a band of zero rows and scattered
// zero cells.
constexpr uint32_t kN = 64;
std::vector<double> test_grid() {
  std::vector<double> g(size_t(kN) * kN);
  for (uint32_t v = 0; v < kN; ++v)
    for (uint32_t u = 0; u < kN; ++u) {
      double x = 1.0 + 0.5 * std::sin(0.3 * u) * std::cos(0.2 * v);
      if (v >= 20 && v < 24) x = 0.0;
      if ((u * 7 + v * 13) % 17 == 0) x = 0.0;
      g[size_t(v) * kN + u] = x;
    }
  g[size_t(40) * kN + 9] = 2000.0;
  return g;
}

// The compensated function written from pbrt's lines, independent of the builder.
std::vector<double> compensated_of(const std::vector<double>& g) {
  double sum = 0.0;
  for (double v : g) sum += v;
  const double avg = sum / double(g.size());
  std::vector<double> c(g.size());
  bool all_zero = true;
  for (size_t i = 0; i < g.size(); ++i) {
    c[i] = std::max(g[i] - avg, 0.0);
    all_zero = all_zero && c[i] == 0.0;
  }
  if (all_zero) std::fill(c.begin(), c.end(), 1.0);
  return c;
}

bool rel_close(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::max(std::fabs(a), std::fabs(b)); }

// Ruling 88 for one distribution: draws from env_sample, their direction pdf recomputed by
// env_pdf. The bound on the fraction of draws within e of an interior cell edge is derived in the
// file header. e is measured from the code under test, so it is held to the mapping bounds first:
// the uniform-point error to polynomial error + 1e-6, every draw's own error to the header's pole
// figure (1e-4) in float and to the same polynomial bound in double.
void check_direction_pdf(const EnvTable& t, bool allow, double e_uniform, double poly) {
  const ar::EnvTableView pv = view_of(t.plain), cv = view_of(t.compensated);
  const EnvDistribution& dist = allow ? t.compensated : t.plain;
  const uint32_t n = dist.n;
  const int draws = 100000;
  std::mt19937_64 rng(allow ? 8801 : 8800);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  std::vector<ar::EnvDirSample> s(draws);
  double e = e_uniform;
  for (ar::EnvDirSample& d : s) {
    d = ar::env_sample(pv, cv, Real2{Real(uni(rng)), Real(uni(rng))}, allow);
    REQUIRE(d.pdf > Real(0));
    const Real2 q = ar::equal_area_sphere_to_square(d.wi);
    e = std::max({e, std::fabs(double(q.x) - double(d.p.x)), std::fabs(double(q.y) - double(d.p.y))});
  }
  CHECK(e_uniform <= poly + 1e-6);
  CHECK(e <= (sizeof(Real) == 4 ? 1e-4 : poly + 1e-6));
  const auto cell_pdf = [&](uint32_t iu, uint32_t iv) {
    return double(dist.func[size_t(iv) * n + iu]) / double(dist.func_integral) / (4.0 * kPi);
  };
  // Distance of coordinate c to the nearest interior edge, and the step to the cell across it.
  const auto edge = [&](double c, int* step) {
    const double k = std::round(c * n);
    if (k < 1.0 || k > double(n) - 1.0) return 2.0;
    *step = c < k / n ? 1 : -1;
    return std::fabs(c - k / n);
  };
  int near = 0, bad_far = 0, bad_near = 0;
  for (const ar::EnvDirSample& d : s) {
    const double pdf = double(ar::env_pdf(pv, cv, d.wi, allow));
    const uint32_t iu = d.cell % n, iv = d.cell / n;
    int sx = 0, sy = 0;
    const double dx = edge(double(d.p.x), &sx), dy = edge(double(d.p.y), &sy);
    if (std::min(dx, dy) > e) {
      bad_far += !rel_close(pdf, double(d.pdf), 1e-4);
      continue;
    }
    ++near;
    bool ok = rel_close(pdf, double(d.pdf), 1e-4);
    if (dx <= e) ok = ok || rel_close(pdf, cell_pdf(iu + sx, iv), 1e-4);
    if (dy <= e) ok = ok || rel_close(pdf, cell_pdf(iu, iv + sy), 1e-4);
    if (dx <= e && dy <= e) ok = ok || rel_close(pdf, cell_pdf(iu + sx, iv + sy), 1e-4);
    bad_near += !ok;
  }
  double band = 0.0;
  for (uint32_t iv = 0; iv < n; ++iv)
    for (uint32_t iu = 0; iu < n; ++iu) {
      const double fx = std::min(1.0, ((iu > 0) + (iu + 1 < n)) * e * n), fy = std::min(1.0, ((iv > 0) + (iv + 1 < n)) * e * n);
      band += double(dist.func[size_t(iv) * n + iu]) / double(dist.func_integral) / (double(n) * n) * (fx + fy - fx * fy);
    }
  const double bound = band + 3.0 * std::sqrt(band * (1.0 - band) / draws), frac = double(near) / draws;
  std::printf("env_table pdf_li%s %s: e %.3e, near-edge fraction %.3e, bound %.3e (uniform 4e(n-1) %.3e)\n", AR_RT_SUFFIX,
              allow ? "compensated" : "plain", e, frac, bound, 4.0 * e * (n - 1));
  CHECK(bad_far == 0);
  CHECK(bad_near == 0);
  CHECK(frac <= bound);
}

std::string thrown(const std::vector<double>& g, uint32_t n) {
  try {
    (void)build_env_table(g, n);
  } catch (const std::invalid_argument& ex) {
    return ex.what();
  }
  return "";
}

}  // namespace

TEST_SUITE("fast") {

AR_RT_CASE("env_table/mapping round trip") {
  const double poly = polynomial_error();
  std::mt19937_64 rng(1530);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  double e_fwd = 0.0, e_sq = 0.0, e_inv = 0.0, e_dir = 0.0;
  for (int k = 0; k < 100000; ++k) {
    // square -> sphere -> square
    const Real2 p{Real(uni(rng)), Real(uni(rng))};
    const Real3 w = ar::equal_area_square_to_sphere(p);
    const D3 wr = ref_square_to_sphere(double(p.x), double(p.y));
    e_fwd = std::max({e_fwd, std::fabs(double(w.x) - wr.x), std::fabs(double(w.y) - wr.y), std::fabs(double(w.z) - wr.z)});
    const Real2 q = ar::equal_area_sphere_to_square(w);
    e_sq = std::max({e_sq, std::fabs(double(q.x) - double(p.x)), std::fabs(double(q.y) - double(p.y))});
    // sphere -> square -> sphere
    const double z = 1.0 - 2.0 * uni(rng), phi = 2.0 * kPi * uni(rng), s = std::sqrt(std::max(0.0, 1.0 - z * z));
    const Real3 d{Real(s * std::cos(phi)), Real(s * std::sin(phi)), Real(z)};
    const Real2 c = ar::equal_area_sphere_to_square(d);
    const D2 cr = ref_sphere_to_square(d3(d));
    e_inv = std::max({e_inv, std::fabs(double(c.x) - cr.x), std::fabs(double(c.y) - cr.y)});
    const D3 back = d3(ar::equal_area_square_to_sphere(c)), dd = d3(d);
    const D3 diff{back.x - dd.x, back.y - dd.y, back.z - dd.z};
    e_dir = std::max(e_dir, std::sqrt(dot3(diff, diff)));
  }
  std::printf("env_table mapping%s: polynomial error %.3e; forward vs ref %.3e, inverse vs ref %.3e, square trip %.3e, sphere trip %.3e\n",
              AR_RT_SUFFIX, poly, e_fwd, e_inv, e_sq, e_dir);
  const double tol = poly + 1e-6;
  CHECK(e_inv <= tol);
  CHECK(e_sq <= tol);
  CHECK(e_fwd <= 1e-6);
  CHECK(e_dir <= 8.0 * tol + 1e-6);
}

AR_RT_CASE("env_table/mapping is area-preserving") {
  const int n = 64;
  const double expect = 4.0 * kPi / (n * n);
  const std::vector<double> om = cell_solid_angles(n);
  double worst = 0.0, total = 0.0;
  for (double o : om) total += o, worst = std::max(worst, std::fabs(o / expect - 1.0));
  std::printf("env_table area%s: worst cell %.3e relative, total %.9f (4 pi = %.9f)\n", AR_RT_SUFFIX, worst, total, 4 * kPi);
  CHECK(worst <= 1e-5);
}

AR_RT_CASE("env_table/pdf integrates to one") {
  // Sum over cells of the solid-angle pdf at the cell's centre direction times the cell's own
  // solid angle (corner quadrature, independent of the table).
  const EnvTable t = build_env_table(test_grid(), kN);
  const ar::EnvTableView pv = view_of(t.plain), cv = view_of(t.compensated);
  const std::vector<double> om = cell_solid_angles(int(kN));
  for (bool allow : {false, true}) {
    double sum = 0.0;
    for (uint32_t iv = 0; iv < kN; ++iv)
      for (uint32_t iu = 0; iu < kN; ++iu) {
        const Real3 w = ar::equal_area_square_to_sphere(Real2{Real((iu + 0.5) / kN), Real((iv + 0.5) / kN)});
        sum += double(ar::env_pdf(pv, cv, w, allow)) * om[size_t(iv) * kN + iu];
      }
    std::printf("env_table pdf integral%s %s: %.8f\n", AR_RT_SUFFIX, allow ? "compensated" : "plain", sum);
    CHECK(std::fabs(sum - 1.0) <= 1e-4);
  }
}

AR_RT_CASE("env_table/sample pdf equals the pdf at its point") {
  const EnvTable t = build_env_table(test_grid(), kN);
  std::mt19937_64 rng(1531);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  for (const EnvDistribution* d : {&t.plain, &t.compensated}) {
    const ar::EnvTableView v = view_of(*d);
    int bad = 0, zero = 0, cell = 0;
    const auto check = [&](Real2 u) {
      const ar::EnvSquareSample s = ar::piecewise2d_sample(v, u);
      zero += !(s.pdf > Real(0)) || d->func[s.cell] == 0.0f;
      cell += s.cell != ar::piecewise_cell(s.p.y, kN) * kN + ar::piecewise_cell(s.p.x, kN);
      bad += !rel_close(double(s.pdf), double(ar::piecewise2d_pdf(v, s.p)), 1e-6);
    };
    // Every cell's CDF ends: u at a cell's lower CDF value gives du = 0, the float just below its
    // upper value gives du just under 1, where (o + du) / n rounds onto the next cell's edge in float.
    const auto below = [](float c) { return std::nextafter(c, 0.0f); };
    for (uint32_t r = 0; r < kN; ++r)
      for (uint32_t c = 0; c < kN; ++c) {
        const float* row = d->cdf_u.data() + size_t(r) * (kN + 1);
        if (d->func[size_t(r) * kN + c] == 0.0f || d->marginal_func[r] == 0.0f) continue;
        for (float uy : {d->marginal_cdf[r], below(d->marginal_cdf[r + 1])})
          for (float ux : {row[c], below(row[c + 1])}) check(Real2{Real(ux), Real(uy)});
      }
    const float last = 0.99999994f;  // the largest float below 1
    for (int k = 0; k < 4; ++k) check(Real2{Real(k & 1 ? last : 0.0f), Real(k & 2 ? last : 0.0f)});
    for (int k = 0; k < 100000; ++k) check(Real2{Real(uni(rng)), Real(uni(rng))});
    CHECK(zero == 0);
    CHECK(cell == 0);
    CHECK(bad == 0);
  }
}

AR_RT_CASE("env_table/direction pdf matches the sample pdf") {
  const EnvTable t = build_env_table(test_grid(), kN);
  const double e = square_round_trip_error(), poly = polynomial_error();
  check_direction_pdf(t, false, e, poly);
  check_direction_pdf(t, true, e, poly);
}

AR_RT_CASE("env_table/builder arrays and invalid input") {
  const std::vector<double> g = test_grid();
  const EnvTable t = build_env_table(g, kN);
  const std::vector<double> c = compensated_of(g);
  double sum = 0.0;
  for (double v : g) sum += v;
  int func = 0, mono = 0;
  for (size_t i = 0; i < g.size(); ++i) func += t.plain.func[i] != float(g[i]) || t.compensated.func[i] != float(c[i]);
  for (const EnvDistribution* d : {&t.plain, &t.compensated}) {
    for (uint32_t v = 0; v < kN; ++v)
      for (uint32_t i = 0; i < kN; ++i) mono += d->cdf_u[v * (kN + 1) + i + 1] < d->cdf_u[v * (kN + 1) + i];
    CHECK(d->marginal_cdf[0] == 0.0f);
    CHECK(d->marginal_cdf[kN] == 1.0f);
  }
  CHECK(func == 0);
  CHECK(mono == 0);
  CHECK(double(t.plain.func_integral) == doctest::Approx(sum / double(g.size())).epsilon(1e-6));
  CHECK(t.bytes() == 2u * (kN * kN + kN * (kN + 1) + kN + kN + 1) * sizeof(float));

  // A uniform grid: the compensated function is the all-ones fill and both pdfs are uniform.
  const EnvTable u = build_env_table(std::vector<double>(64, 2.0), 8);
  CHECK(std::all_of(u.compensated.func.begin(), u.compensated.func.end(), [](float f) { return f == 1.0f; }));
  CHECK(double(ar::piecewise2d_pdf(view_of(u.plain), Real2{Real(0.3), Real(0.9)})) == doctest::Approx(1.0));
  CHECK(double(ar::piecewise2d_pdf(view_of(u.compensated), Real2{Real(0.7), Real(0.1)})) == doctest::Approx(1.0));
  // Uniform grids whose double mean rounds above (0.1) and below (0.3) the value at n = 64: the
  // compensated table is uniform either way.
  for (double value : {0.1, 0.3}) {
    const EnvTable w = build_env_table(std::vector<double>(size_t(kN) * kN, value), kN);
    for (double x : {0.01, 0.37, 0.99})
      CHECK(double(ar::piecewise2d_pdf(view_of(w.compensated), Real2{Real(x), Real(1.0 - x)})) == doctest::Approx(1.0));
  }

  const auto has = [](const std::string& what, const char* word) { return what.find(word) != std::string::npos; };
  CHECK(has(thrown({1.0}, 0), "n is 0"));
  CHECK(has(thrown({}, 4), "empty"));
  CHECK(has(thrown({1.0, 2.0, 3.0}, 2), "not n * n"));
  CHECK(has(thrown({1.0, -0.5, 1.0, 1.0}, 2), "negative"));
  CHECK(has(thrown({1.0, std::nan(""), 1.0, 1.0}, 2), "not finite"));
  CHECK(has(thrown({1.0, HUGE_VAL, 1.0, 1.0}, 2), "not finite"));
  CHECK(has(thrown({0.0, 0.0, 0.0, 0.0}, 2), "every entry is 0"));
  CHECK(has(thrown({1e39, 0.0, 0.0, 0.0}, 2), "does not fit a float"));
  CHECK(has(thrown({1e-300, 1e-300, 1e-300, 1e-300}, 2), "below the smallest normal float"));
}

}  // TEST_SUITE("fast")

TEST_SUITE("gate") {

// Chi-squared of sampled square points in 16 x 16 bins of the n = 64 table against the bin sums
// of the function the table must represent (the grid, or the compensated function written from
// pbrt's lines in this file), so a builder that mis-forms the function fails too.
static void chi2_table(bool compensated, const char* name) {
  const st::Run run = st::begin(name, st::gate_family());
  const std::vector<double> g = test_grid(), f = compensated ? compensated_of(g) : g;
  const EnvTable t = build_env_table(g, kN);
  const ar::EnvTableView v = view_of(compensated ? t.compensated : t.plain);
  const int bins = 16, per = int(kN) / bins, samples = 1 << 20;
  std::vector<double> observed(size_t(bins) * bins, 0.0), expected(size_t(bins) * bins, 0.0);
  double total = 0.0;
  for (double x : f) total += x;
  for (uint32_t iv = 0; iv < kN; ++iv)
    for (uint32_t iu = 0; iu < kN; ++iu) expected[size_t(iv / per) * bins + iu / per] += samples * f[size_t(iv) * kN + iu] / total;
  std::mt19937_64 rng(run.seed);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  for (int k = 0; k < samples; ++k) {
    const ar::EnvSquareSample s = ar::piecewise2d_sample(v, Real2{Real(uni(rng)), Real(uni(rng))});
    const int bx = std::min(bins - 1, int(double(s.p.x) * bins)), by = std::min(bins - 1, int(double(s.p.y) * bins));
    observed[size_t(by) * bins + bx] += 1.0;
  }
  const st::Chi2Result res = st::chi2_test(run, observed, expected);
  std::printf("env_table chi2 %s: chi2 %.1f dof %d p %.4f\n", name, res.chi2, res.dof, res.p);
  CHECK_MESSAGE(res.pass, res.message);
}

AR_RT_CASE("env_table/chi2 plain") { chi2_table(false, "env_table/chi2 plain" AR_RT_SUFFIX); }
AR_RT_CASE("env_table/chi2 compensated") { chi2_table(true, "env_table/chi2 compensated" AR_RT_SUFFIX); }

}  // TEST_SUITE("gate")
