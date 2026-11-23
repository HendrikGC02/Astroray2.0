// SPDX-License-Identifier: Apache-2.0
// The environment importance table (0004 D11): a piecewise-constant density over the equal-area
// square (equal_area.h), sampled and evaluated from plain arrays the host builds
// (host/light/env_table.h). Two distributions exist for one world: the plain one, proportional to
// the weighted radiance grid, which light-path emission samples, and the MIS-compensated one, which
// NEE samples because BSDF sampling already covers the dim directions (allow_incomplete). Radiance
// at a sampled direction always comes from the world program, never from the table.
//
// Ref: third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/util/sampling.h PiecewiseConstant1D
// (Sample), PiecewiseConstant2D (Sample, PDF) with domain [0,1]^2, util/math.h FindInterval;
// src/pbrt/lights.h ImageInfiniteLight::SampleLi and lights.cpp ImageInfiniteLight::PDF_Li
// (mapPDF / (4 pi), compensatedDistribution when allowIncompletePDF) (Apache-2.0); Karlik, Sik,
// Vevoda, Skrivan, Krivanek 2019, "MIS compensation: optimizing sampling techniques in multiple
// importance sampling", ACM TOG 38(6).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/vec.h"
#include "../portability.h"
#include "equal_area.h"

AR_NS_BEGIN

// One PiecewiseConstant2D over [0,1]^2 with n x n cells, laid out as pbrt keeps it; cell (iu, iv)
// covers p.x in [iu/n, (iu+1)/n) and p.y in [iv/n, (iv+1)/n), and its index is iv n + iu.
//   func           n n     the function, row iv at [iv n, iv n + n)   (pConditionalV[iv].func)
//   cdf_u          n (n+1) row iv's conditional CDF at [iv (n+1), ...) (pConditionalV[iv].cdf)
//   marginal_func  n       row integrals                              (pMarginal.func)
//   marginal_cdf   n+1     the marginal CDF over rows                 (pMarginal.cdf)
//   func_integral          integral of func over the square            (pMarginal.funcInt)
// A density over the square; a density over directions divides it by 4 pi (equal_area.h).
struct EnvTableView {
  const float* func;
  const float* cdf_u;
  const float* marginal_func;
  const float* marginal_cdf;
  float func_integral;
  uint32_t n;
};

struct EnvSquareSample {
  Real2 p;
  Real pdf;       // over the square; 0 when the table has no mass
  uint32_t cell;  // iv n + iu
};

struct EnvDirSample {
  Real3 wi;
  Real pdf;  // over solid angle; 0 means no sample
  Real2 p;
  uint32_t cell;
};

inline constexpr Real kEnvInv4Pi = Real(1) / (Real(4) * Real(kPiDouble));

// The cell of coordinate x in [0,1] among n: pbrt's PDF lookup Clamp(int(x n), 0, n - 1).
AR_FN uint32_t piecewise_cell(Real x, uint32_t n) {
  const Real c = x * Real(n);
  if (!(c > Real(0))) return 0u;
  const uint32_t i = uint32_t(c);
  return i < n ? i : n - 1u;
}

// Neighbouring representable values of a positive finite x.
AR_FN float env_step(float x, int dir) { return bit_cast<float>(uint32_t(int64_t(bit_cast<uint32_t>(x)) + dir)); }
AR_FN double env_step(double x, int dir) { return bit_cast<double>(uint64_t(int64_t(bit_cast<uint64_t>(x)) + dir)); }

// pbrt's FindInterval(size, cdf[i] <= u): the largest i in [0, size - 2] with cdf[i] <= u, by
// binary search. A run of equal CDF values (zero cells) resolves to its last index, so a cell of
// zero function is never returned while the row has mass.
AR_FN uint32_t piecewise_find_interval(const float* cdf, uint32_t size, Real u) {
  int64_t len = int64_t(size) - 2, first = 1;
  while (len > 0) {
    const int64_t half = len >> 1, middle = first + half;
    const bool pred = Real(load_ro(cdf + middle)) <= u;
    first = pred ? middle + 1 : first;
    len = pred ? len - (half + 1) : half;
  }
  const int64_t i = first - 1;
  return uint32_t(i < 0 ? 0 : (i > int64_t(size) - 2 ? int64_t(size) - 2 : i));
}

