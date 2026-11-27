// SPDX-License-Identifier: Apache-2.0
// OpenPBRParams: the record the VM fills per shading point (thread-local, passed by reference,
// never by value) and the lobe stack reads. Field order is the order of the table in the record.
// Ref: docs/decisions/0005-material-model.md item 2(c) (table), item 3 (record <= 304 B).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../portability.h"

AR_NS_BEGIN

// OpenPBRParams::flags bits.
inline constexpr uint32_t PARAMS_THIN_WALLED = 1u << 0;
inline constexpr uint32_t PARAMS_IOR_DISPERSIVE = 1u << 1;

struct OpenPBRParams {
  // 24 scalars. thin_film_thickness in micrometres; other lengths in scene units (metres).
  Real base_weight, base_diffuse_roughness, base_metalness;
  Real specular_weight, specular_roughness, specular_roughness_anisotropy;
  Real transmission_weight, transmission_depth, transmission_scatter_anisotropy;
  Real subsurface_weight, subsurface_radius, subsurface_scatter_anisotropy;
  Real fuzz_weight, fuzz_roughness;
  Real coat_weight, coat_roughness, coat_roughness_anisotropy, coat_ior, coat_darkening;
  Real thin_film_weight, thin_film_thickness, thin_film_ior;
  Real geometry_opacity, closure_weight;
  // 9 spectral slots at the path's wavelengths.
  Spec base_color, specular_color, specular_ior, transmission_color, transmission_scatter, subsurface_color,
      subsurface_radius_scale, fuzz_color, coat_color;
  // World-space frame after bump and normal maps.
  Real3 geometry_normal, geometry_coat_normal, geometry_tangent, geometry_coat_tangent;
  uint32_t metal_ior;  // constant-block offset of the (n, k) SpectrumRef pair; 0 = F82 mode (item 12)
  uint32_t flags;      // PARAMS_* bits
  // The denoiser guide's RGB of base_color [0] and specular_color [1] when the program stored the slot
  // per point (guide_rgb_pack); 0 when it did not, and the constant block's guide RGB stands. They
  // fill the 8 B that padded 296 B of fields to 304 (PROVISIONAL, item 2).
  uint32_t guide_rgb[2];
};

static_assert(sizeof(Real) != 4 || sizeof(OpenPBRParams) <= 304, "OpenPBRParams must stay within 304 B (0005 item 3)");

// The guide RGB of a per-point colour slot (0008 d15): the RGB the reflectance lift consumed,
// clamped to [0, 1] per channel as albedo_coeffs clamps it (a NaN to 0, as rgb_clamp_negative), so
// the guide is the colour the spectrum was fitted from (pbrt-v4 RGBAlbedoSpectrum). Two RGBs fit the
// 8 B only packed: three 10-bit unorm channels, r in bits 0-9, g 10-19, b 20-29, rounded to nearest,
// so a channel is at most 0.5 / 1023 = 4.9e-4 from the clamped value, an eighth of an 8-bit guide
// step (three halves would need 6 B each). Bit 30 marks a stored value, so 0 means "not stored".
// Ref: docs/decisions/0008-film-outputs-denoise.md d15; color/rgb2spec.h albedo_coeffs; pbrt-v4
// src/pbrt/util/spectrum.h RGBAlbedoSpectrum (Apache-2.0).
inline constexpr uint32_t kGuideRgbStored = 1u << 30;
inline constexpr uint32_t kGuideRgbMax = 1023u;

AR_FN uint32_t guide_rgb_pack(const float* rgb) {
  uint32_t w = kGuideRgbStored;
  for (int c = 0; c < 3; ++c) {
    float x = rgb[c] >= 0.0f ? rgb[c] : 0.0f;
    x = x < 1.0f ? x : 1.0f;
    w |= uint32_t(x * float(kGuideRgbMax) + 0.5f) << (10 * c);
  }
  return w;
}

// Times the rounded reciprocal rather than a division (IEEE division is a subroutine on the GPU):
// at most one more rounding, far below the packing's step.
AR_FN Real guide_rgb_channel(uint32_t w, int c) {
  return Real((w >> (10 * c)) & kGuideRgbMax) * Real(1.0 / double(kGuideRgbMax));
}

AR_NS_END
