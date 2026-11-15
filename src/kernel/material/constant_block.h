// SPDX-License-Identifier: Apache-2.0
// The per-material constant block: all 41 OpenPBR Surface inputs at their authored values, in
// the specification's order, plus the interior medium fields. Scalars are f32, colour inputs and
// specular_ior are SpectrumRefs whose semantic is fixed by the slot.
// Ref: OpenPBR Surface 1.1.2, https://academysoftwarefoundation.github.io/OpenPBR/ (inputs and
// defaults); docs/decisions/0005-material-model.md item 2(b) (block), item 2a (interior fields),
// item 10 (semantic per slot). Lengths are scene units (metres), item 2 "Not in the record":
// OpenPBR's 1 cm subsurface_radius default is 0.01 here; thin_film_thickness stays in um.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"
#include "slot_resolve.h"
#include "spectrum_ref.h"

AR_NS_BEGIN

struct ConstantBlock {
  float base_weight;
  SpectrumRef base_color;
  float base_diffuse_roughness;
  float base_metalness;
  float specular_weight;
  SpectrumRef specular_color;
  float specular_roughness;
  float specular_roughness_anisotropy;
  SpectrumRef specular_ior;
  float transmission_weight;
  SpectrumRef transmission_color;
  float transmission_depth;
  SpectrumRef transmission_scatter;
  float transmission_scatter_anisotropy;
  float transmission_dispersion_scale;
  float transmission_dispersion_abbe_number;
  float subsurface_weight;
  SpectrumRef subsurface_color;
  float subsurface_radius;
  SpectrumRef subsurface_radius_scale;
  float subsurface_scatter_anisotropy;
  float fuzz_weight;
  SpectrumRef fuzz_color;
  float fuzz_roughness;
  float coat_weight;
  SpectrumRef coat_color;
  float coat_roughness;
  float coat_roughness_anisotropy;
  float coat_ior;
  float coat_darkening;
  float thin_film_weight;
  float thin_film_thickness;
  float thin_film_ior;
  float emission_luminance;
  SpectrumRef emission_color;
  float geometry_opacity;
  uint32_t geometry_thin_walled;  // 0 or 1
  // Zero means "the shading point's geometric frame" (the MaterialX default is the geometry
  // property); the VM substitutes the frame when it fills OpenPBRParams.
  float geometry_normal[3];
  float geometry_coat_normal[3];
  float geometry_tangent[3];
  float geometry_coat_tangent[3];
  // Item 2a: constant source of specular_ior for the medium stack, and its priority.
  SpectrumRef interior_ior;
  uint8_t medium_priority;
  uint8_t pad_[3];
};

static_assert(sizeof(ConstantBlock) == 424, "ConstantBlock: 26 f32, 1 u32, 12 f32, 11 SpectrumRef, u8 + pad");

// The colour and IOR inputs, in block order, plus interior_ior.
enum class Slot : uint8_t {
  BASE_COLOR = 0,
  SPECULAR_COLOR,
  SPECULAR_IOR,
  TRANSMISSION_COLOR,
  TRANSMISSION_SCATTER,
  SUBSURFACE_COLOR,
  SUBSURFACE_RADIUS_SCALE,
  FUZZ_COLOR,
  COAT_COLOR,
  EMISSION_COLOR,
  INTERIOR_IOR,
  COUNT_
};

// Item 10's table, one row per slot; the compiler lifts each source with it.
inline constexpr SpectrumSemantic kSlotSemantic[int(Slot::COUNT_)] = {
    SpectrumSemantic::REFLECTANCE,  // BASE_COLOR
    SpectrumSemantic::REFLECTANCE,  // SPECULAR_COLOR
    SpectrumSemantic::IOR,          // SPECULAR_IOR
    SpectrumSemantic::REFLECTANCE,  // TRANSMISSION_COLOR
    SpectrumSemantic::REFLECTANCE,  // TRANSMISSION_SCATTER
    SpectrumSemantic::REFLECTANCE,  // SUBSURFACE_COLOR
    SpectrumSemantic::UNBOUNDED,    // SUBSURFACE_RADIUS_SCALE
    SpectrumSemantic::REFLECTANCE,  // FUZZ_COLOR
    SpectrumSemantic::REFLECTANCE,  // COAT_COLOR
    SpectrumSemantic::ILLUMINANT,   // EMISSION_COLOR
    SpectrumSemantic::IOR,          // INTERIOR_IOR
};

