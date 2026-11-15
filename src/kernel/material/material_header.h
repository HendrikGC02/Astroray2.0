// SPDX-License-Identifier: Apache-2.0
// MaterialHeader: the 16 B per-material record in device memory.
// Ref: docs/decisions/0005-material-model.md item 2(a) (fields), item 14 (ext_kind list),
// item 43 (flag meanings).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

struct MaterialHeader {
  uint32_t program_offset;  // the program starts with an entry table (item 32)
  uint32_t const_offset;    // constant block
  uint32_t ext;             // ext_kind u8 (bits 0-7) | ext_index u24 (bits 8-31)
  uint32_t flags;           // the bits below
};

static_assert(sizeof(MaterialHeader) == 16, "MaterialHeader is 16 B (0005 item 2)");

// Item 43. One named constant per bit.
inline constexpr uint32_t MAY_BE_DELTA = 1u << 0;
inline constexpr uint32_t NON_OPAQUE = 1u << 1;
inline constexpr uint32_t EXTENSION = 1u << 2;
inline constexpr uint32_t READS_RAY_TYPE = 1u << 3;
inline constexpr uint32_t CONSTANT_SURFACE = 1u << 4;
inline constexpr uint32_t CONSTANT_EMISSION = 1u << 5;

inline constexpr uint32_t kExtIndexMax = (1u << 24) - 1u;

AR_FN uint32_t ext_pack(uint32_t kind, uint32_t index) { return (kind & 0xffu) | ((index & kExtIndexMax) << 8); }
AR_FN uint32_t ext_kind(const MaterialHeader& h) { return h.ext & 0xffu; }
AR_FN uint32_t ext_index(const MaterialHeader& h) { return h.ext >> 8; }

AR_NS_END
