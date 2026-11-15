// SPDX-License-Identifier: Apache-2.0
#include "host/state_alloc.h"

#include <stdexcept>

namespace astroray::host {
namespace {

StateLayout layout_of(const ar::StateField* fields, uint32_t count, const ar::StateFeatures& features,
                      uint64_t capacity, bool shadow) {
  if (capacity == 0) throw std::invalid_argument("state layout: capacity must be positive");
  StateLayout out;
  out.capacity = capacity;
  uint64_t cursor = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const ar::StateField& f = fields[i];
    FieldAlloc a{f.name, false, 0, 0, 0};
    if (ar::feature_on(features, f.feature)) {
      a.allocated = true;
      a.element_bytes = (shadow && !features.media) ? f.bytes_no_media : f.bytes;
      a.bytes = a.element_bytes * capacity;
      a.offset = cursor;
      cursor = (cursor + a.bytes + kStateArrayAlign - 1) / kStateArrayAlign * kStateArrayAlign;
      out.bytes_per_slot += a.element_bytes;
    }
    out.fields.push_back(a);
  }
  out.total_bytes = cursor;
  return out;
}

}  // namespace

StateLayout path_state_layout(const ar::StateFeatures& features, uint64_t capacity) {
  return layout_of(ar::kPathStateFields, ar::kPathStateFieldCount, features, capacity, false);
}

StateLayout shadow_item_layout(const ar::StateFeatures& features, uint64_t capacity) {
  return layout_of(ar::kShadowItemFields, ar::kShadowItemFieldCount, features, capacity, true);
}

}  // namespace astroray::host
