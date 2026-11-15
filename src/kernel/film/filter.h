// SPDX-License-Identifier: Apache-2.0
// Pixel filter in the kernel: importance-sampled sub-pixel offsets and the
// splat footprint. Camera samples draw their offset from the filter
// and weigh 1 (0008 d5); light-path splats spread over the footprint (0008 d4).
// Ref: Ernst, Stamminger and Greiner 2006 (filter importance sampling); the
// table lookup is Cycles kernel/util/lookup_table.h lookup_table_read
// (Apache-2.0, Blender Foundation 2011-2022), converted to Real and changed to
// the table layout below; the splat weights are docs/decisions/0008 d4: the
// unit-integral filter at each touched pixel centre, as pbrt-v4
// src/pbrt/film.cpp RGBFilm::AddSplat (Apache-2.0).
// Includes are relative so the host library can include this file without
// src/kernel on its include path.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

// Ref: Cycles kernel/types.h, FILTER_TABLE_SIZE.
constexpr int kFilterTableSize = 1024;
// The density is read with linear interpolation, so it needs more entries than
// the inverse CDF to reach 1e-6 on a Gaussian of width 1 px (peak 1.6, h^2 f'' / 8).
constexpr int kFilterDensitySize = 8192;

// Built once per settings change by host/film/filter_table.cpp.
//   inv_cdf[j]: filter-importance-sampling offset, in pixels from the pixel's
//     corner, for u = j / (size - 1); symmetric about the pixel centre 0.5.
//   density[j]: the filter at |d| = j / (kFilterDensitySize - 1) * radius, scaled so that
//     the 1-D integral over [-radius, radius] is 1.
struct FilterTable {
  float inv_cdf[kFilterTableSize];
  float density[kFilterDensitySize];
  float radius;  // support half-width in pixels
};

struct FilterOffset {
  Real dx, dy;
};

// Linear interpolation at u in [0, 1] (clamped), as lookup_table_read.
AR_FN Real filter_table_read(const float* table, Real u) {
  const Real uc = min(max(u, Real(0)), Real(1));
  const Real x = uc * Real(kFilterTableSize - 1);
  int index = static_cast<int>(x);
  if (index > kFilterTableSize - 1) index = kFilterTableSize - 1;
  const int nindex = index + 1 < kFilterTableSize ? index + 1 : kFilterTableSize - 1;
  const Real t = x - Real(index);
  const Real d0 = Real(load_ro(table + index));
  if (t == Real(0)) return d0;
  const Real d1 = Real(load_ro(table + nindex));
  return (Real(1) - t) * d0 + t * d1;
}

// Offset from the pixel centre in pixels. The two dimensions are the first
// two sampler dimensions of the sample (0003 d23); the filter is separable.
AR_FN FilterOffset sample_filter(const FilterTable& t, Real u0, Real u1) {
  return FilterOffset{filter_table_read(t.inv_cdf, u0) - Real(0.5),
                      filter_table_read(t.inv_cdf, u1) - Real(0.5)};
}

// 1-D filter density at offset d of the splat from a pixel centre. The support
// is half-open, [-radius, radius), the same convention as sampling, so Box at
// 1 px gives each position exactly one pixel.
AR_FN Real filter_density(const FilterTable& t, Real d) {
  const Real r = Real(t.radius);
  if (d < -r || d >= r) return Real(0);
  const Real x = abs(d) / r * Real(kFilterDensitySize - 1);
  int index = static_cast<int>(x);
  if (index > kFilterDensitySize - 1) index = kFilterDensitySize - 1;
  const int nindex = index + 1 < kFilterDensitySize ? index + 1 : kFilterDensitySize - 1;
  const Real f = x - Real(index);
  // d0 + f (d1 - d0) returns d0 exactly when the entries are equal (Box).
  const Real d0 = Real(load_ro(t.density + index));
  return d0 + f * (Real(load_ro(t.density + nindex)) - d0);
}

// Unit-integral 2-D weight of a splat at (dx, dy) from a pixel centre.
AR_FN Real splat_weight(const FilterTable& t, Real dx, Real dy) {
  return filter_density(t, dx) * filter_density(t, dy);
}

// The rectangle of pixels whose centres lie within the filter support of a
// splat position. Each pixel's weight is the unit-integral filter at the offset
// of its centre (splat_weight), with no renormalisation over the touched pixels,
// so the light image uses the same kernel as the camera film (0008 d4). A filter
// whose support can hold no centre (radius < 0.5 px) yields nx = 0; the splat
// producer must refuse such filters when it validates settings.
struct SplatFootprint {
  int x0, y0;  // first pixel
  int nx, ny;  // pixel counts, 0 when no centre lies within the support
  Real px, py;
};

// px, py: splat position in pixel coordinates (pixel i spans [i, i + 1)).
AR_FN SplatFootprint splat_footprint(const FilterTable& t, Real px, Real py) {
  const Real r = Real(t.radius);
  SplatFootprint f;
  f.px = px;
  f.py = py;
  // Centres c = i + 0.5 with -r <= px - c < r, that is px - r - 0.5 < i <= px + r - 0.5.
  f.x0 = static_cast<int>(ar::floor(px - r - Real(0.5))) + 1;
  f.y0 = static_cast<int>(ar::floor(py - r - Real(0.5))) + 1;
  f.nx = static_cast<int>(ar::floor(px + r - Real(0.5))) - f.x0 + 1;
  f.ny = static_cast<int>(ar::floor(py + r - Real(0.5))) - f.y0 + 1;
  if (f.nx < 1 || f.ny < 1) {
    f.nx = 0;
    f.ny = 0;
  }
  return f;
}

// Weight of pixel (x0 + i, y0 + j) of the footprint.
AR_FN Real splat_tap_weight(const FilterTable& t, const SplatFootprint& f, int i, int j) {
  return splat_weight(t, f.px - (Real(f.x0 + i) + Real(0.5)), f.py - (Real(f.y0 + j) + Real(0.5)));
}

AR_NS_END
