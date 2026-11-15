// SPDX-License-Identifier: Apache-2.0
// What an interpreter run reads besides its program: the shading context (hit-time only, no
// texture-time pre-pass) and the immutable scene tables the ops look up in.
// Ref: docs/decisions/0005-material-model.md item 33 (context fields), item 10 (lifts need the
// JH table and the working-space illuminant).
#pragma once
#include <cstdint>

#include "../color/observer.h"
#include "../color/rgb2spec.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../portability.h"
#include "../scene/texture_view.h"

AR_NS_BEGIN

// UV sets a program can address; LOAD_CTX_UV with a higher index is a bad operand. Raised when
// the Blender node coverage needs more sets (the context is thread-local, so this is stack).
inline constexpr uint32_t kMaxUvSets = 2;

struct ShadeContext {
  Real3 P, Ng, N;       // position, geometric normal, shading normal (world space)
  Real3 wo;             // toward the viewer; inside NEE, toward the shading point (item 33)
  Real2 uv[kMaxUvSets];
  uint32_t front_face;  // 1 when the hit is on the front side of Ng
  uint32_t object_id, instance_id;
  uint32_t ray_type;    // ray-type flags (Light Path)
  uint32_t depth[6];    // total, diffuse, glossy, transmission, volume, transparent
  Lambda lambda_t;      // the path's transport wavelengths
};

// The image table is indexed by the slot the compiler resolved from an image key (the image
// store's dense slots); a slot at or past image_count samples magenta. The kernel never searches.
// srgb_decode is the host-built srgb_decode_table (color/srgb.h) Closest lookups read.
struct SceneTables {
  const Rgb2SpecTable* rgb2spec;
  ObserverView observer;
  const DeviceImageView* images;
  uint32_t image_count;
  const float* srgb_decode;
};

AR_NS_END
