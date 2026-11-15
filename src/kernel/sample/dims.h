// SPDX-License-Identifier: Apache-2.0
// The fixed dimension map of stream 0 and the variable-count stream ids. Each
// 4-D block (dims 4b .. 4b+3) is jointly stratified, so the blocks below are
// the unit a consumer may draw from together. Dims 0-7 are per path kind; the
// per-vertex block starts at 8 + 12e for event index e.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d23 (dimension map,
// variable-count streams), d2 (film wavelength), d6 (hero lane from its own dimension).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN

// Camera path, dims 0-7.
inline constexpr uint32_t DIM_CAM_FILTER_X = 0;
inline constexpr uint32_t DIM_CAM_FILTER_Y = 1;
inline constexpr uint32_t DIM_CAM_LENS_U = 2;
inline constexpr uint32_t DIM_CAM_LENS_V = 3;
inline constexpr uint32_t DIM_CAM_TIME = 4;
inline constexpr uint32_t DIM_CAM_WAVELENGTH = 5;
inline constexpr uint32_t DIM_CAM_HERO = 6;
inline constexpr uint32_t DIM_CAM_UNUSED7 = 7;

// Light path, dims 0-7.
inline constexpr uint32_t DIM_LIGHT_PICK = 0;
inline constexpr uint32_t DIM_LIGHT_POS_U = 1;
inline constexpr uint32_t DIM_LIGHT_POS_V = 2;
inline constexpr uint32_t DIM_LIGHT_TIME = 3;
inline constexpr uint32_t DIM_LIGHT_DIR_U = 4;
inline constexpr uint32_t DIM_LIGHT_DIR_V = 5;
inline constexpr uint32_t DIM_LIGHT_WAVELENGTH = 6;
inline constexpr uint32_t DIM_LIGHT_HERO = 7;

// Per-event block: 12 dimensions from 8 + 12e; offsets within it. Camera and
// light paths share offsets 0-3 and 8-11; offsets 4-7 differ as named.
inline constexpr uint32_t DIM_EVENT_FIRST = 8;
inline constexpr uint32_t DIM_EVENT_COUNT = 12;
inline constexpr uint32_t EV_BSDF_U = 0;  // BSDF or phase direction u, v
inline constexpr uint32_t EV_BSDF_V = 1;
inline constexpr uint32_t EV_LOBE_CHOICE = 2;  // lobe, Mix and opacity choice
inline constexpr uint32_t EV_ROULETTE = 3;     // Russian roulette
inline constexpr uint32_t EV_LIGHT_U = 4;      // camera path: light position u, v
inline constexpr uint32_t EV_LIGHT_V = 5;
inline constexpr uint32_t EV_LIGHT_PICK = 6;  // light pick including the floor branch
inline constexpr uint32_t EV_DIST_CHOICE = 7;  // distance-strategy choice (both kinds)
inline constexpr uint32_t EV_CONN_LENS_U = 4;  // light path: the connection's lens sample
inline constexpr uint32_t EV_CONN_LENS_V = 5;
inline constexpr uint32_t EV_LIGHT_PATH_UNUSED6 = 6;
inline constexpr uint32_t EV_EXT_CHOICE = 8;   // grating order, fluorescent event
inline constexpr uint32_t EV_EXT_VALUE = 9;    // extension value (lambda_i)
inline constexpr uint32_t EV_EQUIANGULAR = 10;  // equiangular distance
inline constexpr uint32_t EV_MARCHER_OFFSET = 11;  // marcher first sub-step offset

// Variable-count streams: one 4-D block per draw, block index = a stored counter.
inline constexpr uint32_t STREAM_FIXED = 0;
inline constexpr uint32_t STREAM_PATH_MEDIUM = 1;    // path medium_rng
inline constexpr uint32_t STREAM_SHADOW_MEDIUM = 2;  // item medium_rng, starts at e * 2^17
inline constexpr uint32_t STREAM_RANDOM_WALK = 3;    // walk step counter + e * 2^16

// First dimension of event e's block, plus `offset` in 0..11.
AR_FN uint32_t event_dim(uint32_t e, uint32_t offset) {
  AR_ASSERT(offset < DIM_EVENT_COUNT);
  return DIM_EVENT_FIRST + DIM_EVENT_COUNT * e + offset;
}

// Dimension of lane `lane` of block `block_counter` in a variable-count stream.
AR_FN uint32_t stream_dim(uint32_t block_counter, uint32_t lane) {
  AR_ASSERT(lane < 4u);
  return 4u * block_counter + lane;
}

AR_NS_END
