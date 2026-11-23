// SPDX-License-Identifier: Apache-2.0
#include "host/light/env_table.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>
#include <string>

namespace astroray::host {

namespace {

// pbrt's PiecewiseConstant1D constructor over [0,1] for f[0..n): cdf[i] = cdf[i-1] + f[i-1] / n,
// integral = cdf[n], then cdf / integral, or i / n when the integral is 0. Returns the integral.
double piecewise1d(const double* f, uint32_t n, double* cdf) {
  cdf[0] = 0.0;
  for (uint32_t i = 1; i <= n; ++i) cdf[i] = cdf[i - 1] + f[i - 1] / double(n);
  const double integral = cdf[n];
  for (uint32_t i = 1; i <= n; ++i) cdf[i] = integral == 0.0 ? double(i) / double(n) : cdf[i] / integral;
  return integral;
}

// pbrt's PiecewiseConstant2D constructor: one conditional per row iv over f[iv n, iv n + n), the
// marginal over the rows' integrals. Built in double, narrowed once.
EnvDistribution piecewise2d(const std::vector<double>& f, uint32_t n) {
  EnvDistribution d;
  d.n = n;
  const size_t nn = size_t(n) * n;
  std::vector<double> cdf(size_t(n) + 1), row_int(n);
  d.func.resize(nn);
  d.cdf_u.resize(size_t(n) * (n + 1));
  d.marginal_func.resize(n);
  d.marginal_cdf.resize(size_t(n) + 1);
  for (uint32_t v = 0; v < n; ++v) {
    row_int[v] = piecewise1d(f.data() + size_t(v) * n, n, cdf.data());
    for (uint32_t i = 0; i <= n; ++i) d.cdf_u[size_t(v) * (n + 1) + i] = float(cdf[i]);
  }
  const double integral = piecewise1d(row_int.data(), n, cdf.data());
  for (size_t i = 0; i < nn; ++i) d.func[i] = float(f[i]);
  for (uint32_t v = 0; v < n; ++v) d.marginal_func[v] = float(row_int[v]);
  for (uint32_t i = 0; i <= n; ++i) d.marginal_cdf[i] = float(cdf[i]);
  d.func_integral = float(integral);
  return d;
}

[[noreturn]] void bad(const std::string& why) { throw std::invalid_argument("build_env_table: " + why); }

}  // namespace

uint64_t EnvTable::bytes() const {
  uint64_t b = 0;
  for (const EnvDistribution* d : {&plain, &compensated})
    b += (d->func.size() + d->cdf_u.size() + d->marginal_func.size() + d->marginal_cdf.size()) * sizeof(float);
  return b;
}

EnvTable build_env_table(const std::vector<double>& luminance, uint32_t n) {
  if (n == 0) bad("n is 0");
  if (luminance.empty()) bad("the luminance grid is empty");
  if (luminance.size() != size_t(n) * n)
    bad("the grid has " + std::to_string(luminance.size()) + " entries, not n * n = " + std::to_string(size_t(n) * n));
  double peak = 0.0;
  for (size_t i = 0; i < luminance.size(); ++i) {
    const double v = luminance[i];
    if (!std::isfinite(v)) bad("entry " + std::to_string(i) + " is not finite");
    if (v < 0.0) bad("entry " + std::to_string(i) + " is negative (" + std::to_string(v) + ")");
    peak = std::max(peak, v);
  }
  if (peak == 0.0) bad("every entry is 0, so the plain distribution has no mass");
  if (peak > double(FLT_MAX)) bad("the largest entry " + std::to_string(peak) + " does not fit a float");

  EnvTable t;
  t.n = n;
  t.plain = piecewise2d(luminance, n);
  if (!(t.plain.func_integral >= FLT_MIN)) bad("the grid's integral is below the smallest normal float");

  // pbrt ImageInfiniteLight::ImageInfiniteLight:
  //   Float average = std::accumulate(d.begin(), d.end(), 0.) / d.size();
  //   for (Float &v : d)
  //       v = std::max<Float>(v - average, 0);
  //   if (std::all_of(d.begin(), d.end(), [](Float v) { return v == 0; }))
  //       std::fill(d.begin(), d.end(), Float(1));
  double sum = 0.0;
  for (double v : luminance) sum += v;
  const double average = sum / double(luminance.size());
  std::vector<double> comp(luminance.size());
  for (size_t i = 0; i < comp.size(); ++i) comp[i] = std::max(luminance[i] - average, 0.0);
  if (std::all_of(comp.begin(), comp.end(), [](double v) { return v == 0.0; })) std::fill(comp.begin(), comp.end(), 1.0);
  t.compensated = piecewise2d(comp, n);
  if (!(t.compensated.func_integral >= FLT_MIN)) bad("the compensated integral is below the smallest normal float");
  return t;
}

}  // namespace astroray::host
