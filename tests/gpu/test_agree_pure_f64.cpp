// SPDX-License-Identifier: Apache-2.0
// The CPU double references of the agreement harness: the production kernel headers compiled
// with AR_REAL_DOUBLE, so step (iii) compares the production GPU build with the same source in
// double (0002 D3). Float inputs widen exactly. The float references and the tests are in
// test_agree_pure.cpp. Ref: docs/decisions/0002-kernel-architecture.md D3, D12.
#if !defined(AR_REAL_DOUBLE)
#define AR_REAL_DOUBLE
#endif
#include <cstdint>

#include "agree.h"
#include "color/rgb2spec.h"
#include "spectral/planck.h"

namespace astroray::agree {

Results cpu_planck_f64(const Inputs& in) {
  Results r;
  r.n = in.n;
  r.stride = 4;
  r.v.resize(size_t(in.n) * 4u);
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* p = in.v.data() + size_t(i) * in.stride;
    for (int j = 0; j < 4; ++j) r.v[size_t(i) * 4u + j] = ar::planck_radiance(double(p[j]), double(p[4]));
  }
  return r;
}

Results cpu_sigmoid_f64(const Inputs& in, const TableRef& table) {
  ar::Rgb2SpecTable t = {};
  t.res = table.res;
  t.z_nodes = table.z_nodes;
  t.coeffs = table.coeffs;
  Results r;
  r.n = in.n;
  r.stride = 4;
  r.v.resize(size_t(in.n) * 4u);
  for (uint32_t i = 0; i < in.n; ++i) {
    const float* p = in.v.data() + size_t(i) * in.stride;
    const ar::SigmoidCoeffs c = ar::fetch_coeffs(t, ar::Rgb{{double(p[0]), double(p[1]), double(p[2])}});
    for (int j = 0; j < 4; ++j) r.v[size_t(i) * 4u + j] = ar::sigmoid_eval(c, double(p[3 + j]));
  }
  return r;
}

}  // namespace astroray::agree