struct Piecewise1DSample {
  Real x, pdf;
  uint32_t offset;
};

// pbrt's PiecewiseConstant1D::Sample over [0,1]: n cells, cdf with n + 1 entries, integral
// func_int. Rounding of (o + du) / n can land on the edge of the next cell, whose pdf lookup
// (piecewise_cell) differs from the pdf reported here; the point is stepped back inside cell o
// so a sample and the pdf at its own point always agree.
AR_FN Piecewise1DSample piecewise1d_sample(const float* func, const float* cdf, Real func_int, uint32_t n, Real u) {
  Piecewise1DSample s;
  const uint32_t o = piecewise_find_interval(cdf, n + 1u, u);
  const Real c0 = Real(load_ro(cdf + o)), c1 = Real(load_ro(cdf + o + 1u));
  Real du = u - c0;
  if (c1 - c0 > Real(0)) du /= c1 - c0;
  s.pdf = func_int > Real(0) ? Real(load_ro(func + o)) / func_int : Real(0);
  Real x = (Real(o) + du) / Real(n);
  while (x > Real(0) && piecewise_cell(x, n) > o) x = env_step(x, -1);
  while (piecewise_cell(x, n) < o) x = env_step(x, +1);
  s.x = x;
  s.offset = o;
  return s;
}

// pbrt's PiecewiseConstant2D::Sample: the row from the marginal with u.y, the column from that
// row's conditional with u.x; pdf = pdf_u pdf_v. An empty view (n = 0) gives pdf 0.
AR_FN EnvSquareSample piecewise2d_sample(const EnvTableView& t, Real2 u) {
  EnvSquareSample s;
  s.p = Real2{Real(0), Real(0)};
  s.pdf = Real(0);
  s.cell = 0u;
  if (t.n == 0u) return s;
  const Piecewise1DSample mv = piecewise1d_sample(t.marginal_func, t.marginal_cdf, Real(t.func_integral), t.n, u.y);
  const uint32_t row = mv.offset;
  const Piecewise1DSample mu = piecewise1d_sample(t.func + row * t.n, t.cdf_u + row * (t.n + 1u),
                                                  Real(load_ro(t.marginal_func + row)), t.n, u.x);
  s.p = Real2{mu.x, mv.x};
  s.pdf = mu.pdf * mv.pdf;
  s.cell = row * t.n + mu.offset;
  return s;
}

// pbrt's PiecewiseConstant2D::PDF: func of the cell containing p over the integral.
AR_FN Real piecewise2d_pdf(const EnvTableView& t, Real2 p) {
  if (t.n == 0u || !(t.func_integral > 0.0f)) return Real(0);
  const uint32_t iu = piecewise_cell(p.x, t.n), iv = piecewise_cell(p.y, t.n);
  return Real(load_ro(t.func + iv * t.n + iu)) / Real(t.func_integral);
}

// ImageInfiniteLight::SampleLi without the radiance: a direction from the compensated table when
// allow_incomplete (NEE), else from the plain one (light-path emission), pdf over solid angle.
AR_FN EnvDirSample env_sample(const EnvTableView& plain, const EnvTableView& compensated, Real2 u, bool allow_incomplete) {
  const EnvSquareSample q = piecewise2d_sample(allow_incomplete ? compensated : plain, u);
  EnvDirSample s;
  s.p = q.p;
  s.cell = q.cell;
  s.pdf = q.pdf * kEnvInv4Pi;
  s.wi = q.pdf > Real(0) ? equal_area_square_to_sphere(q.p) : Real3{Real(0), Real(0), Real(1)};
  return s;
}

// ImageInfiniteLight::PDF_Li: the density over solid angle of direction `dir` (unit, world
// frame) under the distribution env_sample draws from with the same flag.
AR_FN Real env_pdf(const EnvTableView& plain, const EnvTableView& compensated, Real3 dir, bool allow_incomplete) {
  return piecewise2d_pdf(allow_incomplete ? compensated : plain, equal_area_sphere_to_square(dir)) * kEnvInv4Pi;
}

AR_NS_END
