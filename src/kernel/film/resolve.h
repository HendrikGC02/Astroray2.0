// SPDX-License-Identifier: Apache-2.0
// film_resolve: one thread per active pixel folds the pass's slots and splat entry into the
// fp64 moments with plain read-add-write (one writer per pixel, no atomics), then clears them.
//   per valid sample m of pixel j: x_c = convert(...) / s;  S1 += x;  S2 += x^2;
//   S1A += x when the sample index n + m is even;  n += 1
// An invalid sample (NaN, Inf, any negative lane) is counted and skipped. Until the pass has
// an active-pixel list (task 101), active pixel j is pixel j of the render sub-rectangle.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3 (Resolve), d4 (SPLAT fold), d6 (invalid
// samples), d7 (conversion); 0003 d2 and d23 (wavelengths recomputed from the camera key),
// d10 (radiance scale removed in double).
#pragma once
#include <cstdint>

#include "../core/packet.h"
#include "../sample/dims.h"
#include "../sample/keys.h"
#include "../sample/sobol.h"
#include "film_write.h"

AR_NS_BEGIN

// float::max as a literal, so the kernel needs no <cfloat>.
inline constexpr float kFilmFloatMax = 3.402823466e38f;

AR_FN bool film_sample_valid(const Spec& L) {
  for (int i = 0; i < kLanes; ++i) {
    const float v = float(L.v[i]);
    // Negated comparisons are false for NaN, so NaN, negatives and +Inf all fail.
    if (!(v >= 0.0f && v <= kFilmFloatMax)) return false;
  }
  return true;
}

// x is the sample's value in this plane before the radiance scale: S1 += x / s, S2 += (x / s)^2,
// and S1A += x / s for even sample indices. Plain read-add-write: one thread owns the pixel.
AR_FN void film_moment_add(const FilmData& f, uint32_t plane, uint32_t pixel, double x, bool even) {
  const double v = x * f.inv_radiance_scale;
  const uint64_t at = uint64_t(plane) * f.plane_stride + pixel;
  f.s1[at] += v;
  f.s2[at] += v * v;
  if (f.s1a != nullptr && even) f.s1a[at] += v;
}

AR_FN void film_resolve_pixel(const KernelGlobals& kg, uint32_t j) {
  const FilmData& f = *kg.film;
  // pixel_id(x, y, width) is the plane index, so one value serves as key and address.
  const uint32_t pixel = pixel_id(j % f.rect_width, j / f.rect_width, f.width);
  for (uint32_t m = 0; m < kg.session.samples_per_pass; ++m) {
    const uint32_t k = kg.session.samples_per_pass;
    const uint32_t n0 = kg.session.samples_done;
    const uint32_t slot = j * k + m;
    Spec L;
    for (int i = 0; i < kLanes; ++i) L.v[i] = Real(f.slot_L[4u * slot + uint32_t(i)]);
    const uint32_t flags = f.slot_flags[slot];
    for (int i = 0; i < kLanes; ++i) f.slot_L[4u * slot + uint32_t(i)] = 0.0f;
    f.slot_flags[slot] = 0u;

    if (!film_sample_valid(L)) {
      f.invalid[pixel] += 1u;
      continue;
    }
    const uint32_t index = n0 + m;
    const Real u = draw(kg.session.sampler_mode, pixel, index, 0u, DIM_CAM_WAVELENGTH,
                        kg.session.seed);
    const FilmWavelengths fw = sample_film_wavelengths(f.wavelengths, u);
    const Spec w = film_weights(flags, fw.pf);
    const bool even = (index & 1u) == 0u;
    for (uint32_t c = 0; c < f.moment_channel_count; ++c) {
      const uint32_t offset = f.moment[c].offset;
      const uint32_t response = f.moment[c].response;
      // Components are written out, not looped over an array: a dynamically indexed local
      // array would put the sample's channel values on the stack of every GPU thread.
      if (response == kFilmDisplayResponse) {
        const DisplayRgb rgb = display_rgb(f.response, L, fw.lf, w, f.film_exposure);
        film_moment_add(f, offset, pixel, rgb.v[0], even);
        film_moment_add(f, offset + 1u, pixel, rgb.v[1], even);
        film_moment_add(f, offset + 2u, pixel, rgb.v[2], even);
      } else {
        film_moment_add(f, offset, pixel, convert(f.response, L, fw.lf, w, response), even);
      }
    }
    f.n[pixel] += 1u;
  }

  // The light image is one independent estimate per pass: it adds to S1, S2 and, on even
  // passes, S1A of the display channel, and does not count as a camera sample.
  if (f.splat != nullptr) {
    const bool even = (kg.session.pass_index & 1u) == 0u;
    for (uint32_t q = 0; q < f.splat_components; ++q) {
      const uint64_t from = uint64_t(q) * f.plane_stride + pixel;
      const double v = double(f.splat[from]);
      f.splat[from] = 0.0f;
      const uint64_t at = uint64_t(f.moment[0].offset + q) * f.plane_stride + pixel;
      f.s1[at] += v;
      f.s2[at] += v * v;
      if (f.s1a != nullptr && even) f.s1a[at] += v;
    }
  }
}

AR_FN uint32_t film_resolve_count(const KernelGlobals& kg) { return kg.film->rect_width * kg.film->rect_height; }

AR_NS_END

// CPU chunk function: active pixels [first, last). Defined in kernel_cpu/film_resolve.cpp.
void cpu_film_resolve(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
