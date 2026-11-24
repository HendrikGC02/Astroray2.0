// SPDX-License-Identifier: Apache-2.0
#include "common/noisy_fixture.h"

#include <cmath>
#include <random>

namespace astroray::test {

NoisyFixture make_noisy_fixture() {
  constexpr uint32_t n = NoisyFixture::kSize;
  NoisyFixture f;
  f.clean.resize(size_t(n) * n * 3);
  f.noisy.resize(f.clean.size());
  f.noisy_clamped.resize(f.clean.size());
  f.albedo.resize(f.clean.size());
  f.normal.resize(f.clean.size());
  // Light from the upper left, towards the viewer.
  const double lx = -0.4, ly = 0.5, lz = 0.768;
  for (uint32_t y = 0; y < n; ++y) {
    for (uint32_t x = 0; x < n; ++x) {
      const size_t i = (size_t(y) * n + x) * 3;
      const double u = (x + 0.5) / n, v = (y + 0.5) / n;
      // Albedo: a 4 x 4 board of two colours (edges every 32 px) and a disc of a third.
      const bool odd = ((x / 32) + (y / 32)) % 2 != 0;
      double a[3] = {odd ? 0.2 : 0.8, odd ? 0.5 : 0.3, odd ? 0.8 : 0.2};
      // Normal: flat, except a hemisphere bump in the disc.
      double nx = 0, ny = 0, nz = 1;
      const double dx = u - 0.62, dy = v - 0.4, r = 0.22;
      const double d2 = dx * dx + dy * dy;
      if (d2 < r * r) {
        a[0] = 0.9;
        a[1] = 0.85;
        a[2] = 0.3;
        nx = dx / r;
        ny = dy / r;
        nz = std::sqrt(1.0 - (d2 / (r * r)));
      }
      // Shading: Lambert under the light times a smooth horizontal and vertical ramp, up to about 2.
      const double lambert = std::fmax(0.0, nx * lx + ny * ly + nz * lz);
      const double ramp = (0.25 + 1.5 * u) * (0.6 + 0.6 * v);
      for (int k = 0; k < 3; ++k) {
        f.albedo[i + k] = float(a[k]);
        f.clean[i + k] = float(a[k] * lambert * ramp);
      }
      f.normal[i + 0] = float(nx);
      f.normal[i + 1] = float(ny);
      f.normal[i + 2] = float(nz);
    }
  }
  // Noise of a few tens of samples per pixel: standard deviation 0.35 of the value plus a floor
  // of 0.02, so dark pixels are noisy too. mt19937_64's sequence is fixed by the standard.
  std::mt19937_64 rng(0x176u);
  const double two_pi = 6.283185307179586;
  auto uniform = [&rng]() { return (double(rng() >> 11) + 0.5) * (1.0 / 9007199254740992.0); };
  for (size_t i = 0; i < f.clean.size(); ++i) {
    // Two statements: the order of two calls in one expression is unspecified.
    const double u1 = uniform();
    const double u2 = uniform();
    const double g = std::sqrt(-2.0 * std::log(u1)) * std::cos(two_pi * u2);
    f.noisy[i] = float(f.clean[i] + (0.35 * f.clean[i] + 0.02) * g);
    f.noisy_clamped[i] = std::fmax(0.0f, f.noisy[i]);
  }
  return f;
}

double rel_mse(const float* x, uint32_t stride, const std::vector<float>& ref) {
  const size_t pixels = ref.size() / 3;
  double sum = 0;
  for (size_t p = 0; p < pixels; ++p)
    for (int k = 0; k < 3; ++k) {
      const double r = ref[3 * p + k], d = double(x[p * stride + k]) - r;
      sum += d * d / (r * r + kRelMseEps);
    }
  return sum / double(ref.size());
}

}  // namespace astroray::test
