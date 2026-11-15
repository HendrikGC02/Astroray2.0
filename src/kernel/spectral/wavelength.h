// SPDX-License-Identifier: Apache-2.0
// Film wavelengths: four lanes stratified in CDF space, recomputed from the sample's
// one sampler dimension and never stored.
// Lane i uses u_i = frac(u + i/4), lambda_i = F^-1(u_i), pf_i = p(lambda_i). Offsetting
// the wavelengths themselves instead is biased under a non-uniform pdf.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d2, d3; pbrt-v4
// src/pbrt/util/spectrum.h SampledWavelengths::SampleVisible (Apache-2.0), after Wilkie
// et al. 2014, "Hero Wavelength Spectral Sampling", CGF 33(4).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"

AR_NS_BEGIN

// Built by the host (host/spectral/wavelength_table.h). edges and cdf hold bins + 1
// floats: edges[0] and edges[bins] bound the support in nm, cdf[0] = 0, cdf[bins] = 1,
// the pdf is constant per bin.
struct WavelengthTableView {
  const float* edges;
  const float* cdf;
  uint32_t bins;
};

struct FilmWavelengths {
  Lambda lf;
  Spec pf;  // pdf per nm at each lf
};

// The bin b with cdf[b] <= u < cdf[b + 1]. cdf[bins] = 1 > u keeps the search inside the
// table; a bin of zero width in the CDF is never returned, as a later bin has cdf <= u.
AR_FN uint32_t wavelength_bin(const WavelengthTableView& t, Real u) {
  uint32_t lo = 0;
  uint32_t hi = t.bins;
  while (hi - lo > 1u) {
    const uint32_t mid = (lo + hi) >> 1;
    if (Real(load_ro(t.cdf + mid)) <= u) lo = mid;
    else hi = mid;
  }
  return lo;
}

// u in [0, 1) is the sample's DIM_CAM_WAVELENGTH or DIM_LIGHT_WAVELENGTH value.
AR_FN FilmWavelengths sample_film_wavelengths(const WavelengthTableView& t, Real u) {
  FilmWavelengths out;
  for (int i = 0; i < kLanes; ++i) {
    // Wrapping by subtraction keeps both branches exact in float for u on the sampler's
    // 2^-24 grid; the sum u + i/4 rounds once it reaches [1, 2).
    const Real q = Real(i) * Real(0.25);
    const Real ui = u >= Real(1) - q ? u - (Real(1) - q) : u + q;
    const uint32_t b = wavelength_bin(t, ui);
    const Real c0 = Real(load_ro(t.cdf + b));
    const Real dc = Real(load_ro(t.cdf + b + 1)) - c0;
    const Real e0 = Real(load_ro(t.edges + b));
    const Real de = Real(load_ro(t.edges + b + 1)) - e0;
    out.lf.nm[i] = e0 + (ui - c0) / dc * de;
    out.pf.v[i] = dc / de;
  }
  return out;
}

AR_NS_END
