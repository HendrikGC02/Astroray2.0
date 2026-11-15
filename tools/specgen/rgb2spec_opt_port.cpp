// SPDX-License-Identifier: Apache-2.0
// Jakob-Hanika coefficient-table optimiser, adapted for tools/specgen (included by specgen.cpp,
// not a separate translation unit).
//
// Adapted from rgb2spec_opt.cpp and details/lu.h of https://github.com/mitsuba-renderer/rgb2spec
// at commit 721145dedf2491851bd46ab8fd165955cb38ddaf (BSD-3-Clause). Original notice, kept:
//
//   Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>, All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without modification, are
//   permitted provided that the following conditions are met:
//   1. Redistributions of source code must retain the above copyright notice, this list of
//      conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright notice, this list of
//      conditions and the following disclaimer in the documentation and/or other materials
//      provided with the distribution.
//   3. Neither the name of the copyright holder nor the names of its contributors may be used
//      to endorse or promote products derived from this software without specific prior
//      written permission.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
//   OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
//   COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
//   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
//   TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Changed from the original (this file is marked as changed, per the licence):
//  - the built-in CIE data and illuminant are replaced by tables the caller passes in (our
//    CSV observer and our D65 scaled to Y = 1), so the table shares observer_hash;
//  - the quadrature is the engine's own definition, a sum over the 1 nm samples divided by Y_n
//    (0003 d11), instead of Simpson's 3/8 rule on a finer grid, so the optimiser and the
//    engine integrate identically;
//  - working precision is double with strict floating point (the original defaults to float
//    with relaxed FP and SIMD reassociation), so the output is bit-reproducible;
//  - the tables live in a struct instead of globals (no process-global state), and the
//    gamut is given as the XYZ -> RGB matrix of the working space;
//  - output goes to caller-owned vectors; the SIMD padding and the forward-model block that
//    follows the table in the original file are dropped.
// Method: Jakob and Hanika 2019, "A Low-Dimensional Function Space for Efficient Spectral
// Upsampling", CGF 38(2): sigmoid of a quadratic, CIELab residual, optimised outward from the
// middle of the scale axis with the previous solution as the starting point.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace rgb2spec_port {

constexpr double kLambdaMin = 360.0;  // the observer's range, 0003 d14
constexpr double kLambdaMax = 830.0;

// Forward model: spectrum -> working-space RGB by quadrature, plus the Lab white.
struct Model {
  int n = 0;                       // number of wavelength samples
  std::vector<double> lambda01;    // wavelength scaled to [0, 1]
  std::vector<double> rgb_tbl[3];  // working-space RGB weight of each sample (illuminant and 1/Y_n included)
  double rgb_to_xyz[3][3] = {};
  double xyz_whitepoint[3] = {};
};

inline double sqr(double x) { return x * x; }
inline double smoothstep(double x) { return x * x * (3.0 - 2.0 * x); }

// details/lu.h, "LU decomposition & triangular solving code lifted from Wikipedia".
inline int LUPDecompose(double** A, int N, double Tol, int* P) {
  for (int i = 0; i <= N; i++) P[i] = i;
  for (int i = 0; i < N; i++) {
    double maxA = 0.0;
    int imax = i;
    for (int k = i; k < N; k++) {
      double absA = std::fabs(A[k][i]);
      if (absA > maxA) {
        maxA = absA;
        imax = k;
      }
    }
    if (maxA < Tol) return 0;
    if (imax != i) {
      std::swap(P[i], P[imax]);
      std::swap(A[i], A[imax]);
      P[N]++;
    }
    for (int j = i + 1; j < N; j++) {
      A[j][i] /= A[i][i];
      for (int k = i + 1; k < N; k++) A[j][k] -= A[j][i] * A[i][k];
    }
  }
  return 1;
}

inline void LUPSolve(double** A, const int* P, const double* b, int N, double* x) {
  for (int i = 0; i < N; i++) {
    x[i] = b[P[i]];
    for (int k = 0; k < i; k++) x[i] -= A[i][k] * x[k];
  }
  for (int i = N - 1; i >= 0; i--) {
    for (int k = i + 1; k < N; k++) x[i] -= A[i][k] * x[k];
    x[i] = x[i] / A[i][i];
  }
}