AR_FN SpectrumRef& slot_ref(ConstantBlock& b, Slot s) {
  switch (s) {
    case Slot::BASE_COLOR: return b.base_color;
    case Slot::SPECULAR_COLOR: return b.specular_color;
    case Slot::SPECULAR_IOR: return b.specular_ior;
    case Slot::TRANSMISSION_COLOR: return b.transmission_color;
    case Slot::TRANSMISSION_SCATTER: return b.transmission_scatter;
    case Slot::SUBSURFACE_COLOR: return b.subsurface_color;
    case Slot::SUBSURFACE_RADIUS_SCALE: return b.subsurface_radius_scale;
    case Slot::FUZZ_COLOR: return b.fuzz_color;
    case Slot::COAT_COLOR: return b.coat_color;
    case Slot::EMISSION_COLOR: return b.emission_color;
    default: return b.interior_ior;
  }
}

// Every input at its OpenPBR default. The colour defaults go through the RGB lift (the table is
// the working space's), so subsurface_radius_scale = (1, 0.5, 0.25) is a real spectrum.
AR_FN ConstantBlock default_constant_block(const Rgb2SpecTable& t) {
  ConstantBlock b = {};
  b.base_weight = 1.0f;
  b.base_diffuse_roughness = 0.0f;
  b.base_metalness = 0.0f;
  b.specular_weight = 1.0f;
  b.specular_roughness = 0.3f;
  b.specular_roughness_anisotropy = 0.0f;
  b.transmission_weight = 0.0f;
  b.transmission_depth = 0.0f;
  b.transmission_scatter_anisotropy = 0.0f;
  b.transmission_dispersion_scale = 0.0f;
  b.transmission_dispersion_abbe_number = 20.0f;
  b.subsurface_weight = 0.0f;
  b.subsurface_radius = 0.01f;
  b.subsurface_scatter_anisotropy = 0.0f;
  b.fuzz_weight = 0.0f;
  b.fuzz_roughness = 0.5f;
  b.coat_weight = 0.0f;
  b.coat_roughness = 0.0f;
  b.coat_roughness_anisotropy = 0.0f;
  b.coat_ior = 1.6f;
  b.coat_darkening = 1.0f;
  b.thin_film_weight = 0.0f;
  b.thin_film_thickness = 0.5f;
  b.thin_film_ior = 1.4f;
  b.emission_luminance = 0.0f;
  b.geometry_opacity = 1.0f;
  b.geometry_thin_walled = 0u;
  b.medium_priority = 0;
  const Rgb grey08{{Real(0.8), Real(0.8), Real(0.8)}}, one{{Real(1), Real(1), Real(1)}}, zero{{Real(0), Real(0), Real(0)}};
  const Rgb radius_scale{{Real(1), Real(0.5), Real(0.25)}};
  uint32_t flags = 0;
  for (int i = 0; i < int(Slot::COUNT_); ++i) {
    const Slot s = Slot(i);
    const SpectrumSemantic sem = kSlotSemantic[i];
    if (sem == SpectrumSemantic::IOR) {
      slot_ref(b, s) = make_const_ref(sem, Real(1.5));
      continue;
    }
    Rgb rgb = one;
    if (s == Slot::BASE_COLOR || s == Slot::SUBSURFACE_COLOR) rgb = grey08;
    if (s == Slot::TRANSMISSION_SCATTER) rgb = zero;
    if (s == Slot::SUBSURFACE_RADIUS_SCALE) rgb = radius_scale;
    const bool ok = make_rgb_ref(t, rgb, sem, &slot_ref(b, s), &flags);
    AR_ASSERT(ok);
    (void)ok;
  }
  AR_ASSERT(flags == 0u);
  return b;
}

AR_NS_END
