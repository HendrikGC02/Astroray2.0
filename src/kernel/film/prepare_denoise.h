// SPDX-License-Identifier: Apache-2.0
// film_prepare_denoise: the three images a denoiser reads, from the film, in one pass.
//   colour: the display mean S1 / n (film_display_mean, the one present reads), working-space
//           RGB, negative components clamped to 0 in this copy only (the film and the Noisy Image
//           keep them).
//   albedo: the guide_albedo ADD planes' sum over n (their mean), clamped to [0, 1] (OIDN's
//           albedo range: diffuse + specular).
//   normal: the guide_normal ADD planes' sum over n, averaged and NOT renormalised.
// Pixel j of the render sub-rectangle is element j of each plane (float x 3 per pixel, rows
// bottom-up like the film and ImageView). A pixel with n = 0 writes colour 0, albedo 0 and normal 0:
// no sample exists to average, so there is nothing to read (an escaped path's albedo is the sky's
// contribution, a per-sample value, so it is not a constant to write here). A non-finite value never leaves the kernel: it becomes 0 (the film's `invalid` counter
// already counts the sample that caused it, so this adds none). A film without guide planes
// (guide_albedo < 0, which implies guide_normal < 0) writes albedo 0 and normal 0 for every
// pixel; the host then passes no guide views to the denoiser. The film is never written.
// Ref: docs/decisions/0008-film-outputs-denoise.md d15 (colour, albedo, normal), d16, d17, d18;
// 0002 D8 (film_prepare_denoise).
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "film_write.h"
#include "present.h"

AR_NS_BEGIN

// Where the kernel writes (KernelGlobals::denoise): three planes of width * height * 3 floats
// owned by the host.
struct DenoiseTarget {
  float* colour;
  float* albedo;
  float* normal;
  uint32_t width, height;
};

AR_FN bool film_float_finite(float v) { return (bit_cast<uint32_t>(v) & 0x7f800000u) != 0x7f800000u; }

// Non-finite to 0, negative to 0.
AR_FN float film_denoise_colour(float v) { return (film_float_finite(v) && v > 0.0f) ? v : 0.0f; }
AR_FN float film_denoise_albedo(float v) { return !film_float_finite(v) || v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
AR_FN float film_denoise_normal(float v) { return film_float_finite(v) ? v : 0.0f; }

AR_FN uint32_t film_prepare_denoise_count(const KernelGlobals& kg) { return kg.film->rect_width * kg.film->rect_height; }

AR_FN void film_prepare_denoise_pixel(const KernelGlobals& kg, uint32_t j) {
  const FilmData& f = *kg.film;
  const DenoiseTarget& t = *kg.denoise;
  AR_ASSERT(f.n != nullptr && j < t.width * t.height);
  const uint32_t pixel = load_ro(kg.active_pixels + j);
  const uint32_t n = f.n[pixel];
  float rgb[3];
  film_display_mean(f, pixel, rgb);  // 0 when n = 0
  for (uint32_t c = 0; c < 3u; ++c) {
    float albedo = 0.0f, normal = 0.0f;
    if (n != 0u && f.guide_albedo >= 0) {
      AR_ASSERT(f.guide_normal >= 0);  // requested together, as film_write_guides asserts
      const uint64_t a = uint64_t(uint32_t(f.guide_albedo) + c) * f.plane_stride + pixel;
      const uint64_t m = uint64_t(uint32_t(f.guide_normal) + c) * f.plane_stride + pixel;
      albedo = film_denoise_albedo(f.add[a] / float(n));
      normal = film_denoise_normal(f.add[m] / float(n));
    }
    t.colour[3ull * j + c] = film_denoise_colour(rgb[c]);
    t.albedo[3ull * j + c] = albedo;
    t.normal[3ull * j + c] = normal;
  }
}

AR_NS_END

// CPU chunk function: pixels [first, last) of the render sub-rectangle.
void cpu_film_prepare_denoise(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
