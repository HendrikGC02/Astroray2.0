// SPDX-License-Identifier: Apache-2.0
// The single termination-reason enum that every counter and the statistics
// pass read. Values are the order of the record; a new reason is appended by an
// amendment of the record, never inserted.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d22.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

enum class Term : uint8_t {
  ACTIVE,
  ESCAPED,
  RUSSIAN_ROULETTE,
  ZERO_THROUGHPUT,
  MAX_BOUNCE,
  TRANSPARENT_LIMIT,
  ABSORBED,
  VOLUME_LIMIT,
  STACK_OVERFLOW,
  INVALID,
  CAPTURED,
  GEODESIC_STEP_LIMIT,
  GEODESIC_NUMERICAL,
  LIGHT_PATH_IN_REGION,
  LIGHT_PATH_END,
  CANCELLED,
};

AR_NS_END
