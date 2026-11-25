// SPDX-License-Identifier: Apache-2.0
// Path state and shadow item as structures of arrays: one pointer per field,
// generated from path_state.def and shadow_item.def. A field whose feature is off
// has a null pointer and no allocation. The byte counts below are at the build's
// Real; the budgets of 0003 d19-d20 are stated at Real = float.
// The ps_/sh_ accessors need KernelGlobals and live in kernel_globals.h.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d19, d20, Verification
// check 13; docs/decisions/0002-kernel-architecture.md D13 (one X-macro list, one
// array per field, fields of an off feature not allocated); Cycles
// kernel/integrator/state_template.h (Apache-2.0).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../portability.h"

AR_NS_BEGIN

// Array-valued fields are one struct per element so that each field is one array.
struct U16x6 {
  uint16_t v[6];
};
struct U32x4 {
  uint32_t v[4];
};
// unorm16 values stored as their uint16_t codes.
struct Unorm16x8 {
  uint16_t v[8];
};
static_assert(sizeof(U16x6) == 12 && sizeof(U32x4) == 16 && sizeof(Unorm16x8) == 16, "array field widths");

// The feature tags of both tables: the condition columns of 0003 d19 and d20.
// GUIDES_OR_LIGHTPASS and LIGHTPASS name 0008 D10's render passes (d19, "light
// passes"); LIGHTPASS_OR_MEDIA names 0004 D20's light-tracing pass (d20, "light
// pass"). They are different facts, see StateFeatures. The _OR_ENV tags add a baked environment
// world (0004 D11): its NEE items take the world's radiance at delivery from their lanes and an
// item_flags bit.
enum class StateFeature : uint8_t {
  ALWAYS,
  MOTION,
  CURVED_OR_SUBSURFACE,
  MEDIA,
  FILTER_GLOSSY,
  GUIDES_OR_LIGHTPASS,
  LIGHTPASS,
  STAT_TIME,
  STAT_RAYS,
  LIGHTPASS_OR_MEDIA,
  NON_OPAQUE,
  LIGHT_GROUPS,
  MEDIA_NON_OPAQUE_OR_ENV,
  LIGHTPASS_MEDIA_OR_ENV,
};

// The scene and settings facts the tags derive from. `media` is d19's "any medium
// (incl. world volume and transmission_depth > 0) or >= 2 transmissive materials";
// the host computes it, this header only combines the facts.
struct StateFeatures {
  bool motion = false;
  bool curved = false;
  bool subsurface = false;
  bool media = false;
  bool filter_glossy = false;
  bool guides = false;
  bool light_render_passes = false;  // 0008 D10 render passes (d19 rows 28-29: "light passes")
  bool light_tracing = false;        // 0004 D20 light-tracing pass, active when casters exist (d20: "light pass")
  bool stat_time = false;
  bool stat_rays = false;
  bool non_opaque = false;
  bool light_groups = false;
  bool env_world = false;  // the world is a WORLD_ENV light (a baked environment table, 0004 D11)
};

constexpr StateFeatures all_features_on() {
  StateFeatures f;
  f.motion = f.curved = f.subsurface = f.media = f.filter_glossy = f.guides = true;
  f.light_render_passes = f.light_tracing = f.stat_time = f.stat_rays = f.non_opaque = f.light_groups = true;
  f.env_world = true;
  return f;
}

constexpr bool feature_on(const StateFeatures& f, StateFeature t) {
  switch (t) {
    case StateFeature::ALWAYS: return true;
    case StateFeature::MOTION: return f.motion;
    case StateFeature::CURVED_OR_SUBSURFACE: return f.curved || f.subsurface;
    case StateFeature::MEDIA: return f.media;
    case StateFeature::FILTER_GLOSSY: return f.filter_glossy;
    case StateFeature::GUIDES_OR_LIGHTPASS: return f.guides || f.light_render_passes;
    case StateFeature::LIGHTPASS: return f.light_render_passes;
    case StateFeature::STAT_TIME: return f.stat_time;
    case StateFeature::STAT_RAYS: return f.stat_rays;
    case StateFeature::LIGHTPASS_OR_MEDIA: return f.light_tracing || f.media;
    case StateFeature::NON_OPAQUE: return f.non_opaque;
    case StateFeature::LIGHT_GROUPS: return f.light_groups;
    case StateFeature::MEDIA_NON_OPAQUE_OR_ENV: return f.media || f.non_opaque || f.env_world;
    case StateFeature::LIGHTPASS_MEDIA_OR_ENV: return f.light_tracing || f.media || f.env_world;
  }
  return false;
}

// One row per field, in table order. bytes_no_media differs from bytes only for
// the shared r_l_film_pos of the shadow item.
struct StateField {
  const char* name;
  uint32_t bytes;
  uint32_t bytes_no_media;
  StateFeature feature;
};

inline constexpr StateField kPathStateFields[] = {
#define AR_PATH_FIELD(n, t, f) {#n, sizeof(t), sizeof(t), StateFeature::f},
#include "path_state.def"
#undef AR_PATH_FIELD
};
inline constexpr uint32_t kPathStateFieldCount = sizeof(kPathStateFields) / sizeof(kPathStateFields[0]);

inline constexpr StateField kShadowItemFields[] = {
#define AR_SHADOW_FIELD(n, t, f) {#n, sizeof(t), sizeof(t), StateFeature::f},
#define AR_SHADOW_FIELD_SHARED(n, t, f, nt) {#n, sizeof(t), sizeof(nt), StateFeature::f},
#include "shadow_item.def"
#undef AR_SHADOW_FIELD
#undef AR_SHADOW_FIELD_SHARED
};
inline constexpr uint32_t kShadowItemFieldCount = sizeof(kShadowItemFields) / sizeof(kShadowItemFields[0]);

// Bytes per path with the enabled fields only (the per-field arrays carry no padding).
constexpr uint32_t path_state_bytes(const StateFeatures& f) {
  uint32_t b = 0;
  for (const StateField& x : kPathStateFields)
    if (feature_on(f, x.feature)) b += x.bytes;
  return b;
}

constexpr uint32_t shadow_item_bytes(const StateFeatures& f) {
  uint32_t b = 0;
  for (const StateField& x : kShadowItemFields)
    if (feature_on(f, x.feature)) b += f.media ? x.bytes : x.bytes_no_media;
  return b;
}

// Per-pool byte budgets of 0003 d19 and d20 (ruling 9), at Real = float. The record's
// totals (157, 225, 124) are pinned by tests/cpu/test_state_layout.cpp; a double
// build is not budgeted.
inline constexpr uint32_t kPathStateBudgetBytes = 256;
inline constexpr uint32_t kShadowItemBudgetBytes = 128;
#if !defined(AR_REAL_DOUBLE)
static_assert(path_state_bytes(all_features_on()) <= kPathStateBudgetBytes, "path state over its 256 B budget");
static_assert(shadow_item_bytes(all_features_on()) <= kShadowItemBudgetBytes, "shadow item over its 128 B budget");
#endif

struct PathStateSoA {
#define AR_PATH_FIELD(n, t, f) t* n;
#include "path_state.def"
#undef AR_PATH_FIELD
};

// r_l_film_pos is a Spec array with media and a Real2 array without (see
// shadow_item.def); kernel_globals.h has its two accessors.
struct ShadowItemSoA {
#define AR_SHADOW_FIELD(n, t, f) t* n;
#define AR_SHADOW_FIELD_SHARED(n, t, f, nt) t* n;
#include "shadow_item.def"
#undef AR_SHADOW_FIELD
#undef AR_SHADOW_FIELD_SHARED
};

AR_NS_END