inline void cie_lab(const Model& m, double* p) {
  double X = 0, Y = 0, Z = 0;
  const double Xw = m.xyz_whitepoint[0], Yw = m.xyz_whitepoint[1], Zw = m.xyz_whitepoint[2];
  for (int j = 0; j < 3; ++j) {
    X += p[j] * m.rgb_to_xyz[0][j];
    Y += p[j] * m.rgb_to_xyz[1][j];
    Z += p[j] * m.rgb_to_xyz[2][j];
  }
  auto f = [](double t) -> double {
    const double delta = 6.0 / 29.0;
    if (t > delta * delta * delta) return std::cbrt(t);
    return t / (delta * delta * 3.0) + 4.0 / 29.0;
  };
  p[0] = 116.0 * f(Y / Yw) - 16.0;
  p[1] = 500.0 * (f(X / Xw) - f(Y / Yw));
  p[2] = 200.0 * (f(Y / Yw) - f(Z / Zw));
}

inline void eval_residual(const Model& m, const double* coeffs, const double* rgb, double* residual) {
  double out[3] = {0, 0, 0};
  const double c0 = coeffs[0], c1 = coeffs[1], c2 = coeffs[2];
  for (int i = 0; i < m.n; ++i) {
    const double lambda = m.lambda01[size_t(i)];
    const double x = (c0 * lambda + c1) * lambda + c2;
    const double s = 0.5 * x / std::sqrt(1.0 + x * x) + 0.5;
    for (int j = 0; j < 3; ++j) out[j] += m.rgb_tbl[j][size_t(i)] * s;
  }
  cie_lab(m, out);
  for (int j = 0; j < 3; ++j) residual[j] = rgb[j];
  cie_lab(m, residual);
  for (int j = 0; j < 3; ++j) residual[j] -= out[j];
}

// Lab value of `in` and its analytic Jacobian d(lab)/d(in).
inline void cie_lab_jac(const Model& m, const double in[3], double lab[3], double jac[3][3]) {
  double xyz[3] = {0, 0, 0};
  for (int a = 0; a < 3; ++a)
    for (int j = 0; j < 3; ++j) xyz[a] += in[j] * m.rgb_to_xyz[a][j];
  double fv[3], gd[3];  // f(t) and f'(t) / w_n, with t = xyz / whitepoint
  for (int k = 0; k < 3; ++k) {
    const double wn = m.xyz_whitepoint[k];
    const double t = xyz[k] / wn, delta = 6.0 / 29.0;
    if (t > delta * delta * delta) {
      const double cr = std::cbrt(t);
      fv[k] = cr;
      gd[k] = (1.0 / 3.0) / (cr * cr * wn);
    } else {
      fv[k] = t / (delta * delta * 3.0) + 4.0 / 29.0;
      gd[k] = 1.0 / (delta * delta * 3.0 * wn);
    }
  }
  lab[0] = 116.0 * fv[1] - 16.0;
  lab[1] = 500.0 * (fv[0] - fv[1]);
  lab[2] = 200.0 * (fv[1] - fv[2]);
  const double g[3][3] = {{0, 116.0 * gd[1], 0}, {500.0 * gd[0], -500.0 * gd[1], 0}, {0, 200.0 * gd[1], -200.0 * gd[2]}};
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += g[a][k] * m.rgb_to_xyz[k][b];
      jac[a][b] = s;
    }
}

// Residual and its Jacobian in one pass over the wavelengths.
inline void eval_residual_jac(const Model& m, const double* coeffs, const double* rgb, double* residual, double jac[3][3]) {
  double out[3] = {0, 0, 0};
  double dout[3][3] = {};  // d out[j] / d coeffs[a]
  const double c0 = coeffs[0], c1 = coeffs[1], c2 = coeffs[2];
  for (int i = 0; i < m.n; ++i) {
    const double lambda = m.lambda01[size_t(i)];
    const double x = (c0 * lambda + c1) * lambda + c2;
    const double dx0 = lambda * lambda, dx1 = lambda;
    const double q = 1.0 / std::sqrt(1.0 + x * x);
    const double s = 0.5 * x * q + 0.5;
    const double sp = 0.5 * q * q * q;
    for (int j = 0; j < 3; ++j) {
      const double w = m.rgb_tbl[j][size_t(i)];
      out[j] += w * s;
      const double wsp = w * sp;
      dout[j][0] += wsp * dx0;
      dout[j][1] += wsp * dx1;
      dout[j][2] += wsp;
    }
  }
  double out_lab[3], lab_jac[3][3];
  cie_lab_jac(m, out, out_lab, lab_jac);
  for (int j = 0; j < 3; ++j) residual[j] = rgb[j];
  cie_lab(m, residual);
  for (int j = 0; j < 3; ++j) residual[j] -= out_lab[j];
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += lab_jac[a][k] * dout[k][b];
      jac[a][b] = -s;
    }
}

