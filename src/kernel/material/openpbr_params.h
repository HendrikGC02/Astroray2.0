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
  uint32_t pad_[2];    // 296 B of fields padded to 304 (PROVISIONAL, item 2)
};

static_assert(sizeof(Real) != 4 || sizeof(OpenPBRParams) <= 304, "OpenPBRParams must stay within 304 B (0005 item 3)");

AR_NS_END
