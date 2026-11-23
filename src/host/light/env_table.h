// SPDX-License-Identifier: Apache-2.0
// The host side of the environment importance table (0004 D11): from a non-negative luminance grid
// on the equal-area square, the arrays of the plain and the MIS-compensated piecewise-constant
// distributions that kernel/light/environment_table.h samples (layout in its EnvTableView). Built
// in double, stored as float. Where the grid comes from (bake_environment, the w(lambda)
// weighting), the upload and the ledger are not this file's.
// Ref: third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/util/sampling.h PiecewiseConstant1D
// and PiecewiseConstant2D constructors (domain [0,1]^2); src/pbrt/lights.cpp
// ImageInfiniteLight::ImageInfiniteLight (the compensated distribution) (Apache-2.0); Karlik et
// al. 2019, "MIS compensation", ACM TOG 38(6).
#pragma once
#include <cstdint>
#include <vector>

namespace astroray::host {

// One distribution's arrays, named and laid out as ar::EnvTableView's fields (the view is made by
// whoever binds them: plain floats keep this header free of the kernel's Real-dependent types).
struct EnvDistribution {
  std::vector<float> func, cdf_u, marginal_func, marginal_cdf;
  float func_integral = 0.0f;
  uint32_t n = 0;
};

struct EnvTable {
  uint32_t n = 0;
  EnvDistribution plain;        // light-path emission
  EnvDistribution compensated;  // NEE (allowIncompletePDF)
  uint64_t bytes() const;
};

// `luminance` holds n x n cells, row-major: index iv n + iu for the cell covering square p.x in
// [iu/n, (iu+1)/n), p.y in [iv/n, (iv+1)/n). The compensated function is max(L - mean(L), 0),
// filled with 1 when that is 0 everywhere. For a uniform grid of value L the double mean is L only
// up to rounding: when it comes out at or above L every residual is 0 and the fill gives all ones
// (always so when the sum is exact, e.g. L = 2); when it comes out below L every residual is the
// same few ulps of L. Both give a uniform compensated table, but in the second case a uniform L
// below about 1e-22 leaves residuals under FLT_MIN and the build throws (see below).
// Throws std::invalid_argument, naming the reason, for n = 0, an empty grid, a size other than
// n n, a negative or non-finite entry, an all-zero grid (the plain table has no mass), or a grid
// whose plain table does not fit float (largest entry above FLT_MAX or integral below FLT_MIN).
EnvTable build_env_table(const std::vector<double>& luminance, uint32_t n);

}  // namespace astroray::host
