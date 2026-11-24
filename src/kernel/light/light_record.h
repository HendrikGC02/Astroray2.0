// SPDX-License-Identifier: Apache-2.0
// The light record: what the host stores per lamp and what every light type reads. The type
// is a closed enum switched on in light.h, never a virtual call (0004 D7, 0002 D1).
// Ref: docs/decisions/0004-integrator.md D6 (types and energy conventions), D7 (interface);
// 0003 d10 (radiance scale folded into the strength on the host), d11 (Blender watt = factor 1),
// d16 (illuminant convention); 0007-appendix-abi.md ar_light_desc (xf, size, power, colour).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../material/slot_resolve.h"
#include "../material/spectrum_ref.h"
#include "../portability.h"

AR_NS_BEGIN

// Storage is float on both builds, like SpectrumRef; kernels widen to Real on use.
struct LightRecord {
  uint8_t type;         // LightType (light.h)
  uint8_t light_group;  // 0008 d10
  uint16_t flags;       // kLight* bits below
  uint32_t visibility;  // ray-class bits of 0007's instance visibility, same order
  float xf[12];         // row-major 3x4, translation in column 3 (ar_instances_desc xf)
  float size[2];        // point and sphere lamps: size[0] = radius in world units, size[1] unused
  float strength;       // Blender power P times the radiance scale s (0003 d10), in Y-normalised watts
  SpectrumRef emission; // colour, semantic ILLUMINANT (0003 d16)
};
static_assert(sizeof(LightRecord) == 92, "light record layout (8 + 48 + 8 + 4 + 24 B)");

// Flag bits of the emissive-triangle light.
inline constexpr uint16_t kLightTwoSided = 1u << 0;  // emits from both faces: theta_e = pi (0004 D9)
// The emission is per point: Le comes from the material's `emission` entry at the sampled point,
// run by the NEE stage (074); `emission` and `strength` are then unused and sample_li's Le is 0.
inline constexpr uint16_t kLightEmissionProgram = 1u << 1;
// One-sided toward -cross(p1 - p0, p2 - p0) (Blender's BACK emission sampling).
inline constexpr uint16_t kLightBackFace = 1u << 2;

// TRIANGLE: xf[0..8] are the world-space vertices p0, p1, p2 (xyz each), xf[9..11] are 0;
// size[0] and size[1] carry the instance index and the triangle index within its mesh as uint32
// bits (below 0x7f800000, so never a NaN pattern), which the emission entry needs for its
// shading point. WORLD_CONST: xf and size are unused. For both, Le = strength * resolve(emission)
// unless kLightEmissionProgram is set.
AR_FN Real3 tri_vertex(const LightRecord& r, int k) {
  return Real3{Real(r.xf[3 * k]), Real(r.xf[3 * k + 1]), Real(r.xf[3 * k + 2])};
}
AR_FN uint32_t tri_instance(const LightRecord& r) { return bit_cast<uint32_t>(r.size[0]); }
AR_FN uint32_t tri_index(const LightRecord& r) { return bit_cast<uint32_t>(r.size[1]); }

// Point and sphere lamps are isotropic: only the translation of xf matters, and the radius is
// already in world units (the host bakes scale into it).
AR_FN Real3 light_position(const LightRecord& r) { return Real3{Real(r.xf[3]), Real(r.xf[7]), Real(r.xf[11])}; }

// s(lambda) times power: the illuminant upsample of the colour (0003 d16) times P (d11, factor 1)
// times the radiance scale s, which the film resolve divides out again (d10). The geometric
// factor of D6's table (1/(4 pi), 1/(4 pi^2 r^2), ...) is the light type's.
AR_FN Spec light_emission(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  return resolve_slot<SpectrumSemantic::ILLUMINANT>(r.emission, lam, obs) * Real(r.strength);
}

AR_NS_END
