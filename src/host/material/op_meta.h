// SPDX-License-Identifier: Apache-2.0
// Per-opcode compiler metadata generated from the same ops.def as the enum.
// Ref: docs/decisions/0005-material-model.md item 30.
#pragma once

#include <cstdint>

#include "../../kernel/vm/ops.h"

namespace astroray::host {

enum class CostClass : uint8_t { CHEAP, MEM, TEX };

struct OpMeta {
  ar::Op op;
  const char* name;
  int arity;
  const char* operand_types;  // '+' separated, "" for none
  const char* result_type;    // "f32", "f32x3", "spec" or ""
  CostClass cost;
  const char* interval_rule;  // function name, or nullptr for NONE_SURFACE_ONLY
  bool volume_reachable;
  bool test_only;
};

// One entry per opcode, indexed by the opcode value.
const OpMeta* op_meta_table();
int op_meta_count();
const OpMeta& op_meta(ar::Op op);

}  // namespace astroray::host