// Levenberg-Marquardt on the squared CIELab distance; `coeffs` is the starting point and the result.
inline double LM(const Model& m, const double rgb[3], double coeffs[3], int it = 15) {
  double residual[3], jac[3][3];
  eval_residual_jac(m, coeffs, rgb, residual, jac);
  double cost = sqr(residual[0]) + sqr(residual[1]) + sqr(residual[2]);
  double lambda = 1e-3;
  for (int i = 0; i < it && cost > 1e-12; ++i) {
    double A[3][3], g[3] = {0, 0, 0};  // normal equations A = J^T J, g = J^T residual
    for (int a = 0; a < 3; ++a) {
      for (int b = 0; b < 3; ++b) {
        double s = 0;
        for (int k = 0; k < 3; ++k) s += jac[k][a] * jac[k][b];
        A[a][b] = s;
      }
      for (int k = 0; k < 3; ++k) g[a] += jac[k][a] * residual[k];
    }
    bool accepted = false;
    for (int t = 0; t < 10 && !accepted; ++t) {
      double M0[3], M1[3], M2[3], *M[3] = {M0, M1, M2};
      for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) M[a][b] = A[a][b] + (a == b ? lambda : 0.0);
      int P[4];
      if (LUPDecompose(M, 3, 1e-30, P) != 1) {
        lambda *= 10.0;
        continue;
      }
      double step[3];
      LUPSolve(M, P, g, 3, step);
      const double trial[3] = {coeffs[0] - step[0], coeffs[1] - step[1], coeffs[2] - step[2]};
      double trial_res[3];
      eval_residual(m, trial, rgb, trial_res);
      const double trial_cost = sqr(trial_res[0]) + sqr(trial_res[1]) + sqr(trial_res[2]);
      if (trial_cost < cost) {
        std::memcpy(coeffs, trial, sizeof(trial));
        cost = trial_cost;
        lambda = std::max(lambda * 0.5, 1e-12);
        accepted = true;
      } else {
        lambda *= 10.0;
        if (lambda > 1e12) break;
      }
    }
    if (!accepted) break;
    eval_residual_jac(m, coeffs, rgb, residual, jac);
  }
  return std::sqrt(cost);
}

// Fills scale[res] and coeffs[3][res][res][res][3] (the layout of pbrt's RGBToSpectrumTable:
// [largest component][scale k][y][x][c0, c1, c2], polynomial in wavelength in nm).
// Each (l, j) slice is independent and writes its own region, so the result does not depend on
// the thread count or the order slices finish in.
inline void optimise_table(const Model& m, int res, std::vector<float>& scale, std::vector<float>& out, unsigned n_threads) {
  scale.resize(size_t(res));
  for (int k = 0; k < res; ++k) scale[size_t(k)] = float(smoothstep(smoothstep(double(k) / double(res - 1))));
  out.assign(size_t(3) * 3 * size_t(res) * res * res, 0.0f);

  auto process = [&](int l, int j) {
    const double y = double(j) / double(res - 1);
    for (int i = 0; i < res; ++i) {
      const double x = double(i) / double(res - 1);
      const int start = res / 5;
      for (int pass = 0; pass < 2; ++pass) {
        double coeffs[3] = {0, 0, 0}, rgb[3];
        // Upwards from `start`, then downwards from `start` again with a fresh starting point.
        const int dk = pass == 0 ? 1 : -1;
        for (int k = start; k >= 0 && k < res; k += dk) {
          const double b = double(scale[size_t(k)]);
          rgb[l] = b;
          rgb[(l + 1) % 3] = x * b;
          rgb[(l + 2) % 3] = y * b;
          LM(m, rgb, coeffs);
          // Remap the polynomial from the [0,1] wavelength domain back to nm.
          const double c0 = kLambdaMin, c1 = 1.0 / (kLambdaMax - kLambdaMin);
          const double A = coeffs[0], B = coeffs[1], C = coeffs[2];
          const size_t idx = ((size_t(l) * size_t(res) + size_t(k)) * size_t(res) + size_t(j)) * size_t(res) + size_t(i);
          out[3 * idx + 0] = float(A * (c1 * c1));
          out[3 * idx + 1] = float(B * c1 - 2 * A * c0 * (c1 * c1));
          out[3 * idx + 2] = float(C - B * c0 * c1 + A * (c0 * c1) * (c0 * c1));
        }
      }
    }
  };

  const int n_tasks = 3 * res;
  std::atomic<int> next_task(0);
  auto worker = [&]() {
    int t;
    while ((t = next_task.fetch_add(1)) < n_tasks) process(t / res, t % res);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < std::max(1u, n_threads); ++t) pool.emplace_back(worker);
  for (std::thread& th : pool) th.join();
}

}  // namespace rgb2spec_port
