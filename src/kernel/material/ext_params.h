// SPDX-License-Identifier: Apache-2.0
// ExtParams: the per-material constants of an extension kind, and the closed kind list.
// Ref: docs/decisions/0005-material-model.md items 14 (kinds), 16 (64 B block at ext_index;
// ext_fields.def), 23 (what the grating's block must hold).
#pragma once
#include <cstdint>

#include "../portability.h"

AR_NS_BEGIN

// The ids come from ext_fields.def, the one place they are written.
enum class ExtKind : uint32_t {
#define AR_EXT_KIND(name, id) name = id,
#include "ext_fields.def"
};

// 16 x 32-bit, PROVISIONAL (item 16). The layout of the words is each kind's (the grating's is
// grating lobe's); ar_material_desc::ext_constants carries this block byte for byte.
struct ExtParams {
  uint32_t w[16];
};

static_assert(sizeof(ExtParams) == 64, "ExtParams is 64 B (0005 item 16)");
static_assert(alignof(ExtParams) == 4, "ExtParams is 16 u32 words");

AR_NS_END
