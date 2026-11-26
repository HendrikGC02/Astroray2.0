// SPDX-License-Identifier: Apache-2.0
// Path flag word (u32): one named constant per bit of the record, no other bit, and the shadow
// item's item_flags bits (u8, d20) that are allocated. Assigning a free bit amends the record.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d20, d21.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

inline constexpr uint32_t SECONDARIES_TERMINATED = 1u << 0;
inline constexpr uint32_t WAVELENGTHS_REWRITTEN = 1u << 1;
inline constexpr uint32_t LAST_BOUNCE_SPECULAR = 1u << 2;
inline constexpr uint32_t AOV_WRITTEN = 1u << 3;
inline constexpr uint32_t LAST_SEGMENT_CURVED = 1u << 4;
inline constexpr uint32_t IN_CURVED_REGION = 1u << 5;
inline constexpr uint32_t PATH_KIND = 1u << 6;  // 0 camera, 1 light

// Bits 7-8: hero lane.
inline constexpr uint32_t HERO_LANE_SHIFT = 7;
inline constexpr uint32_t HERO_LANE_MASK = 3u << HERO_LANE_SHIFT;

// Bits 9-11: light-pass partition.
inline constexpr uint32_t PARTITION_ARMED = 1u << 9;
inline constexpr uint32_t PARTITION_CHAIN_NON_EMPTY = 1u << 10;
inline constexpr uint32_t PARTITION_CASTER_BIT = 1u << 11;

// Bits 12-14: medium-stack depth.
inline constexpr uint32_t MEDIUM_DEPTH_SHIFT = 12;
inline constexpr uint32_t MEDIUM_DEPTH_MASK = 7u << MEDIUM_DEPTH_SHIFT;

inline constexpr uint32_t DENOISE_SURFACE_DONE = 1u << 15;

// Bits 16-17: first-vertex class (pending, surface, volume).
inline constexpr uint32_t FIRST_VERTEX_CLASS_SHIFT = 16;
inline constexpr uint32_t FIRST_VERTEX_CLASS_MASK = 3u << FIRST_VERTEX_CLASS_SHIFT;

inline constexpr uint32_t SHADOW_CATCHER_HIT = 1u << 18;
inline constexpr uint32_t SHADOW_CATCHER_PASS = 1u << 19;
inline constexpr uint32_t FRONT_FACE = 1u << 20;
inline constexpr uint32_t FIRST_BOUNCE_SPECULAR = 1u << 21;

// Shadow item_flags bit 4 (0003 d20): the contribution still lacks the environment world's radiance
// along ray_d at the item's lambda_t, which shade_shadow evaluates on delivery (light/world_env.h).
inline constexpr uint8_t ITEM_WORLD_LE = uint8_t(1u << 4);

// Shadow item_flags bits 2-3 (0003 d20): the emitting path's hero lane, which ratio tracking draws
// its distances at (0006 D8).
inline constexpr uint32_t ITEM_HERO_LANE_SHIFT = 2;
inline constexpr uint32_t ITEM_HERO_LANE_MASK = 3u << ITEM_HERO_LANE_SHIFT;

AR_FN uint32_t hero_lane(uint32_t flags) { return (flags & HERO_LANE_MASK) >> HERO_LANE_SHIFT; }

// Returns the flags with the hero lane replaced; lane must be 0..3.
AR_FN uint32_t set_hero_lane(uint32_t flags, uint32_t lane) {
  AR_ASSERT(lane < 4u);
  return (flags & ~HERO_LANE_MASK) | ((lane << HERO_LANE_SHIFT) & HERO_LANE_MASK);
}

AR_FN uint32_t item_hero_lane(uint32_t item_flags) { return (item_flags & ITEM_HERO_LANE_MASK) >> ITEM_HERO_LANE_SHIFT; }

// item_flags bits 2-3 holding lane 0..3.
AR_FN uint32_t item_hero_bits(uint32_t lane) {
  AR_ASSERT(lane < 4u);
  return (lane << ITEM_HERO_LANE_SHIFT) & ITEM_HERO_LANE_MASK;
}

AR_FN uint32_t medium_depth(uint32_t flags) { return (flags & MEDIUM_DEPTH_MASK) >> MEDIUM_DEPTH_SHIFT; }

// Returns the flags with the medium-stack depth replaced; depth must be 0..7.
AR_FN uint32_t set_medium_depth(uint32_t flags, uint32_t depth) {
  AR_ASSERT(depth < 8u);
  return (flags & ~MEDIUM_DEPTH_MASK) | ((depth << MEDIUM_DEPTH_SHIFT) & MEDIUM_DEPTH_MASK);
}

AR_NS_END
