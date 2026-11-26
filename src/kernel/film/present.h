// SPDX-License-Identifier: Apache-2.0
// film_present and film_heatmap: the two ways a film becomes a frame to show.
//   present: display mean S1 / n in working-space RGB, alpha 1, clamped to the finite half
//            range, IEEE half (round to nearest even), RGBA16F of the render sub-rectangle.
//   heatmap: sample count or bounce mean through viridis into sRGB RGBA8 (the one 8-bit
//            exception: a scene-linear heat map would be bent by the view transform).
// Both write row-major, rows bottom-up like the film (pixel j of the sub-rectangle is element j).
// The output pointer and the heat-map range come from KernelGlobals::display (DisplayTarget).
// Ref: docs/decisions/0007-blender-integration.md d29 (RGBA16F, clamp to +-65504), d30, d40;
// 0008 d1 (rows bottom-up), d12 (statistics, default ranges, "no data" colour), d3 (display
// mean S1 / n); RULINGS 57 (RGBA16F images, heat maps the single 8-bit exception);
// IEEE 754-2008 binary16 (5 exponent bits, bias 15, 10 fraction bits).
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "film_write.h"
#include "viridis.h"

AR_NS_BEGIN

inline constexpr float kHalfMax = 65504.0f;  // largest finite binary16

// Clamp to the finite half range. NaN passes through (a NaN here is a bug upstream: invalid
// samples never reach the film, 0008 d6), so this never hides one.
AR_FN float film_clamp_half(float v) { return v > kHalfMax ? kHalfMax : (v < -kHalfMax ? -kHalfMax : v); }

// Float to binary16 bits, round to nearest even. Finite input only, already clamped to
// +-kHalfMax so no finite value overflows to infinity. NaN and Inf map to half NaN and Inf.
AR_FN uint16_t float_to_half(float value) {
  const uint32_t bits = bit_cast<uint32_t>(value);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t mag = bits & 0x7fffffffu;
  const uint32_t exp = mag >> 23;  // biased float exponent
  uint32_t half;
  if (exp == 255u) {               // NaN or Inf: stays one (the clamp keeps Inf out; NaN is a bug upstream)
    half = 0x7c00u | ((mag & 0x7fffffu) != 0u ? 0x200u : 0u);
  } else if (exp >= 113u) {               // |x| >= 2^-14: a normal half
    const uint32_t mant = mag & 0x7fffffu;
    half = ((exp - 112u) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1fffu;
    // A carry out of the fraction bumps the exponent, which is the correct next value.
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) ++half;
  } else if (exp >= 102u) {        // 2^-25 <= |x| < 2^-14: a subnormal half (unit 2^-24)
    const uint32_t mant = (mag & 0x7fffffu) | 0x800000u;
    const uint32_t shift = 126u - exp;  // 14 .. 24
    half = mant >> shift;
    const uint32_t rem = mant & ((1u << shift) - 1u);
    const uint32_t mid = 1u << (shift - 1u);
    if (rem > mid || (rem == mid && (half & 1u) != 0u)) ++half;
  } else {
    half = 0u;                     // below half of the smallest subnormal (2^-25 ties to 0)
  }
  return uint16_t(sign | half);
}

enum HeatmapMode : uint32_t { HEATMAP_SAMPLE_COUNT = 0, HEATMAP_BOUNCE_COUNT = 1 };

// What the two kernels write and how (KernelGlobals::display). present: out is uint16_t RGBA16F,
// rect_width * rect_height * 4 halves. heatmap: out is uint32_t RGBA8, one packed word per pixel
// (R | G << 8 | B << 16 | A << 24); mode is a HeatmapMode and range_max > 0 is the max spp for the
// sample count or the max depth for the bounce mean.
struct DisplayTarget {
  void* out;
  uint32_t mode;
  float range_max;
};

// Fixed "no data" colour (0008 d12): mid grey, outside viridis.
inline constexpr uint32_t kHeatmapNoData = 0xff808080u;

AR_FN uint32_t film_present_count(const KernelGlobals& kg) { return kg.film->rect_width * kg.film->rect_height; }

// The display mean S1 / n of `pixel` in working-space RGB (0008 d3): 0 with no samples, not the
// NaN of 0 / 0. The display group is always channel 0. Shared by present and film_prepare_denoise,
// so both read the one mean.
AR_FN void film_display_mean(const FilmData& f, uint32_t pixel, float rgb[3]) {
  const uint32_t n = f.n[pixel];
  for (uint32_t c = 0; c < 3u; ++c) {
    const uint64_t at = uint64_t(f.moment[0].offset + c) * f.plane_stride + pixel;
    rgb[c] = n == 0u ? 0.0f : float(f.s1[at] / double(n));
  }
}

AR_FN void film_present_pixel(const KernelGlobals& kg, uint32_t j) {
  const uint32_t pixel = load_ro(kg.active_pixels + j);
  float rgb[3];
  film_display_mean(*kg.film, pixel, rgb);
  uint16_t* o = static_cast<uint16_t*>(kg.display->out) + 4ull * j;
  o[0] = float_to_half(film_clamp_half(rgb[0]));
  o[1] = float_to_half(film_clamp_half(rgb[1]));
  o[2] = float_to_half(film_clamp_half(rgb[2]));
  o[3] = 0x3c00u;  // 1.0
}

// Viridis entry for t in [0, 1] (nearest of the 256 entries) as 8-bit sRGB, alpha 255.
AR_FN uint32_t viridis_rgba8(float t) {
  const float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const int i = int(c * float(kViridisSize - 1) + 0.5f);
  const uint32_t r = uint32_t(viridis_component(i, 0) * 255.0f + 0.5f);
  const uint32_t g = uint32_t(viridis_component(i, 1) * 255.0f + 0.5f);
  const uint32_t b = uint32_t(viridis_component(i, 2) * 255.0f + 0.5f);
  return r | (g << 8) | (b << 16) | 0xff000000u;
}

// False when the film lacks what the mode reads; the host checks before launching, since a
// kernel cannot report.
AR_FN bool film_heatmap_supported(const FilmData& f, uint32_t mode) {
  if (mode == HEATMAP_SAMPLE_COUNT) return f.n != nullptr;
  if (mode == HEATMAP_BOUNCE_COUNT) return f.bounce_sum >= 0 && f.bounce_paths >= 0;
  return false;
}

AR_FN void film_heatmap_pixel(const KernelGlobals& kg, uint32_t j) {
  const FilmData& f = *kg.film;
  const DisplayTarget& p = *kg.display;
  AR_ASSERT(p.range_max > 0.0f && film_heatmap_supported(f, p.mode));
  float value;
  bool has_data;
  if (p.mode == HEATMAP_SAMPLE_COUNT) {
    const uint32_t n = f.n[load_ro(kg.active_pixels + j)];
    has_data = n != 0u;
    value = float(n);
  } else {
    Real mean = Real(0);
    has_data = film_bounce_mean_value(f, load_ro(kg.active_pixels + j), &mean);
    value = float(mean);
  }
  static_cast<uint32_t*>(p.out)[j] = has_data ? viridis_rgba8(value / p.range_max) : kHeatmapNoData;
}

AR_NS_END

// CPU chunk functions: pixels [first, last) of the render sub-rectangle.
void cpu_film_present(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_film_heatmap(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
