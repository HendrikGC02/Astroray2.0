// SPDX-License-Identifier: Apache-2.0
// Sizes of the path-state and shadow-item arrays for a feature set and a capacity:
// per-field bytes, 256-B aligned offsets and totals. Computes sizes only; the
// allocation itself goes through the VRAM ledger (026).
// Ref: docs/decisions/0002-kernel-architecture.md D13 (each field its own array,
// 256-B aligned, a field of an off feature not allocated); 0003 d19-d20.
#pragma once
#include <cstdint>
#include <vector>

#include "kernel/state/state.h"

namespace astroray::host {

inline constexpr uint64_t kStateArrayAlign = 256;

struct FieldAlloc {
  const char* name;
  bool allocated;
  uint32_t element_bytes;  // 0 when the field is not allocated
  uint64_t bytes;          // element_bytes * capacity; 0 when not allocated
  uint64_t offset;         // multiple of kStateArrayAlign; 0 when not allocated
};

struct StateLayout {
  std::vector<FieldAlloc> fields;  // one per row of the table, in table order
  uint64_t capacity = 0;
  uint32_t bytes_per_slot = 0;     // sum of element_bytes: bytes per path or per item
  uint64_t total_bytes = 0;        // end of the last array, rounded up to kStateArrayAlign
};

// Throws std::invalid_argument for a zero capacity.
StateLayout path_state_layout(const ar::StateFeatures& features, uint64_t capacity);
StateLayout shadow_item_layout(const ar::StateFeatures& features, uint64_t capacity);

}  // namespace astroray::host
