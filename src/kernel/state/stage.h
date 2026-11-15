// SPDX-License-Identifier: Apache-2.0
// The next_stage values, in launch order (the scheduler launches every non-empty
// stage once per step in this order), and the flat shade-kernel identities.
// SHADE_HAIR and SHADE_LIGHT_PATH_HAIR exist only if 0005's hair split is adopted,
// and ShadeKind::Hair and LightHair with them; they are omitted until then.
// generate_camera, generate_light and the partition and sort kernels are not
// Stage values.
// Ref: docs/decisions/0002-kernel-architecture.md D8 (Stage enum, ShadeKind), D5.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

enum class Stage : uint8_t {
  INTERSECT_CLOSEST,
  INTERSECT_SUBSURFACE,
  INTERSECT_SHADOW,  // shadow pool
  SHADE_BACKGROUND,
  SHADE_LIGHT,
  SHADE_SURFACE,
  SHADE_SURFACE_EXT,
  SHADE_LIGHT_PATH,
  SHADE_LIGHT_PATH_EXT,
  SHADE_SHADOW,  // shadow pool
  SHADE_VOLUME,
  SHADE_SUBSURFACE,
  ADVANCE_GEODESIC,
  TERMINATED,         // main pool
  SHADOW_TERMINATED,  // shadow pool
};

inline constexpr uint32_t kStageCount = static_cast<uint32_t>(Stage::SHADOW_TERMINATED) + 1u;

enum class ShadeKind : uint8_t { Common, Ext, Light, LightExt };

AR_NS_END
