// SPDX-License-Identifier: Apache-2.0
// Opcode enum generated from ops.def, plus the dispatch include point.
// The interpreter (057) builds its switch from the same table:
//   switch (op) {
//   #define AR_OP(NAME, ...) AR_OP_CASE(NAME, handle_##NAME)
//   #include "vm/ops.def"
//   #undef AR_OP
//   }
// so a new row cannot be missed by the switch. Ids are the row order, dense.
// Ref: docs/decisions/0005-material-model.md item 30.
#pragma once

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN
enum class Op : unsigned char {
#define AR_OP(NAME, ...) NAME,
#include "ops.def"
#undef AR_OP
  COUNT_
};
AR_NS_END

#define AR_OP_CASE(NAME, HANDLER) \
  case ::ar::Op::NAME: HANDLER(); break;
