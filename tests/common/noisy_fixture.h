// SPDX-License-Identifier: Apache-2.0
// A deterministic denoiser fixture: a clean 128 x 128 HDR image with smooth gradients and sharp
// edges, its clean albedo and normal guides, and a copy with seeded Gaussian noise. The error
// measure is V11's relative MSE.
// Ref: docs/decisions/0008-film-outputs-denoise.md V11 (relMSE = mean((x - ref)^2 / (ref^2 + eps)));
// Box and Muller 1958, "A Note on the Generation of Random Normal Deviates" (the Gaussian noise).
#pragma once
#include <cstdint>
#include <vector>

namespace astroray::test {

struct NoisyFixture {
  static constexpr uint32_t kSize = 128;  // width and height
  // 3 floats per pixel, rows bottom-up like the film. noisy_clamped is noisy with negatives set to 0,
  // what a denoiser is handed (0008 d15): the baseline a denoiser must beat, since the clamp alone
  // already lowers relMSE.
  std::vector<float> clean, noisy, noisy_clamped, albedo, normal;
};

// The eps of relMSE, chosen here: it keeps near-black reference pixels from dominating the mean
// while leaving the error of every pixel brighter than about 0.1 relative.
constexpr double kRelMseEps = 1e-2;

// Upper bound of relMSE(denoised) / relMSE(noisy_clamped) for OIDN on this fixture, both devices
// and modes. Measured (OIDN 2.5.0, Ryzen 7 7800X3D and RTX 5070 Ti): relMSE 0.14854 raw, 0.14332
// clamped; ratio 0.0334 CPU / 0.0335 CUDA in FINAL, 0.0511 / 0.0512 in VIEWPORT. 0.2 leaves a margin
// of about 3.9 over the worst for other CPUs and drivers, while a backend that returns its clamped
// input scores 1.
constexpr double kDenoisedRatioBound = 0.2;

// Always the same image, guides and noise.
NoisyFixture make_noisy_fixture();

// relMSE of x (stride floats per pixel, RGB first) against ref (3 floats per pixel) over every
// pixel and channel.
double rel_mse(const float* x, uint32_t stride, const std::vector<float>& ref);

}  // namespace astroray::test
